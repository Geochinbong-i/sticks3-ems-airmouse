#pragma once

void power_save_init();
void power_save_allow_screen_idle(bool allow);
bool power_save_keep_usb_awake();
void power_save_release_usb();
