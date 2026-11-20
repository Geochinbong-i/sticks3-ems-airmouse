#pragma once

#include <cstdint>

struct AppSettings {
    int epi_interval_sec = 180; // 180, 240, 300
    int preset_idx = 4;         // 0=OFF,1=1m,2=2m,3=3m,4=5m,5=10m,6=15m
    int air_sensitivity = 3;    // 1..5
    bool air_invert_x = false;
    bool air_invert_y = false;
    bool air_swap_xy = false;
    bool air_natural_scroll = false;
    int sound_volume = 80;      // 20..100
    bool sound_muted = false;
    int rotation = 0;           // 0=Portrait, 2=Portrait Flip
};

extern AppSettings g_settings;

void app_settings_load();
void app_settings_save();
