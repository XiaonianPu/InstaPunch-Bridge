#include "bridge_config.h"

#include <stdio.h>
#include <string.h>
#include "nvs.h"

static const char *NAMESPACE = "bridge";

static void slot_keys(bridge_slot_t slot, char *mac_key, char *pin_key)
{
    sprintf(mac_key, "mac%d", (int)slot);
    sprintf(pin_key, "pin%d", (int)slot);
}

esp_err_t bridge_config_load(bridge_slot_t slot, bridge_device_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    char mac_key[8], pin_key[8];
    slot_keys(slot, mac_key, pin_key);
    size_t mac_len = sizeof(out->mac);
    size_t pin_len = sizeof(out->pin);
    if (nvs_get_blob(nvs, mac_key, out->mac, &mac_len) == ESP_OK && mac_len == sizeof(out->mac)) {
        out->valid = true;
        if (nvs_get_str(nvs, pin_key, out->pin, &pin_len) != ESP_OK) {
            out->pin[0] = '\0';
        }
    }
    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t bridge_config_save(bridge_slot_t slot, const bridge_device_t *device)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    char mac_key[8], pin_key[8];
    slot_keys(slot, mac_key, pin_key);
    err = nvs_set_blob(nvs, mac_key, device->mac, sizeof(device->mac));
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, pin_key, device->pin);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t bridge_config_forget(bridge_slot_t slot)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    char mac_key[8], pin_key[8];
    slot_keys(slot, mac_key, pin_key);
    nvs_erase_key(nvs, mac_key);
    nvs_erase_key(nvs, pin_key);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

void bridge_config_load_options(bridge_slot_t slot, bridge_options_t *out)
{
    out->mode = 0;
    out->hold = false;
    nvs_handle_t nvs;
    if (nvs_open(NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    char key[8];
    uint8_t value;
    sprintf(key, "mode%d", (int)slot);
    if (nvs_get_u8(nvs, key, &value) == ESP_OK) {
        out->mode = value;
    }
    sprintf(key, "hold%d", (int)slot);
    if (nvs_get_u8(nvs, key, &value) == ESP_OK) {
        out->hold = value != 0;
    }
    nvs_close(nvs);
}

esp_err_t bridge_config_save_options(bridge_slot_t slot, const bridge_options_t *options)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    char key[8];
    sprintf(key, "mode%d", (int)slot);
    err = nvs_set_u8(nvs, key, options->mode);
    if (err == ESP_OK) {
        sprintf(key, "hold%d", (int)slot);
        err = nvs_set_u8(nvs, key, options->hold ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

bool bridge_config_parse_mac(const char *text, esp_bd_addr_t out)
{
    unsigned int b[ESP_BD_ADDR_LEN];
    char tail;
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail)
        != ESP_BD_ADDR_LEN) {
        return false;
    }
    for (int i = 0; i < ESP_BD_ADDR_LEN; i++) {
        out[i] = (uint8_t)b[i];
    }
    return true;
}

bool bridge_config_parse_slot(const char *text, bridge_slot_t *out)
{
    char tail;
    int number;
    if (sscanf(text, "%d%c", &number, &tail) != 1 || number < 1 || number > BRIDGE_SLOT_COUNT) {
        return false;
    }
    *out = (bridge_slot_t)(number - 1);
    return true;
}

const char *bridge_config_slot_name(bridge_slot_t slot)
{
    static const char *NAMES[] = { "1", "2", "3", "4", "5", "6", "7" };
    _Static_assert(BRIDGE_SLOT_COUNT <= sizeof(NAMES) / sizeof(NAMES[0]), "add slot names");
    return NAMES[slot];
}
