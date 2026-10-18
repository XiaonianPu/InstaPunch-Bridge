#include "pipeline.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "ble_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "links.h"

static const char *TAG = "pipeline";

#define PUMP_PERIOD_MS 10
#define MAX_IN_FLIGHT 4
#define IN_FLIGHT_TIMEOUT_MS 1000
#define LOST_EVENT_PERIOD_MS 1000
#define DEFAULT_MTU 23

typedef struct {
    down_stream_t down;
    ack_window_t window;
    up_stream_t up;
    bool hold;
    bool linked;
    bool spp_busy; // a record is in esp_spp_write, or SPP is congested
    uint32_t down_lost_reported;
    uint32_t up_lost_reported;
} slot_pipe_t;

static slot_pipe_t s_pipes[BRIDGE_SLOT_COUNT];
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

static bool s_phone_ready;
static uint16_t s_mtu = DEFAULT_MTU;
static bool s_congested;
static int s_in_flight;
static uint32_t s_last_conf_ms;
static uint32_t s_last_lost_event_ms;
static uint8_t s_next_slot; // round robin

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void wake(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static size_t max_frame(const slot_pipe_t *pipe)
{
    size_t limit = s_mtu > 3 ? (size_t)s_mtu - 3 : 0;
    if (pipe->hold && limit > FRAME_HOLD_MAX) {
        limit = FRAME_HOLD_MAX;
    }
    return limit;
}

static size_t payload_len(const uint8_t *frame, size_t len)
{
    frame_view_t view;
    return frame_parse(frame, len, &view) ? view.payload_len : 0;
}

static bool gate_open(void)
{
    return s_phone_ready && !s_congested && s_in_flight < MAX_IN_FLIGHT;
}

// Sends one frame for the slot if it has one. Returns true when a frame went out.
static bool pump_slot_down(slot_pipe_t *pipe, uint32_t now)
{
    static uint8_t frame[FRAME_HDR_LEN + FRAME_AGE_LEN + 512];
    size_t limit = max_frame(pipe);
    size_t len = 0;
    bool fresh = false;

    if (pipe->hold && pipe->window.resending) {
        const uint8_t *old;
        while ((old = window_next_resend(&pipe->window, &len)) != NULL) {
            if (len <= limit) {
                memcpy(frame, old, len);
                break;
            }
            pipe->down.lost += (uint32_t)payload_len(old, len); // MTU shrank below the frame
        }
        if (!old) {
            len = 0;
        }
    }
    if (len == 0) {
        len = down_pack(&pipe->down, now, limit, false, frame);
        fresh = len > 0;
    }
    if (len == 0) {
        return false;
    }

    if (fresh && pipe->hold) {
        pipe->down.lost += (uint32_t)window_push(&pipe->window, frame, len);
    }
    if (ble_server_notify_data(frame, len) != ESP_OK) {
        if (pipe->hold) {
            window_start_resend(&pipe->window);
        } else if (fresh) {
            pipe->down.lost += (uint32_t)payload_len(frame, len);
            pipe->down.gap = true;
        }
        return false;
    }
    if (s_in_flight == 0) {
        s_last_conf_ms = now;
    }
    s_in_flight++;
    return true;
}

static void pump_slot_up(bridge_slot_t slot, slot_pipe_t *pipe)
{
    size_t len;
    const uint8_t *record = up_peek(&pipe->up, &len);
    if (!record || !pipe->linked || pipe->spp_busy) {
        return;
    }
    if (links_write(slot, record, len) == ESP_OK) {
        pipe->spp_busy = true; // popped on ESP_SPP_WRITE_EVT
    } else {
        pipe->up.lost += (uint32_t)len;
        up_pop(&pipe->up);
    }
}

static void report_lost(uint32_t now)
{
    if (now - s_last_lost_event_ms < LOST_EVENT_PERIOD_MS) {
        return;
    }
    s_last_lost_event_ms = now;
    char line[48];
    for (int i = 0; i < BRIDGE_SLOT_COUNT; i++) {
        slot_pipe_t *pipe = &s_pipes[i];
        if (pipe->down.lost != pipe->down_lost_reported) {
            snprintf(line, sizeof(line), "event lost %s down %" PRIu32, bridge_config_slot_name(i),
                     pipe->down.lost - pipe->down_lost_reported);
            pipe->down_lost_reported = pipe->down.lost;
            ble_server_send_control(line);
        }
        if (pipe->up.lost != pipe->up_lost_reported) {
            snprintf(line, sizeof(line), "event lost %s up %" PRIu32, bridge_config_slot_name(i),
                     pipe->up.lost - pipe->up_lost_reported);
            pipe->up_lost_reported = pipe->up.lost;
            ble_server_send_control(line);
        }
    }
}

static void pump(void)
{
    uint32_t now = now_ms();

    // Notifications that never confirmed (e.g. a dropped link) must not stall the gate.
    if (s_in_flight > 0 && now - s_last_conf_ms > IN_FLIGHT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "%d notifications unconfirmed, resetting", s_in_flight);
        s_in_flight = 0;
    }

    for (int i = 0; i < BRIDGE_SLOT_COUNT; i++) {
        pump_slot_up((bridge_slot_t)i, &s_pipes[i]);
    }

    bool progress = true;
    while (progress && gate_open()) {
        progress = false;
        for (int k = 0; k < BRIDGE_SLOT_COUNT && gate_open(); k++) {
            int i = (s_next_slot + k) % BRIDGE_SLOT_COUNT;
            if (pump_slot_down(&s_pipes[i], now)) {
                progress = true;
            }
        }
        s_next_slot = (s_next_slot + 1) % BRIDGE_SLOT_COUNT;
    }

    report_lost(now);
}

