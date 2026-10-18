#include "status_led.h"

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

#define LED_GPIO 27 // M5Stack Atom Lite SK6812
#define RMT_RESOLUTION_HZ 10000000 // 0.1 us per tick
#define LED_LEVEL 40 // of 255; the SK6812 is very bright at full power

#define IDLE_ON_MS 1000
#define IDLE_OFF_MS 1000
#define LINKED_ON_MS 80
#define LINKED_PERIOD_MS 3000

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static TaskHandle_t s_task;
static volatile bool s_connected;

static void show(bool on)
{
    // SK6812 takes G, R, B, most significant bit first. Blue.
    uint8_t grb[3] = {0, 0, on ? LED_LEVEL : 0};
    rmt_transmit_config_t tx = {.loop_count = 0};
    if (rmt_transmit(s_channel, s_encoder, grb, sizeof(grb), &tx) == ESP_OK) {
        rmt_tx_wait_all_done(s_channel, 100);
    }
}

// Waits `ms`; returns true when the phone state changed meanwhile.
static bool wait(uint32_t ms)
{
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms)) > 0;
}

static void led_task(void *arg)
{
    for (;;) {
        bool connected = s_connected;
        show(true);
        if (wait(connected ? LINKED_ON_MS : IDLE_ON_MS)) {
            continue;
        }
        show(false);
        wait(connected ? LINKED_PERIOD_MS - LINKED_ON_MS : IDLE_OFF_MS);
    }
}

void status_led_start(void)
{
    rmt_tx_channel_config_t channel = {
        .gpio_num = LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    // SK6812: 0 = 0.3 us high + 0.9 us low, 1 = 0.6 us high + 0.6 us low.
    rmt_bytes_encoder_config_t bytes = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 6, .level1 = 0, .duration1 = 6},
        .flags.msb_first = 1,
    };
    if (rmt_new_tx_channel(&channel, &s_channel) != ESP_OK || rmt_new_bytes_encoder(&bytes, &s_encoder) != ESP_OK ||
        rmt_enable(s_channel) != ESP_OK) {
        ESP_LOGE(TAG, "RMT setup failed, status LED disabled");
        return;
    }
    xTaskCreate(led_task, "led", 2048, NULL, 2, &s_task);
}

void status_led_set_phone(bool connected)
{
    if (connected == s_connected) {
        return;
    }
    s_connected = connected;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}
