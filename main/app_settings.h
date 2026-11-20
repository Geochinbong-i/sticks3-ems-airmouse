#pragma once

#include <cstdint>

enum class TimerMode : int {
    Simple = 0,
    Ems = 1,
};

struct AppSettings {
    int timer_mode = 0;         // 0=Simple Timer, 1=EMS Timer
    int brightness_pct = 80;    // 20, 40, 60, 80, 100
    int tz_offset_hours = 9;    // -12..+14 (default +9 KST)

    int epi_interval_sec = 180; // 180, 240, 300
    int preset_idx = 4;         // 0=OFF,1=1m,2=2m,3=3m,4=5m,5=10m,6=15m

    int air_sensitivity = 3;    // 1..5
    bool air_invert_x = false;
    bool air_invert_y = false;
    bool air_swap_xy = false;
    bool air_natural_scroll = false;

    bool imu_abs_valid = false;
    float imu_abs_gx = 0.0f;
    float imu_abs_gy = 0.0f;
    float imu_abs_gz = 0.0f;
    float imu_abs_ax = 0.0f;
    float imu_abs_ay = 0.0f;
    float imu_abs_az = 1.0f;

    int sound_volume = 80;      // 20..100
    bool sound_muted = false;
    int rotation = 0;           // 0=Portrait, 2=Portrait Flip

    char wifi_ssid[33] = "";
    char wifi_pass[65] = "";
};

extern AppSettings g_settings;

void app_settings_load();
void app_settings_save();
