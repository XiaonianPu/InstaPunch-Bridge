#include <stdio.h>
#include "ble_server.h"
#include "commands.h"
#include "console.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_log.h"
#include "esp_system.h"
#include "links.h"
#include "nvs_flash.h"
#include "pipeline.h"
#include "status_led.h"

static const char *TAG = "bridge";

static void on_device_data(bridge_slot_t slot, const uint8_t *data, size_t len)
{
    pipeline_device_data(slot, data, len);
    if (bridge_command_monitoring()) {
        fwrite(data, 1, len, stdout);
        fflush(stdout);
    }
}

static void on_ble_command(const char *text)
{
    char line[129];
    snprintf(line, sizeof(line), "%s", text);
    bridge_command_run(line, ble_server_send_control, false);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_LOGI(TAG, "dual-mode controller up");

    status_led_start();
    pipeline_start();
    bridge_command_load_options();
    ESP_ERROR_CHECK(ble_server_start(on_ble_command));
    ESP_ERROR_CHECK(links_start(on_device_data));
    bridge_command_start_publisher();
    console_start();
    ESP_LOGI(TAG, "free heap %u bytes, minimum %u", (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}
