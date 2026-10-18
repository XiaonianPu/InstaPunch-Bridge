#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bridge_config.h"
#include "frame.h"

// Moves bytes between the SPP links and the phone (docs/protocol.md §2-§6).
// One pump task owns all per-slot buffers; the Bluetooth callbacks only feed
// it events. It never holds its lock while calling back into links.c with the
// links lock held, so callers must not hold the links lock either.

void pipeline_start(void);

void pipeline_configure(bridge_slot_t slot, frame_mode_t mode, bool hold);

// ---- From links.c (classic side) ----
void pipeline_device_data(bridge_slot_t slot, const uint8_t *data, size_t len);
uint8_t pipeline_link_up(bridge_slot_t slot); // returns the new epoch
void pipeline_link_down(bridge_slot_t slot);
void pipeline_spp_write_done(bridge_slot_t slot, bool ok, bool congested);
void pipeline_spp_uncongested(bridge_slot_t slot);

// ---- From ble_server.c (phone side) ----
// `ready`: connected, encrypted and subscribed to the data characteristic.
void pipeline_phone_ready(bool ready, uint16_t mtu);
void pipeline_mtu(uint16_t mtu);
void pipeline_notify_done(const uint8_t *frame, size_t len, bool ok);
void pipeline_congested(bool congested);
void pipeline_uplink(const uint8_t *frame, size_t len);

// ---- From commands.c ----
void pipeline_ack(bridge_slot_t slot, uint8_t epoch, uint8_t seq);

typedef struct {
    frame_mode_t mode;
    bool hold;
    uint8_t epoch;
    uint32_t down_lost;
    uint32_t up_lost;
} pipeline_info_t;

void pipeline_info(bridge_slot_t slot, pipeline_info_t *out);
