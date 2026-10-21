#include "reset_button.h"

#include "ble_server.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "links.h"
#include "status_led.h"

static const char *TAG = "button";

#define BUTTON_GPIO 39 // M5Stack Atom Lite front button: input only, external pull-up, low when pressed
#define POLL_MS 20
#define HOLD_MS 10000

static void unbind_everything(void)
{
    esp_err_t phone = ble_server_unbind_all();
    esp_err_t devices = links_forget_all();
    ESP_LOGW(TAG, "held %d s: phone %s, devices %s", HOLD_MS / 1000, phone == ESP_OK ? "unbound" : "failed",
             devices == ESP_OK ? "unbound" : "failed");
    status_led_flash_reset();
}

static void button_task(void *arg)
{
    uint32_t held = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (gpio_get_level(BUTTON_GPIO) != 0) {
            held = 0;
            continue;
        }
        // Fires once per press; keeping the button down does nothing more.
        if (held < HOLD_MS && (held += POLL_MS) >= HOLD_MS) {
            unbind_everything();
        }
    }
}

void reset_button_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO setup failed, reset button disabled");
        return;
    }
    xTaskCreate(button_task, "button", 3072, NULL, 3, NULL);
}
