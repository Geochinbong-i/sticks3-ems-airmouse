#include "app_settings.h"

#include "nvs.h"
#include "nvs_flash.h"

AppSettings g_settings;

static constexpr const char *kNvsNs = "sticks3_cfg";

void app_settings_load() {
    nvs_handle_t h = 0;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return;

    int32_t v = 0;
    uint8_t b = 0;
    if (nvs_get_i32(h, "epi_sec", &v) == ESP_OK && (v == 180 || v == 240 || v == 300)) {
        g_settings.epi_interval_sec = (int)v;
    }
    if (nvs_get_i32(h, "preset_i", &v) == ESP_OK && v >= 0 && v <= 6) {
        g_settings.preset_idx = (int)v;
    }
    if (nvs_get_i32(h, "air_sens", &v) == ESP_OK && v >= 1 && v <= 5) {
        g_settings.air_sensitivity = (int)v;
    }
    if (nvs_get_u8(h, "air_invx", &b) == ESP_OK) g_settings.air_invert_x = (b != 0);
    if (nvs_get_u8(h, "air_invy", &b) == ESP_OK) g_settings.air_invert_y = (b != 0);
    if (nvs_get_u8(h, "air_swap", &b) == ESP_OK) g_settings.air_swap_xy = (b != 0);
    if (nvs_get_u8(h, "air_nat", &b) == ESP_OK) g_settings.air_natural_scroll = (b != 0);
    if (nvs_get_i32(h, "vol", &v) == ESP_OK && v >= 10 && v <= 100) {
        g_settings.sound_volume = (int)v;
    }
    if (nvs_get_u8(h, "mute", &b) == ESP_OK) g_settings.sound_muted = (b != 0);
    if (nvs_get_i32(h, "rot", &v) == ESP_OK && (v == 0 || v == 2)) {
        g_settings.rotation = (int)v;
    }
    nvs_close(h);
}

void app_settings_save() {
    nvs_handle_t h = 0;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_i32(h, "epi_sec", g_settings.epi_interval_sec);
    nvs_set_i32(h, "preset_i", g_settings.preset_idx);
    nvs_set_i32(h, "air_sens", g_settings.air_sensitivity);
    nvs_set_u8(h, "air_invx", g_settings.air_invert_x ? 1 : 0);
    nvs_set_u8(h, "air_invy", g_settings.air_invert_y ? 1 : 0);
    nvs_set_u8(h, "air_swap", g_settings.air_swap_xy ? 1 : 0);
    nvs_set_u8(h, "air_nat", g_settings.air_natural_scroll ? 1 : 0);
    nvs_set_i32(h, "vol", g_settings.sound_volume);
    nvs_set_u8(h, "mute", g_settings.sound_muted ? 1 : 0);
    nvs_set_i32(h, "rot", g_settings.rotation);
    nvs_commit(h);
    nvs_close(h);
}
