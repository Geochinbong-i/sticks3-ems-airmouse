#pragma once

#include <cstdint>

enum class BleStatus { Idle, Starting, Advertising, Connected, Failed };

BleStatus airmouse_ble_status();
bool airmouse_ble_start();
void airmouse_ble_stop();
bool airmouse_ble_is_connected();
void airmouse_ble_send(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
void airmouse_ble_update_battery(int level);

bool airmouse_ble_start_time_sync(void (*on_time_write)(const uint8_t *data, uint16_t len),
                                 bool bonded_only = false);
void airmouse_ble_stop_time_sync();
