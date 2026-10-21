#pragma once

#include <stddef.h>
#include <stdint.h>
#include "bridge_config.h"
#include "esp_err.h"

typedef void (*ble_command_cb)(const char *line);

// GATT server for the phone (docs/protocol.md). Every characteristic needs an
// encrypted link, and only the first phone that bonds (the owner) may bond;
// `unbind` (USB, or the owner over BLE) or a 10 s button press clears it. Bluedroid must be enabled.
// Link state, MTU, uplink frames and notify completions go to pipeline.c.
esp_err_t ble_server_start(ble_command_cb on_command);

// Notifies one finished downlink frame (frame.h). ESP_OK means it was queued;
// pipeline_notify_done() follows.
esp_err_t ble_server_notify_data(const uint8_t *frame, size_t len);

// Queues one complete control line; retry on congestion/API failure, no truncation.
void ble_server_send_control(const char *line);

// Publisher admission: subscribed/encrypted, sufficient MTU, empty control queue.
bool ble_server_snapshot_ready(uint32_t *generation);

// "phone <mac> connected" / "phone <mac> idle" / "phone none".
void ble_server_status(char *out, size_t size);

// Removes every BLE bond so the next phone to pair becomes the owner. A
// connected phone is dropped about 1 s later, after the reply has gone out.
esp_err_t ble_server_unbind_all(void);
