#include "ble_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pipeline.h"
#include "status_led.h"

static const char *TAG = "ble";

#define APP_ID 0
#define LOCAL_MTU 517
#define PAIRING_TIMEOUT pdMS_TO_TICKS(30000)
#define CONTROL_MAX 128

// docs/protocol.md, little-endian.
// A7B750D5-8CE4-4F67-B586-8C1AA7800BBF
static const uint8_t UUID_SERVICE[16] = {0xbf, 0x0b, 0x80, 0xa7, 0x1a, 0x8c, 0x86, 0xb5, 0x67, 0x4f, 0xe4, 0x8c, 0xd5, 0x50, 0xb7, 0xa7};
// D36A57C5-8A8E-4C3F-9E65-96E193551BCE
static const uint8_t UUID_DATA[16] = {0xce, 0x1b, 0x55, 0x93, 0xe1, 0x96, 0x65, 0x9e, 0x3f, 0x4c, 0x8e, 0x8a, 0xc5, 0x57, 0x6a, 0xd3};
// 381B0BF7-5C19-4CCB-BF37-BC4214560D5A
static const uint8_t UUID_UPLINK[16] = {0x5a, 0x0d, 0x56, 0x14, 0x42, 0xbc, 0x37, 0xbf, 0xcb, 0x4c, 0x19, 0x5c, 0xf7, 0x0b, 0x1b, 0x38};
// FC838A35-6BDB-4571-A93B-E1D2E538DECD
static const uint8_t UUID_CONTROL[16] = {0xcd, 0xde, 0x38, 0xe5, 0xd2, 0xe1, 0x3b, 0xa9, 0x71, 0x45, 0xdb, 0x6b, 0x35, 0x8a, 0x83, 0xfc};

enum {
    IDX_SVC,
    IDX_DATA_CHAR, IDX_DATA_VAL, IDX_DATA_CCC,
    IDX_UPLINK_CHAR, IDX_UPLINK_VAL,
    IDX_CONTROL_CHAR, IDX_CONTROL_VAL, IDX_CONTROL_CCC,
    IDX_COUNT
};

static const uint16_t UUID_PRIMARY_SERVICE = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t UUID_CHAR_DECLARE = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t UUID_CCC = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t PROP_NOTIFY = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t PROP_WRITE_NR = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t PROP_WRITE_NOTIFY = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static uint8_t s_ccc_init[2] = {0, 0};
static uint8_t s_empty[1] = {0};

#define ENC_READ ESP_GATT_PERM_READ_ENCRYPTED
#define ENC_WRITE ESP_GATT_PERM_WRITE_ENCRYPTED

static const esp_gatts_attr_db_t GATT_DB[IDX_COUNT] = {
    [IDX_SVC] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&UUID_PRIMARY_SERVICE, ESP_GATT_PERM_READ,
                  sizeof(UUID_SERVICE), sizeof(UUID_SERVICE), (uint8_t *)UUID_SERVICE}},

    [IDX_DATA_CHAR] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&UUID_CHAR_DECLARE, ESP_GATT_PERM_READ,
                        1, 1, (uint8_t *)&PROP_NOTIFY}},
    [IDX_DATA_VAL] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *)UUID_DATA, ENC_READ,
                       sizeof(s_empty), sizeof(s_empty), s_empty}},
    [IDX_DATA_CCC] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&UUID_CCC, ESP_GATT_PERM_READ | ENC_WRITE,
                       sizeof(s_ccc_init), sizeof(s_ccc_init), s_ccc_init}},

    [IDX_UPLINK_CHAR] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&UUID_CHAR_DECLARE, ESP_GATT_PERM_READ,
                          1, 1, (uint8_t *)&PROP_WRITE_NR}},
    [IDX_UPLINK_VAL] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *)UUID_UPLINK, ENC_WRITE,
                         LOCAL_MTU, sizeof(s_empty), s_empty}},

    [IDX_CONTROL_CHAR] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&UUID_CHAR_DECLARE, ESP_GATT_PERM_READ,
                           1, 1, (uint8_t *)&PROP_WRITE_NOTIFY}},
    [IDX_CONTROL_VAL] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *)UUID_CONTROL, ENC_WRITE,
                          CONTROL_MAX, sizeof(s_empty), s_empty}},
    [IDX_CONTROL_CCC] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&UUID_CCC, ESP_GATT_PERM_READ | ENC_WRITE,
                          sizeof(s_ccc_init), sizeof(s_ccc_init), s_ccc_init}},
};

