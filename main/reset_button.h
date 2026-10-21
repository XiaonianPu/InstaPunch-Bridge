#pragma once

// Holding the button for 10 s removes the owner phone and every SPP device
// (both slots and all classic-Bluetooth bonds); the LED confirms with fast
// amber flashes. Pin options and modes are kept.
void reset_button_start(void);
