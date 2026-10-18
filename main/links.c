#include "links.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "ble_server.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pipeline.h"

static const char *TAG = "links";

#define TICK_MS(ms) pdMS_TO_TICKS(ms)
#define RETRY_DELAY TICK_MS(5000)
#define DIAL_TIMEOUT TICK_MS(30000)
#define SCAN_LEN 8 // x 1.28 s
#define SCAN_SEEN_MAX 24
#define SCAN_NAME_LOOKUPS_MAX 8 // remote name requests after an inquiry, ~1-5 s each
#define NAME_LOOKUP_TIMEOUT TICK_MS(10000)
#define SPP_UUID16 0x1101
#define DEFAULT_PIN "0000"

typedef enum { LINK_IDLE, LINK_DISCOVERING, LINK_CONNECTING, LINK_CONNECTED } link_state_t;

typedef struct {
    bridge_device_t device;
    link_state_t state;
    uint32_t handle;
    bool closing; // disconnect requested; state stays CONNECTED until ESP_SPP_CLOSE_EVT
    TickType_t next_try;
    char name[32];
} link_t;

static link_t s_links[BRIDGE_SLOT_COUNT];
static links_data_cb s_on_data;
static SemaphoreHandle_t s_lock;

// Only one SDP discovery / connect is in flight at a time, because
// ESP_SPP_DISCOVERY_COMP_EVT does not say which peer it belongs to.
static int s_dialing = -1;
static TickType_t s_dial_started;

static bool s_scan_pending;
static bool s_scan_running;
static bridge_reply_fn s_scan_reply;
// Devices seen by the last scan, with the best name known; also feeds `paired`.
typedef struct {
    esp_bd_addr_t bda;
    char name[32];
} scan_seen_t;
static scan_seen_t s_scan_seen[SCAN_SEEN_MAX];
static int s_scan_seen_count;
static int s_name_next = -1; // >= 0 while looking up names after the inquiry
static int s_name_lookups;
static TickType_t s_name_started;

static void name_lookup_next(void);

static void lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_lock); }

static int slot_by_handle(uint32_t handle)
{
    for (int i = 0; i < BRIDGE_SLOT_COUNT; i++) {
        if (s_links[i].state == LINK_CONNECTED && s_links[i].handle == handle) {
            return i;
        }
    }
    return -1;
}

