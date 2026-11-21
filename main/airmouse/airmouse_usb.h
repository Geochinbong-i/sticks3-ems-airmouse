#pragma once

#include <cstdint>

bool airmouse_usb_start();
void airmouse_usb_stop();
bool airmouse_usb_is_connected();
bool airmouse_usb_is_suspended();
void airmouse_usb_send(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
