#pragma once

#include <cstdint>

bool airmouse_ble_start();
void airmouse_ble_stop();
bool airmouse_ble_is_connected();
void airmouse_ble_send(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
void airmouse_ble_update_battery(int level);
