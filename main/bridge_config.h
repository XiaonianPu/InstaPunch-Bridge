#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_bt_defs.h"
#include "esp_err.h"

#define BRIDGE_PIN_MAX 16

// Generic SPP slots. The bridge does not know what each device is; the app
// decides. Slot i (0-based) is channel / command number i + 1 (docs/protocol.md).
#define BRIDGE_SLOT_COUNT 2
typedef uint8_t bridge_slot_t;

// A downstream SPP device, stored in NVS so users never rebuild the firmware.
typedef struct {
    bool valid;
    esp_bd_addr_t mac;
    char pin[BRIDGE_PIN_MAX + 1];
} bridge_device_t;

esp_err_t bridge_config_load(bridge_slot_t slot, bridge_device_t *out);
esp_err_t bridge_config_save(bridge_slot_t slot, const bridge_device_t *device);
esp_err_t bridge_config_forget(bridge_slot_t slot);

// Per-slot relay options (docs/protocol.md §2). Defaults: mode 0 (line), hold off.
// They survive `forget`, which only clears the device.
typedef struct {
    uint8_t mode; // frame_mode_t
    bool hold;
} bridge_options_t;

void bridge_config_load_options(bridge_slot_t slot, bridge_options_t *out);
esp_err_t bridge_config_save_options(bridge_slot_t slot, const bridge_options_t *options);

// Parses "aa:bb:cc:dd:ee:ff". Returns false on malformed input.
bool bridge_config_parse_mac(const char *text, esp_bd_addr_t out);

// "1".."BRIDGE_SLOT_COUNT" <-> slot. Returns false for anything else.
bool bridge_config_parse_slot(const char *text, bridge_slot_t *out);
const char *bridge_config_slot_name(bridge_slot_t slot);