static ble_command_cb s_on_command;
static uint16_t s_handles[IDX_COUNT];
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;

static bool s_connected;
static bool s_encrypted;
static uint16_t s_conn_id;
static esp_bd_addr_t s_peer;
static uint16_t s_mtu = 23;
static bool s_notify_data;
static bool s_notify_control;
static TimerHandle_t s_pairing_timer;
static TimerHandle_t s_unbind_timer;

#define CONTROL_QUEUE_SIZE 96
static QueueHandle_t s_control_queue;
static SemaphoreHandle_t s_control_lock;
static bool s_control_ready, s_control_busy, s_control_congested;
static uint16_t s_control_conn, s_control_mtu;
static TickType_t s_control_sent;
static uint32_t s_control_overflow, s_control_generation;
static bool s_gatt_congested;

// This lock never encloses calls to pipeline or links (their locks can call us).
static void sync_control(void)
{
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    bool ready = s_connected && s_encrypted && s_notify_control;
    if (!ready || (s_control_ready && s_control_conn != s_conn_id)) {
        xQueueReset(s_control_queue);
        s_control_busy = false;
    }
    if (ready && !s_control_ready) s_control_generation++;
    s_control_ready = ready;
    s_control_conn = s_conn_id;
    s_control_mtu = s_mtu;
    s_control_congested = s_gatt_congested;
    xSemaphoreGive(s_control_lock);
}

static void control_confirm(esp_ble_gatts_cb_param_t *param)
{
    char discarded[CONTROL_MAX + 1];
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    if (s_control_busy && s_control_conn == param->conf.conn_id) {
        if (param->conf.status == ESP_GATT_OK) xQueueReceive(s_control_queue, discarded, 0);
        else ESP_LOGW(TAG, "control notify failed: %d; retaining line", param->conf.status);
        s_control_busy = false;
    }
    xSemaphoreGive(s_control_lock);
}

static void control_task(void *arg)
{
    char line[CONTROL_MAX + 1];
    for (;;) {
        xSemaphoreTake(s_control_lock, portMAX_DELAY);
        if (s_control_ready && s_control_busy && xTaskGetTickCount() - s_control_sent > pdMS_TO_TICKS(5000)) {
            // Do not send another line: a late confirmation cannot be attributed
            // safely. Reconnect, then the publisher rebuilds a current snapshot.
            ESP_LOGW(TAG, "control confirmation timeout; reconnecting phone");
            esp_ble_gap_disconnect(s_peer);
            s_control_ready = false;
        }
        if (s_control_ready && !s_control_busy && !s_control_congested && xQueuePeek(s_control_queue, line, 0)) {
            size_t len = strlen(line);
            if (len > (size_t)s_control_mtu - 3) {
                // Preserve one-notification-per-line v1 framing; never send a
                // truncated MAC/status that a client could accept as valid.
                xQueueReceive(s_control_queue, line, 0);
                ESP_LOGW(TAG, "control line exceeds MTU %u", s_control_mtu);
            } else if (esp_ble_gatts_send_indicate(s_gatts_if, s_control_conn, s_handles[IDX_CONTROL_VAL], len,
                                                  (uint8_t *)line, false) == ESP_OK) {
                s_control_busy = true;
                s_control_sent = xTaskGetTickCount();
            }
        }
        xSemaphoreGive(s_control_lock);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}


// "InstaPunch-Bridge-XXXX": 22 characters, so the scan response is 24 of 31 bytes.
static char s_name[24];
static bool s_adv_data_set;
static bool s_scan_rsp_set;

static esp_ble_adv_params_t ADV_PARAMS = {
    .adv_int_min = 0xa0, // 100 ms
    .adv_int_max = 0xf0, // 150 ms
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void update_phone_ready(void)
{
    static bool s_ready_reported;
    bool ready = s_connected && s_encrypted && s_notify_data;
    if (ready != s_ready_reported) {
        s_ready_reported = ready;
        pipeline_phone_ready(ready, s_mtu);
    }
}

static void start_advertising(void)
{
    if (s_adv_data_set && s_scan_rsp_set && !s_connected) {
        esp_ble_gap_start_advertising(&ADV_PARAMS);
    }
}

static void configure_advertising(void)
{
    uint8_t adv[3 + 2 + sizeof(UUID_SERVICE)] = {0x02, ESP_BLE_AD_TYPE_FLAG, 0x06, 0x11, ESP_BLE_AD_TYPE_128SRV_CMPL};
    memcpy(&adv[5], UUID_SERVICE, sizeof(UUID_SERVICE));
    esp_ble_gap_config_adv_data_raw(adv, sizeof(adv));

    uint8_t rsp[2 + sizeof(s_name)];
    size_t name_len = strlen(s_name);
    rsp[0] = (uint8_t)(name_len + 1);
    rsp[1] = ESP_BLE_AD_TYPE_NAME_CMPL;
    memcpy(&rsp[2], s_name, name_len);
    esp_ble_gap_config_scan_rsp_data_raw(rsp, (uint32_t)(name_len + 2));
}

static void unbind_disconnect(TimerHandle_t timer)
{
    if (s_connected) {
        esp_ble_gap_disconnect(s_peer);
    }
}

static void pairing_timeout(TimerHandle_t timer)
{
    if (s_connected && !s_encrypted) {
        ESP_LOGW(TAG, "peer did not pair in time, disconnecting");
        esp_ble_gap_disconnect(s_peer);
    }
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        s_adv_data_set = true;
        start_advertising();
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        s_scan_rsp_set = true;
        start_advertising();
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT: {
        // Trust on first use: only an unbonded bridge accepts a new phone.
        bool accept = esp_ble_get_bond_device_num() == 0;
        ESP_LOGI(TAG, "pairing request %s", accept ? "accepted (new owner)" : "rejected (already owned)");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, accept);
        break;
    }
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            s_encrypted = true;
            update_phone_ready();
            ESP_LOGI(TAG, "phone " ESP_BD_ADDR_STR " encrypted", ESP_BD_ADDR_HEX(param->ble_security.auth_cmpl.bd_addr));
        } else {
            ESP_LOGW(TAG, "phone failed to authenticate, reason 0x%x", param->ble_security.auth_cmpl.fail_reason);
            esp_ble_gap_disconnect(param->ble_security.auth_cmpl.bd_addr);
        }
        break;
    default:
        break;
    }
    sync_control();
}

