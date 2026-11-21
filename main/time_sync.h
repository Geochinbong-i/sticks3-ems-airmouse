#pragma once

enum class BootTimeSync { Idle, WiFi, Ble };

BootTimeSync time_sync_boot_source();
void time_sync_apply_tz();
void time_sync_init_from_rtc();
void time_sync_start_at_boot();
void time_sync_poll();
void time_sync_cancel_boot_ble();
void time_sync_save_rtc();
bool time_sync_is_valid();
void time_sync_ui_run();