static int slot_by_mac(const esp_bd_addr_t mac)
{
    for (int i = 0; i < BRIDGE_SLOT_COUNT; i++) {
        if (s_links[i].device.valid && memcmp(s_links[i].device.mac, mac, ESP_BD_ADDR_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

static void mark_idle(int slot)
{
    s_links[slot].state = LINK_IDLE;
    s_links[slot].handle = 0;
    s_links[slot].closing = false;
    s_links[slot].next_try = xTaskGetTickCount() + RETRY_DELAY;
}

static void request_close(int slot)
{
    if (s_links[slot].state == LINK_CONNECTED && !s_links[slot].closing) {
        s_links[slot].closing = true;
        esp_spp_disconnect(s_links[slot].handle);
    }
}

static void start_scan_if_pending(void)
{
    if (!s_scan_pending || s_dialing >= 0) {
        return;
    }
    s_scan_pending = false;
    s_scan_running = true;
    s_scan_seen_count = 0;
    s_name_next = -1;
    s_name_lookups = 0;
    if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, SCAN_LEN, 0) != ESP_OK) {
        s_scan_running = false;
        if (s_scan_reply) {
            s_scan_reply("error scan failed");
        }
    }
}

static void finish_dial(void)
{
    s_dialing = -1;
    start_scan_if_pending();
}

static void dial_next(void)
{
    if (s_dialing >= 0 || s_scan_pending || s_scan_running) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    for (int i = 0; i < BRIDGE_SLOT_COUNT; i++) {
        link_t *link = &s_links[i];
        if (!link->device.valid || link->state != LINK_IDLE || (int32_t)(now - link->next_try) < 0) {
            continue;
        }
        link->state = LINK_DISCOVERING;
        s_dialing = i;
        s_dial_started = now;
        ESP_LOGI(TAG, "%s: discovering " ESP_BD_ADDR_STR, bridge_config_slot_name(i),
                 ESP_BD_ADDR_HEX(link->device.mac));
        if (esp_spp_start_discovery(link->device.mac) != ESP_OK) {
            mark_idle(i);
            finish_dial();
        }
        return;
    }
}

static void manager_task(void *arg)
{
    for (;;) {
        lock();
        if (s_dialing >= 0 && xTaskGetTickCount() - s_dial_started > DIAL_TIMEOUT) {
            ESP_LOGW(TAG, "%s: dial timed out", bridge_config_slot_name(s_dialing));
            mark_idle(s_dialing);
            finish_dial();
        }
        if (s_name_next >= 0 && xTaskGetTickCount() - s_name_started > NAME_LOOKUP_TIMEOUT) {
            ESP_LOGW(TAG, "remote name request timed out");
            name_lookup_next();
        }
        dial_next();
        unlock();
        vTaskDelay(TICK_MS(1000));
    }
}

static scan_seen_t *seen_find(const esp_bd_addr_t bda)
{
    for (int i = 0; i < s_scan_seen_count; i++) {
        if (memcmp(s_scan_seen[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
            return &s_scan_seen[i];
        }
    }
    return NULL;
}

static const char *cod_class(uint32_t cod)
{
    static const char *NAMES[] = { "misc", "computer", "phone", "lan", "av", "peripheral",
                                   "imaging", "wearable", "toy", "health" };
    uint32_t major = esp_bt_gap_get_cod_major_dev(cod);
    if (major < sizeof(NAMES) / sizeof(NAMES[0])) {
        return NAMES[major];
    }
    return major == ESP_BT_COD_MAJOR_DEV_UNCATEGORIZED ? "uncategorized" : "other";
}

// Whether the EIR lists the Serial Port service. Many SPP devices send no EIR
// or leave it out, so absence means "unknown", not "no".
static bool eir_has_spp(uint8_t *eir)
{
    static const uint8_t TYPES[] = { ESP_BT_EIR_TYPE_CMPL_16BITS_UUID, ESP_BT_EIR_TYPE_INCMPL_16BITS_UUID };
    for (size_t t = 0; t < sizeof(TYPES); t++) {
        uint8_t len = 0;
        uint8_t *uuids = esp_bt_gap_resolve_eir_data(eir, TYPES[t], &len);
        for (int i = 0; uuids && i + 1 < len; i += 2) {
            if ((uuids[i] | (uuids[i + 1] << 8)) == SPP_UUID16) {
                return true;
            }
        }
    }
    return false;
}

static void scan_result(esp_bt_gap_cb_param_t *param)
{
    if (!s_scan_running || s_name_next >= 0 || seen_find(param->disc_res.bda)) {
        return;
    }
    scan_seen_t *entry = NULL;
    if (s_scan_seen_count < SCAN_SEEN_MAX) {
        entry = &s_scan_seen[s_scan_seen_count++];
        memcpy(entry->bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
        entry->name[0] = '\0';
    }

    char name[32] = "";
    int rssi = 0;
    uint32_t cod = 0;
    bool spp = false;
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];
        if (prop->type == ESP_BT_GAP_DEV_PROP_RSSI) {
            rssi = *(int8_t *)prop->val;
        } else if (prop->type == ESP_BT_GAP_DEV_PROP_COD) {
            cod = *(uint32_t *)prop->val;
        } else if (prop->type == ESP_BT_GAP_DEV_PROP_BDNAME && !name[0]) {
            int len = prop->len < (int)sizeof(name) - 1 ? prop->len : (int)sizeof(name) - 1;
            memcpy(name, prop->val, len);
            name[len] = '\0';
        } else if (prop->type == ESP_BT_GAP_DEV_PROP_EIR) {
            spp = eir_has_spp(prop->val);
            if (!name[0]) {
                uint8_t len = 0;
                uint8_t *eir_name = esp_bt_gap_resolve_eir_data(prop->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
                if (!eir_name) {
                    eir_name = esp_bt_gap_resolve_eir_data(prop->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
                }
                if (eir_name) {
                    len = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
                    memcpy(name, eir_name, len);
                    name[len] = '\0';
                }
            }
        }
    }
    if (entry) {
        strlcpy(entry->name, name, sizeof(entry->name));
    }
    char line[96];
    snprintf(line, sizeof(line), "found " ESP_BD_ADDR_STR " %d %06" PRIx32 " %s %s %s",
             ESP_BD_ADDR_HEX(param->disc_res.bda), rssi, cod & 0xffffff, cod_class(cod), spp ? "spp" : "-", name);
    if (s_scan_reply) {
        s_scan_reply(line);
    }
}

static void scan_finish(void)
{
    s_scan_running = false;
    s_name_next = -1;
    if (s_scan_reply) {
        s_scan_reply("scan done");
    }
    s_scan_reply = NULL;
}

// After the inquiry, asks nameless devices for their names one at a time,
// then reports "scan done". Links stay down until then.
static void name_lookup_next(void)
{
    while (s_name_next >= 0 && s_name_next < s_scan_seen_count) {
        scan_seen_t *entry = &s_scan_seen[s_name_next++];
        if (entry->name[0] || s_name_lookups >= SCAN_NAME_LOOKUPS_MAX) {
            continue;
        }
        if (esp_bt_gap_read_remote_name(entry->bda) == ESP_OK) {
            s_name_lookups++;
            s_name_started = xTaskGetTickCount();
            return;
        }
    }
    scan_finish();
}

static void name_result(esp_bt_gap_cb_param_t *param)
{
    if (!s_scan_running || s_name_next < 0) {
        return;
    }
    scan_seen_t *entry = seen_find(param->read_rmt_name.bda);
    if (entry && param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS && param->read_rmt_name.rmt_name[0]) {
        strlcpy(entry->name, (const char *)param->read_rmt_name.rmt_name, sizeof(entry->name));
        char line[80];
        snprintf(line, sizeof(line), "name " ESP_BD_ADDR_STR " %s", ESP_BD_ADDR_HEX(entry->bda), entry->name);
        if (s_scan_reply) {
            s_scan_reply(line);
        }
    }
    name_lookup_next();
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    lock();
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        scan_result(param);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && s_scan_running && s_name_next < 0) {
            s_name_next = 0;
            name_lookup_next();
        }
        break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        name_result(param);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        int slot = slot_by_mac(param->auth_cmpl.bda);
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "paired with %s", param->auth_cmpl.device_name);
            if (slot >= 0) {
                strlcpy(s_links[slot].name, (const char *)param->auth_cmpl.device_name,
                        sizeof(s_links[slot].name));
            }
        } else {
            ESP_LOGE(TAG, "pairing failed, status %d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {
        int slot = slot_by_mac(param->pin_req.bda);
        const char *text = slot >= 0 && s_links[slot].device.pin[0] ? s_links[slot].device.pin : DEFAULT_PIN;
        esp_bt_pin_code_t pin = {0};
        size_t len = strlen(text);
        memcpy(pin, text, len);
        ESP_LOGI(TAG, "legacy PIN requested");
        esp_bt_gap_pin_reply(param->pin_req.bda, true, len, pin);
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    default:
        break;
    }
    unlock();
}

static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    // Data is delivered outside the lock so the BLE notify path never waits on it.
    if (event == ESP_SPP_DATA_IND_EVT) {
        lock();
        int slot = slot_by_handle(param->data_ind.handle);
        if (slot >= 0 && s_links[slot].closing) {
            slot = -1;
        }
        unlock();
        if (slot >= 0 && s_on_data) {
            s_on_data((bridge_slot_t)slot, param->data_ind.data, param->data_ind.len);
        }
        return;
    }

    // Pipeline calls happen after unlock(): its pump calls links_write() under its own lock.
    int link_up = -1, link_down = -1, write_slot = -1, uncongested = -1;
    bool write_ok = false, write_cong = false;
    char name[sizeof(s_links[0].name)] = "";

    lock();
    switch (event) {
    case ESP_SPP_INIT_EVT:
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        xTaskCreate(manager_task, "links", 3072, NULL, 5, NULL);
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT: {
        int slot = s_dialing;
        if (slot < 0 || s_links[slot].state != LINK_DISCOVERING) {
            break;
        }
        if (param->disc_comp.status == ESP_SPP_SUCCESS && param->disc_comp.scn_num > 0) {
            s_links[slot].state = LINK_CONNECTING;
            esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_MASTER, param->disc_comp.scn[0],
                            s_links[slot].device.mac);
        } else {
            mark_idle(slot);
            finish_dial();
        }
        break;
    }
    case ESP_SPP_OPEN_EVT: {
        int slot = s_dialing;
        bool wanted = slot >= 0 && s_links[slot].state == LINK_CONNECTING &&
                      memcmp(s_links[slot].device.mac, param->open.rem_bda, ESP_BD_ADDR_LEN) == 0;
        if (param->open.status != ESP_SPP_SUCCESS) {
            if (slot >= 0) {
                mark_idle(slot);
            }
        } else if (!wanted) {
            // The slot was re-bound while dialling the old device.
            esp_spp_disconnect(param->open.handle);
            if (slot >= 0) {
                mark_idle(slot);
            }
        } else {
            s_links[slot].state = LINK_CONNECTED;
            s_links[slot].handle = param->open.handle;
            ESP_LOGI(TAG, "%s: connected", bridge_config_slot_name(slot));
            link_up = slot;
            strlcpy(name, s_links[slot].name, sizeof(name));
        }
        finish_dial();
        break;
    }
    case ESP_SPP_CLOSE_EVT: {
        int slot = slot_by_handle(param->close.handle);
        if (slot >= 0) {
            ESP_LOGW(TAG, "%s: link closed", bridge_config_slot_name(slot));
            mark_idle(slot);
            link_down = slot;
        } else if (s_dialing >= 0 && s_links[s_dialing].state == LINK_CONNECTING) {
            mark_idle(s_dialing);
            finish_dial();
        }
        break;
    }
    case ESP_SPP_WRITE_EVT:
        write_slot = slot_by_handle(param->write.handle);
        write_ok = param->write.status == ESP_SPP_SUCCESS;
        write_cong = param->write.cong;
        break;
    case ESP_SPP_CONG_EVT:
        if (!param->cong.cong) {
            uncongested = slot_by_handle(param->cong.handle);
        }
        break;
    default:
        break;
    }
    unlock();

    char line[72];
    if (link_up >= 0) {
        uint8_t epoch = pipeline_link_up((bridge_slot_t)link_up);
        snprintf(line, sizeof(line), "event link %s up %u %s", bridge_config_slot_name(link_up), epoch, name);
        ble_server_send_control(line);
    }
    if (link_down >= 0) {
        pipeline_link_down((bridge_slot_t)link_down);
        snprintf(line, sizeof(line), "event link %s down", bridge_config_slot_name(link_down));
        ble_server_send_control(line);
    }
    if (write_slot >= 0) {
        pipeline_spp_write_done((bridge_slot_t)write_slot, write_ok, write_cong);
    }
    if (uncongested >= 0) {
        pipeline_spp_uncongested((bridge_slot_t)uncongested);
    }
}

esp_err_t links_start(links_data_cb on_data)
{
    s_on_data = on_data;
    s_lock = xSemaphoreCreateRecursiveMutex();
    for (int i = 0; i < BRIDGE_SLOT_COUNT; i++) {
        bridge_config_load((bridge_slot_t)i, &s_links[i].device);
    }

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_pin_code_t unused = {0};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, unused);

    ESP_ERROR_CHECK(esp_spp_register_callback(spp_cb));
    esp_spp_cfg_t cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };
    return esp_spp_enhanced_init(&cfg);
}

