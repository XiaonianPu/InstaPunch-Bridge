#include "commands.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble_server.h"
#include "pipeline.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool s_monitoring;

static const char *mode_name(frame_mode_t mode)
{
    return mode == FRAME_MODE_RAW ? "raw" : "line";
}

static void status(bridge_reply_fn reply)
{
    char line[128];
    for (int i = 0; i < BRIDGE_SLOT_COUNT; i++) {
        links_info_t link;
        pipeline_info_t pipe;
        links_info((bridge_slot_t)i, &link);
        pipeline_info((bridge_slot_t)i, &pipe);
        if (!link.valid) {
            snprintf(line, sizeof(line), "%s none", bridge_config_slot_name(i));
        } else {
            snprintf(line, sizeof(line),
                     "%s " ESP_BD_ADDR_STR " %s %s hold=%d epoch=%u lost=%" PRIu32 "/%" PRIu32 " %s",
                     bridge_config_slot_name(i), ESP_BD_ADDR_HEX(link.mac), link.state, mode_name(pipe.mode),
                     pipe.hold ? 1 : 0, pipe.epoch, pipe.down_lost, pipe.up_lost, link.name);
        }
        reply(line);
        if (link.valid) {
            // Separate line so older apps, which take the rest of the slot line as the name, skip it.
            if (link.has_rssi) {
                snprintf(line, sizeof(line), "%s rssi=%d", bridge_config_slot_name(i), link.rssi_delta);
            } else {
                snprintf(line, sizeof(line), "%s rssi=-", bridge_config_slot_name(i));
            }
            reply(line);
        }
    }
    ble_server_status(line, sizeof(line));
    reply(line);
}

static bool next_slot(char **save, bridge_slot_t *slot)
{
    char *text = strtok_r(NULL, " \t", save);
    return text && bridge_config_parse_slot(text, slot);
}

static void pair(char **save, bridge_reply_fn reply)
{
    bridge_slot_t slot;
    bridge_device_t device = { .valid = true };
    bool ok = next_slot(save, &slot);
    char *mac = strtok_r(NULL, " \t", save);
    char *pin = strtok_r(NULL, " \t", save);
    if (!ok || !mac || !bridge_config_parse_mac(mac, device.mac)) {
        reply("error usage: pair <slot> <aa:bb:cc:dd:ee:ff> [pin]");
        return;
    }
    if (pin && strlen(pin) > BRIDGE_PIN_MAX) {
        reply("error pin too long");
        return;
    }
    if (pin) {
        strcpy(device.pin, pin);
    }
    reply(links_set_device(slot, &device) == ESP_OK ? "ok" : "error save failed");
}

static void forget(char **save, bridge_reply_fn reply)
{
    bridge_slot_t slot;
    if (!next_slot(save, &slot)) {
        reply("error usage: forget <slot>");
        return;
    }
    reply(links_set_device(slot, NULL) == ESP_OK ? "ok" : "error save failed");
}

static void set_option(char **save, bridge_reply_fn reply, bool is_mode)
{
    bridge_slot_t slot;
    bool ok = next_slot(save, &slot);
    char *value = strtok_r(NULL, " \t", save);
    bridge_options_t options;
    if (ok && value) {
        bridge_config_load_options(slot, &options);
        if (is_mode && (strcmp(value, "line") == 0 || strcmp(value, "raw") == 0)) {
            options.mode = strcmp(value, "raw") == 0 ? FRAME_MODE_RAW : FRAME_MODE_LINE;
        } else if (!is_mode && (strcmp(value, "on") == 0 || strcmp(value, "off") == 0)) {
            options.hold = strcmp(value, "on") == 0;
        } else {
            ok = false;
        }
    } else {
        ok = false;
    }
    if (!ok) {
        reply(is_mode ? "error usage: mode <slot> line|raw" : "error usage: hold <slot> on|off");
        return;
    }
    if (bridge_config_save_options(slot, &options) != ESP_OK) {
        reply("error save failed");
        return;
    }
    pipeline_configure(slot, (frame_mode_t)options.mode, options.hold);
    reply("ok");
}

