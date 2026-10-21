#pragma once

#include <stddef.h>
#include <stdint.h>
#include "bridge_config.h"
#include "esp_err.h"

// Sends one line of command output back to whoever asked (USB or BLE).
typedef void (*bridge_reply_fn)(const char *line);

typedef void (*links_data_cb)(bridge_slot_t slot, const uint8_t *data, size_t len);

// Owns the classic-Bluetooth side: one SPP link per slot, reconnecting in the
// background. Registers the BR/EDR GAP and SPP callbacks; Bluedroid must be enabled.
esp_err_t links_start(links_data_cb on_data);

// Binds a slot to a device (or clears it when device is NULL), saves it to NVS
// and applies it immediately: the old link is dropped and the new one dialled.
esp_err_t links_set_device(bridge_slot_t slot, const bridge_device_t *device);

// Writes uplink data (e.g. RTCM) to the slot's device. Fails when not connected.
esp_err_t links_write(bridge_slot_t slot, const uint8_t *data, size_t len);

typedef struct {
    bool valid;
    esp_bd_addr_t mac;
    const char *state; // "idle" | "dialing" | "connected"
    char name[32];
} links_info_t;

void links_info(bridge_slot_t slot, links_info_t *out);

// Inquiry scan (~10 s), then remote name requests for nameless devices (up to
// 8). Open links are dropped until it ends. Replies (docs/protocol.md §5):
//   found <mac> <rssi> <cod> <class> <spp|-> <name>
//   name <mac> <name>
//   scan done
esp_err_t links_scan(bridge_reply_fn reply);

// Classic-Bluetooth devices bonded with the bridge:
//   paired <mac> <slot|-> <name>   ...   paired done
void links_paired(bridge_reply_fn reply);

// Non-disruptive RAM snapshot: devices begin, found/seen pairs, devices done.
void links_cached(bridge_reply_fn reply);