esp_err_t links_set_device(bridge_slot_t slot, const bridge_device_t *device)
{
    esp_err_t err = device ? bridge_config_save(slot, device) : bridge_config_forget(slot);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    link_t *link = &s_links[slot];
    bridge_device_t old = link->device;
    if (device) {
        link->device = *device;
    } else {
        memset(&link->device, 0, sizeof(link->device));
    }
    link->name[0] = '\0';
    request_close(slot);
    link->next_try = xTaskGetTickCount();
    bool still_used = old.valid && slot_by_mac(old.mac) >= 0;
    if (old.valid && !still_used) {
        esp_bt_gap_remove_bond_device(old.mac);
    }
    unlock();
    return ESP_OK;
}

esp_err_t links_write(bridge_slot_t slot, const uint8_t *data, size_t len)
{
    lock();
    bool connected = s_links[slot].state == LINK_CONNECTED && !s_links[slot].closing;
    uint32_t handle = s_links[slot].handle;
    unlock();
    if (!connected) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_spp_write(handle, (int)len, (uint8_t *)data);
}

void links_info(bridge_slot_t slot, links_info_t *out)
{
    static const char *STATES[] = { "idle", "dialing", "dialing", "connected" };
    lock();
    link_t *link = &s_links[slot];
    out->valid = link->device.valid;
    memcpy(out->mac, link->device.mac, sizeof(out->mac));
    out->state = STATES[link->state];
    strlcpy(out->name, link->name, sizeof(out->name));
    unlock();
}

