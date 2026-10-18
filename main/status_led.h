#pragma once

#include <stdbool.h>

// RGB status LED (M5Stack Atom Lite: one SK6812 on G27).
//   phone not connected: on 1 s, off 1 s (2 s period)
//   phone connected:     one 80 ms flash every 3 s
void status_led_start(void);

// Called from the BLE connect / disconnect events. The new pattern starts at once.
void status_led_set_phone(bool connected);
