idf_component_register(SRCS "main.c" "bridge_config.c" "commands.c" "console.c" "frame.c" "links.c" "ble_server.c" "pipeline.c" "status_led.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_driver_uart esp_driver_rmt esp_timer)