static void handle_write(esp_ble_gatts_cb_param_t *param)
{
    if (param->write.is_prep) {
        return;
    }
    uint16_t handle = param->write.handle;
    const uint8_t *value = param->write.value;
    uint16_t len = param->write.len;

    if (handle == s_handles[IDX_DATA_CCC] && len == 2) {
        s_notify_data = value[0] & 0x01;
        update_phone_ready();
    } else if (handle == s_handles[IDX_CONTROL_CCC] && len == 2) {
        s_notify_control = value[0] & 0x01;
    } else if (handle == s_handles[IDX_UPLINK_VAL]) {
        pipeline_uplink(value, len);
    } else if (handle == s_handles[IDX_CONTROL_VAL] && s_on_command) {
        char line[CONTROL_MAX + 1];
        size_t n = len < CONTROL_MAX ? len : CONTROL_MAX;
        memcpy(line, value, n);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            n--;
        }
        line[n] = '\0';
        s_on_command(line);
    }
}

static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        s_gatts_if = gatts_if;
        esp_ble_gap_set_device_name(s_name);
        configure_advertising();
        esp_ble_gatts_create_attr_tab(GATT_DB, gatts_if, IDX_COUNT, 0);
        break;
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK || param->add_attr_tab.num_handle != IDX_COUNT) {
            ESP_LOGE(TAG, "attribute table failed, status 0x%x", param->add_attr_tab.status);
            break;
        }
        memcpy(s_handles, param->add_attr_tab.handles, sizeof(s_handles));
        esp_ble_gatts_start_service(s_handles[IDX_SVC]);
        break;
    case ESP_GATTS_CONNECT_EVT:
        s_connected = true;
        s_gatt_congested = false;
        status_led_set_phone(true);
        s_encrypted = false;
        s_notify_data = false;
        s_notify_control = false;
        s_mtu = 23;
        s_conn_id = param->connect.conn_id;
        memcpy(s_peer, param->connect.remote_bda, ESP_BD_ADDR_LEN);
        // Ask a returning owner to re-encrypt straight away; strangers must pair,
        // which ESP_GAP_BLE_SEC_REQ_EVT refuses once the bridge has an owner.
        esp_ble_set_encryption(s_peer, ESP_BLE_SEC_ENCRYPT_NO_MITM);
        xTimerStart(s_pairing_timer, 0);
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGW(TAG, "phone disconnected reason=0x%x", param->disconnect.reason);
        s_connected = false;
        status_led_set_phone(false);
        s_encrypted = false;
        s_notify_data = false;
        s_notify_control = false;
        xTimerStop(s_pairing_timer, 0);
        update_phone_ready();
        start_advertising();
        break;
    case ESP_GATTS_MTU_EVT:
        s_mtu = param->mtu.mtu;
        pipeline_mtu(s_mtu);
        break;
    case ESP_GATTS_CONF_EVT:
        if (param->conf.handle == s_handles[IDX_CONTROL_VAL]) control_confirm(param);
        if (param->conf.handle == s_handles[IDX_DATA_VAL]) {
            pipeline_notify_done(param->conf.value, param->conf.len, param->conf.status == ESP_GATT_OK);
        }
        break;
    case ESP_GATTS_CONGEST_EVT:
        s_gatt_congested = param->congest.congested;
        pipeline_congested(param->congest.congested);
        break;
    case ESP_GATTS_WRITE_EVT:
        handle_write(param);
        break;
    default:
        break;
    }
    sync_control();
}

