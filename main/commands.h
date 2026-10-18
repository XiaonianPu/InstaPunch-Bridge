#pragma once

#include <stdbool.h>
#include "links.h"

#define BRIDGE_FIRMWARE_VERSION "0.1.0"
#define BRIDGE_PROTOCOL_VERSION 1

// Text commands shared by the USB console and the BLE control characteristic
// (docs/protocol.md §5). `usb` unlocks the commands that need physical access.
//   info | status | scan | paired
//   pair <slot> <aa:bb:cc:dd:ee:ff> [pin] | forget <slot>
//   mode <slot> line|raw | hold <slot> on|off | ack <slot> <epoch> <seq>
//   unbind            (USB only) remove the owner phone
//   monitor on|off    (USB only) echo device data to the console
void bridge_command_run(char *line, bridge_reply_fn reply, bool usb);

// Whether `monitor on` is active.
bool bridge_command_monitoring(void);

// Applies the stored mode / hold options of every slot to the pipeline.
void bridge_command_load_options(void);