esp_err_t links_scan(bridge_reply_fn reply)
{
    lock();
    if (s_scan_pending || s_scan_running) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_reply = reply;
    s_scan_pending = true;
    for (int i = 0; i < BRIDGE_SLOT_COUNT; i++) {
        request_close(i);
    }
    start_scan_if_pending();
    unlock();
    return ESP_OK;
}

void links_paired(bridge_reply_fn reply)
{
    int count = esp_bt_gap_get_bond_device_num();
    esp_bd_addr_t *list = count > 0 ? calloc(count, sizeof(*list)) : NULL;
    if (list && esp_bt_gap_get_bond_device_list(&count, list) != ESP_OK) {
        count = 0;
    }
    char line[80];
    for (int i = 0; list && i < count; i++) {
        lock();
        int slot = slot_by_mac(list[i]);
        const char *name = "";
        if (slot >= 0 && s_links[slot].name[0]) {
            name = s_links[slot].name;
        } else if (seen_find(list[i])) {
            name = seen_find(list[i])->name;
        }
        snprintf(line, sizeof(line), "paired " ESP_BD_ADDR_STR " %s %s", ESP_BD_ADDR_HEX(list[i]),
                 slot >= 0 ? bridge_config_slot_name(slot) : "-", name);
        unlock();
        reply(line);
    }
    free(list);
    reply("paired done");
}