esp_err_t ble_server_notify_data(const uint8_t *frame, size_t len)
{
    if (!(s_connected && s_encrypted && s_notify_data) || len > (size_t)s_mtu - 3) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handles[IDX_DATA_VAL], (uint16_t)len,
                                       (uint8_t *)frame, false);
}

void ble_server_send_control(const char *line)
{
    char copy[CONTROL_MAX + 1];
    if (strlen(line) > CONTROL_MAX) {
        ESP_LOGW(TAG, "control line too long; rejected");
        return;
    }
    strlcpy(copy, line, sizeof(copy));
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    if (s_control_ready && xQueueSend(s_control_queue, copy, 0) != pdTRUE) {
        if ((s_control_overflow++ % 32) == 0) ESP_LOGW(TAG, "control queue full; next snapshot will refresh state");
    }
    xSemaphoreGive(s_control_lock);
}

bool ble_server_snapshot_ready(uint32_t *generation)
{
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    // A complete 24-device snapshot is 51 lines, plus status/paired. Never
    // accumulate periodic snapshots behind congestion or a small MTU.
    *generation = s_control_generation;
    bool ready = s_control_ready && s_control_mtu >= CONTROL_MAX + 3 &&
                 !s_control_congested && uxQueueMessagesWaiting(s_control_queue) == 0 && !s_control_busy;
    xSemaphoreGive(s_control_lock);
    return ready;
}

void ble_server_status(char *out, size_t size)
{
    int count = esp_ble_get_bond_device_num();
    if (count <= 0) {
        snprintf(out, size, "phone none");
        return;
    }
    esp_ble_bond_dev_t owner;
    int n = 1;
    esp_ble_get_bond_device_list(&n, &owner);
    snprintf(out, size, "phone " ESP_BD_ADDR_STR " %s", ESP_BD_ADDR_HEX(owner.bd_addr),
             s_connected && s_encrypted ? "connected" : "idle");
}

esp_err_t ble_server_unbind_all(void)
{
    int count = esp_ble_get_bond_device_num();
    if (count <= 0) {
        return ESP_OK;
    }
    esp_ble_bond_dev_t *list = calloc(count, sizeof(*list));
    if (!list) {
        return ESP_ERR_NO_MEM;
    }
    esp_ble_get_bond_device_list(&count, list);
    for (int i = 0; i < count; i++) {
        esp_ble_remove_bond_device(list[i].bd_addr);
    }
    free(list);
    if (s_connected) {
        // Let the `ok` reach the phone before the link drops.
        xTimerStart(s_unbind_timer, 0);
    }
    return ESP_OK;
}

esp_err_t ble_server_start(ble_command_cb on_command)
{
    s_on_command = on_command;
    s_control_lock = xSemaphoreCreateMutex();
    s_control_queue = xQueueCreate(CONTROL_QUEUE_SIZE, CONTROL_MAX + 1);
    if (!s_control_lock || !s_control_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(control_task, "ble_control", 3072, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    s_pairing_timer = xTimerCreate("ble_pair", PAIRING_TIMEOUT, pdFALSE, NULL, pairing_timeout);
    s_unbind_timer = xTimerCreate("ble_unbind", pdMS_TO_TICKS(1000), pdFALSE, NULL, unbind_disconnect);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "InstaPunch-Bridge-%02X%02X", mac[4], mac[5]);

    esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys));

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_cb));
    esp_ble_gatt_set_local_mtu(LOCAL_MTU);
    return esp_ble_gatts_app_register(APP_ID);
}
