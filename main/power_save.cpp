#include "power_save.h"

#include "M5Unified.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"

namespace {

constexpr const char *kTag = "power_save";
esp_pm_lock_handle_t s_screen_lock = nullptr;
esp_pm_lock_handle_t s_usb_lock = nullptr;

} // namespace

void power_save_init() {
    // StickS3's A/B buttons are active-low on GPIO11 and GPIO12.
    const bool button_wake = M5.getBoard() == m5::board_t::board_M5StickS3 &&
        gpio_wakeup_enable(GPIO_NUM_11, GPIO_INTR_LOW_LEVEL) == ESP_OK &&
        gpio_wakeup_enable(GPIO_NUM_12, GPIO_INTR_LOW_LEVEL) == ESP_OK &&
        esp_sleep_enable_gpio_wakeup() == ESP_OK;

    bool can_light_sleep = false;
    if (button_wake &&
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "screen", &s_screen_lock) == ESP_OK) {
        can_light_sleep = esp_pm_lock_acquire(s_screen_lock) == ESP_OK;
        if (!can_light_sleep) {
            esp_pm_lock_delete(s_screen_lock);
            s_screen_lock = nullptr;
        }
    }

    esp_pm_config_t cfg{};
    cfg.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    cfg.min_freq_mhz = 40;
    cfg.light_sleep_enable = can_light_sleep;
    const esp_err_t result = esp_pm_configure(&cfg);
    ESP_LOGI(kTag, "Power management: %s, screen-off light sleep: %s",
             esp_err_to_name(result), result == ESP_OK && can_light_sleep ? "enabled" : "disabled");
}

void power_save_allow_screen_idle(bool allow) {
    if (!s_screen_lock) return;
    if (allow) {
        esp_pm_lock_release(s_screen_lock);
    } else {
        esp_pm_lock_acquire(s_screen_lock);
    }
}

bool power_save_keep_usb_awake() {
    if (s_usb_lock) return true;
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_hid", &s_usb_lock) != ESP_OK) return false;
    if (esp_pm_lock_acquire(s_usb_lock) == ESP_OK) return true;
    esp_pm_lock_delete(s_usb_lock);
    s_usb_lock = nullptr;
    return false;
}

void power_save_release_usb() {
    if (!s_usb_lock) return;
    esp_pm_lock_release(s_usb_lock);
    esp_pm_lock_delete(s_usb_lock);
    s_usb_lock = nullptr;
}