// Silent on success: acks are frequent and need no answer.
static void ack(char **save, bridge_reply_fn reply)
{
    bridge_slot_t slot;
    bool ok = next_slot(save, &slot);
    char *epoch = strtok_r(NULL, " \t", save);
    char *seq = strtok_r(NULL, " \t", save);
    if (!ok || !epoch || !seq) {
        reply("error usage: ack <slot> <epoch> <seq>");
        return;
    }
    pipeline_ack(slot, (uint8_t)strtoul(epoch, NULL, 10), (uint8_t)strtoul(seq, NULL, 10));
}

void bridge_command_run(char *line, bridge_reply_fn reply, bool usb)
{
    // strtok_r: USB and BLE commands run on different tasks.
    char *state = NULL;
    char **save = &state;
    char *cmd = strtok_r(line, " \t", save);
    if (!cmd) {
        return;
    }
    if (strcmp(cmd, "info") == 0) {
        char text[64];
        snprintf(text, sizeof(text), "bridge %s proto %d slots %d", BRIDGE_FIRMWARE_VERSION,
                 BRIDGE_PROTOCOL_VERSION, BRIDGE_SLOT_COUNT);
        reply(text);
    } else if (strcmp(cmd, "status") == 0) {
        status(reply);
    } else if (strcmp(cmd, "scan") == 0) {
        if (links_scan(reply) != ESP_OK) {
            reply("error scan already running");
        }
    } else if (strcmp(cmd, "devices") == 0) {
        links_cached(reply);
    } else if (strcmp(cmd, "paired") == 0) {
        links_paired(reply);
    } else if (strcmp(cmd, "pair") == 0) {
        pair(save, reply);
    } else if (strcmp(cmd, "forget") == 0) {
        forget(save, reply);
    } else if (strcmp(cmd, "mode") == 0) {
        set_option(save, reply, true);
    } else if (strcmp(cmd, "hold") == 0) {
        set_option(save, reply, false);
    } else if (strcmp(cmd, "ack") == 0) {
        ack(save, reply);
    } else if (usb && strcmp(cmd, "unbind") == 0) {
        reply(ble_server_unbind_all() == ESP_OK ? "ok" : "error unbind failed");
    } else if (usb && strcmp(cmd, "monitor") == 0) {
        char *arg = strtok_r(NULL, " \t", save);
        s_monitoring = arg && strcmp(arg, "on") == 0;
        reply("ok");
    } else {
        reply(usb ? "commands: info | status | devices | scan | paired | pair <slot> <mac> [pin] | forget <slot> | "
                    "mode <slot> line|raw | hold <slot> on|off | unbind | monitor on|off"
                  : "error unknown command");
    }
}

bool bridge_command_monitoring(void)
{
    return s_monitoring;
}

void bridge_command_load_options(void)
{
    for (int i = 0; i < BRIDGE_SLOT_COUNT; i++) {
        bridge_options_t options;
        bridge_config_load_options((bridge_slot_t)i, &options);
        pipeline_configure((bridge_slot_t)i, (frame_mode_t)options.mode, options.hold);
    }
}

static void publish_task(void *arg)
{
    TickType_t last = 0;
    bool published = false;
    uint32_t last_generation = 0;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        // When the phone is absent, do not build or queue obsolete snapshots.
        uint32_t generation;
        if (ble_server_snapshot_ready(&generation) && (!published || generation != last_generation || now - last >= pdMS_TO_TICKS(5000))) {
            links_cached(ble_server_send_control);
            status(ble_server_send_control);
            links_paired(ble_server_send_control);
            last = now;
            last_generation = generation;
            published = true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void bridge_command_start_publisher(void)
{
    // Snapshot publishing runs separately from Bluetooth callbacks.
    configASSERT(xTaskCreate(publish_task, "bridge_status", 6144, NULL, 4, NULL) == pdPASS);
}