static void pump_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PUMP_PERIOD_MS));
        lock();
        pump();
        unlock();
    }
}

void pipeline_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < BRIDGE_SLOT_COUNT; i++) {
        down_init(&s_pipes[i].down, (uint8_t)(i + 1));
        window_init(&s_pipes[i].window);
        up_init(&s_pipes[i].up);
    }
    xTaskCreate(pump_task, "pipeline", 4096, NULL, 6, &s_task);
}

void pipeline_configure(bridge_slot_t slot, frame_mode_t mode, bool hold)
{
    lock();
    slot_pipe_t *pipe = &s_pipes[slot];
    pipe->down.mode = mode;
    if (pipe->hold && !hold) {
        window_clear(&pipe->window);
    }
    pipe->hold = hold;
    unlock();
}

void pipeline_device_data(bridge_slot_t slot, const uint8_t *data, size_t len)
{
    lock();
    slot_pipe_t *pipe = &s_pipes[slot];
    if (s_phone_ready || pipe->hold) {
        down_push(&pipe->down, data, len, now_ms(), !s_phone_ready);
    } else {
        pipe->down.gap = true; // nobody listening and nothing kept
    }
    unlock();
    wake();
}

uint8_t pipeline_link_up(bridge_slot_t slot)
{
    lock();
    slot_pipe_t *pipe = &s_pipes[slot];
    down_link_up(&pipe->down);
    pipe->linked = true;
    pipe->spp_busy = false;
    uint8_t epoch = pipe->down.epoch;
    unlock();
    wake();
    return epoch;
}

void pipeline_link_down(bridge_slot_t slot)
{
    lock();
    slot_pipe_t *pipe = &s_pipes[slot];
    down_link_down(&pipe->down);
    up_clear(&pipe->up); // stale corrections are worse than none
    pipe->linked = false;
    pipe->spp_busy = false;
    unlock();
    wake();
}

void pipeline_spp_write_done(bridge_slot_t slot, bool ok, bool congested)
{
    lock();
    slot_pipe_t *pipe = &s_pipes[slot];
    size_t len;
    if (up_peek(&pipe->up, &len)) {
        if (!ok) {
            pipe->up.lost += (uint32_t)len;
        }
        up_pop(&pipe->up);
    }
    pipe->spp_busy = congested; // cleared again by ESP_SPP_CONG_EVT
    unlock();
    wake();
}

void pipeline_spp_uncongested(bridge_slot_t slot)
{
    lock();
    s_pipes[slot].spp_busy = false;
    unlock();
    wake();
}

void pipeline_phone_ready(bool ready, uint16_t mtu)
{
    lock();
    if (ready != s_phone_ready) {
        for (int i = 0; i < BRIDGE_SLOT_COUNT; i++) {
            slot_pipe_t *pipe = &s_pipes[i];
            if (ready) {
                if (pipe->hold) {
                    window_start_resend(&pipe->window);
                }
            } else if (pipe->hold) {
                pipe->down.replay_end = pipe->down.in; // unsent bytes now count as offline
            } else {
                down_discard(&pipe->down);
            }
        }
    }
    s_phone_ready = ready;
    s_mtu = mtu;
    s_congested = false;
    s_in_flight = 0;
    unlock();
    wake();
}

void pipeline_mtu(uint16_t mtu)
{
    lock();
    s_mtu = mtu;
    unlock();
    wake();
}

void pipeline_notify_done(const uint8_t *frame, size_t len, bool ok)
{
    lock();
    if (s_in_flight > 0) {
        s_in_flight--;
    }
    s_last_conf_ms = now_ms();
    if (!ok && frame && len >= FRAME_HDR_LEN && frame[0] >= 1 && frame[0] <= BRIDGE_SLOT_COUNT) {
        slot_pipe_t *pipe = &s_pipes[frame[0] - 1];
        if (pipe->hold) {
            window_start_resend(&pipe->window);
        } else {
            pipe->down.lost += (uint32_t)payload_len(frame, len);
            pipe->down.gap = true;
        }
    }
    unlock();
    wake();
}

void pipeline_congested(bool congested)
{
    lock();
    s_congested = congested;
    unlock();
    wake();
}

void pipeline_uplink(const uint8_t *frame, size_t len)
{
    if (len < UP_HDR_LEN || frame[0] < 1 || frame[0] > BRIDGE_SLOT_COUNT) {
        return;
    }
    lock();
    slot_pipe_t *pipe = &s_pipes[frame[0] - 1];
    if (pipe->linked) {
        up_write(&pipe->up, frame + UP_HDR_LEN, len - UP_HDR_LEN, frame[1] & UP_MORE);
    } else {
        pipe->up.lost += (uint32_t)(len - UP_HDR_LEN);
    }
    unlock();
    wake();
}

void pipeline_ack(bridge_slot_t slot, uint8_t epoch, uint8_t seq)
{
    lock();
    window_ack(&s_pipes[slot].window, epoch, seq);
    unlock();
}

void pipeline_info(bridge_slot_t slot, pipeline_info_t *out)
{
    lock();
    slot_pipe_t *pipe = &s_pipes[slot];
    out->mode = pipe->down.mode;
    out->hold = pipe->hold;
    out->epoch = pipe->down.epoch;
    out->down_lost = pipe->down.lost;
    out->up_lost = pipe->up.lost;
    unlock();
}
