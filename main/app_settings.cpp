#include "app_settings.h"

#include "nvs.h"
#include "nvs_flash.h"

#include <cmath>
#include <cstring>

AppSettings g_settings;

static constexpr const char *kNvsNs = "sticks3_cfg";

struct StoredImuAbs {
    uint32_t magic;
    float gx;
    float gy;
    float gz;
    float ax;
    float ay;
    float az;
};

struct StoredWifi {
    char ssid[sizeof(g_settings.wifi_ssid)];
    char pass[sizeof(g_settings.wifi_pass)];
};

static constexpr uint32_t kImuAbsMagic = 0x494D5531u; // "IMU1"

void app_settings_load() {
    nvs_handle_t h = 0;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return;

    int32_t v = 0;
    uint8_t b = 0;
    if (nvs_get_i32(h, "tmr_mode", &v) == ESP_OK && (v == 0 || v == 1)) {
        g_settings.timer_mode = (int)v;
    }
    if (nvs_get_i32(h, "bright", &v) == ESP_OK && v >= 20 && v <= 100) {
        g_settings.brightness_pct = (int)v;
    }
    if (nvs_get_i32(h, "screen_off", &v) == ESP_OK &&
        (v == 0 || v == 30 || v == 60 || v == 180)) {
        g_settings.screen_off_sec = v;
    }
    if (nvs_get_i32(h, "tz_hr", &v) == ESP_OK && v >= -12 && v <= 14) {
        g_settings.tz_offset_hours = (int)v;
    }
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

    if (nvs_get_u8(h, "harp_toggle", &b) == ESP_OK) g_settings.harp_toggle = (b != 0);

    StoredImuAbs imu{};
    size_t imu_sz = sizeof(imu);
    if (nvs_get_blob(h, "imu_abs", &imu, &imu_sz) == ESP_OK &&
        imu_sz == sizeof(imu) && imu.magic == kImuAbsMagic &&
        std::isfinite(imu.gx) && std::isfinite(imu.gy) && std::isfinite(imu.gz) &&
        std::isfinite(imu.ax) && std::isfinite(imu.ay) && std::isfinite(imu.az)) {
        g_settings.imu_abs_valid = true;
        g_settings.imu_abs_gx = imu.gx;
        g_settings.imu_abs_gy = imu.gy;
        g_settings.imu_abs_gz = imu.gz;
        g_settings.imu_abs_ax = imu.ax;
        g_settings.imu_abs_ay = imu.ay;
        g_settings.imu_abs_az = imu.az;
    }

    if (nvs_get_u8(h, "boot_ble_time", &b) == ESP_OK) g_settings.boot_ble_time_sync = (b != 0);

    StoredWifi wifi{};
    size_t wifi_len = sizeof(wifi);
    if (nvs_get_blob(h, "wifi_cfg", &wifi, &wifi_len) == ESP_OK &&
        wifi_len == sizeof(wifi) && wifi.ssid[0] != '\0' &&
        wifi.ssid[sizeof(wifi.ssid) - 1] == '\0' && wifi.pass[sizeof(wifi.pass) - 1] == '\0') {
        std::memcpy(g_settings.wifi_ssid, wifi.ssid, sizeof(wifi.ssid));
        std::memcpy(g_settings.wifi_pass, wifi.pass, sizeof(wifi.pass));
    } else {
        // Older firmware saved the credentials as two string entries.
        size_t ssid_len = sizeof(g_settings.wifi_ssid);
        if (nvs_get_str(h, "wifi_ssid", g_settings.wifi_ssid, &ssid_len) != ESP_OK) {
            g_settings.wifi_ssid[0] = '\0';
        }
        size_t pass_len = sizeof(g_settings.wifi_pass);
        if (nvs_get_str(h, "wifi_pass", g_settings.wifi_pass, &pass_len) != ESP_OK) {
            g_settings.wifi_pass[0] = '\0';
        }
    }

    nvs_close(h);
}

void app_settings_save() {
    nvs_handle_t h = 0;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_i32(h, "tmr_mode", g_settings.timer_mode);
    nvs_set_i32(h, "bright", g_settings.brightness_pct);
    nvs_set_i32(h, "screen_off", g_settings.screen_off_sec);
    nvs_set_i32(h, "tz_hr", g_settings.tz_offset_hours);
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
    nvs_set_u8(h, "harp_toggle", g_settings.harp_toggle ? 1 : 0);

    if (g_settings.imu_abs_valid) {
        StoredImuAbs imu{
            kImuAbsMagic,
            g_settings.imu_abs_gx,
            g_settings.imu_abs_gy,
            g_settings.imu_abs_gz,
            g_settings.imu_abs_ax,
            g_settings.imu_abs_ay,
            g_settings.imu_abs_az,
        };
        nvs_set_blob(h, "imu_abs", &imu, sizeof(imu));
    } else {
        nvs_erase_key(h, "imu_abs");
    }

    nvs_set_u8(h, "boot_ble_time", g_settings.boot_ble_time_sync ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

bool app_settings_save_wifi(const char *ssid, const char *pass) {
    StoredWifi wifi{};
    const size_t ssid_len = std::strlen(ssid);
    const size_t pass_len = std::strlen(pass);
    if (ssid_len == 0 || ssid_len >= sizeof(wifi.ssid) || pass_len >= sizeof(wifi.pass)) return false;
    std::memcpy(wifi.ssid, ssid, ssid_len);
    std::memcpy(wifi.pass, pass, pass_len);

    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNs, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool saved = nvs_set_blob(handle, "wifi_cfg", &wifi, sizeof(wifi)) == ESP_OK &&
                       nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (saved) {
        std::memcpy(g_settings.wifi_ssid, wifi.ssid, sizeof(wifi.ssid));
        std::memcpy(g_settings.wifi_pass, wifi.pass, sizeof(wifi.pass));
    }
    return saved;
}
