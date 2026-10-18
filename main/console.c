#include "console.h"

#include <stdio.h>
#include "commands.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "console";
#define CONSOLE_UART CONFIG_ESP_CONSOLE_UART_NUM
#define CMD_LINE_MAX 96

static void usb_reply(const char *line)
{
    printf("%s\n", line);
    fflush(stdout);
}

static void console_task(void *arg)
{
    char line[CMD_LINE_MAX];
    size_t len = 0;
    uint8_t c;
    for (;;) {
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len > 0) {
                line[len] = '\0';
                bridge_command_run(line, usb_reply, true);
                len = 0;
            }
        } else if (len < CMD_LINE_MAX - 1) {
            line[len++] = (char)c;
        }
    }
}

void console_start(void)
{
    if (uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "uart driver install failed");
        return;
    }
    xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);
}
