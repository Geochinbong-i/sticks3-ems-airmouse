#include "M5Unified.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "imu_cal.h"
#include "input_util.h"
#include "ir/ir_ui.h"
#include "power_save.h"
#include "sound_fx.h"
#include "settings_ui.h"
#include "time_sync.h"
#include "ui_common.h"
#include "ui_theme.h"
#include "ui_menu.h"
#include "airmouse/airmouse_ui.h"
#include "air_harp/air_harp_ui.h"
#include "audio/voice_ui.h"
#include "timer/ems_timer_ui.h"
#include "timer/simple_timer_ui.h"

#include <ctime>

static const char *TAG = "sticks3_app";

enum class HomeItem { Timer, AirMouse, AirHarp, IrRemote, Recorder, Settings, Count };
constexpr int kHomeItemCount = static_cast<int>(HomeItem::Count);

static void draw_home_screen(M5Canvas &canvas, const MenuCursor &cursor, int64_t now_ms) {
    bool ems_run = ems_timer_is_running();
    const auto alarms = ems_timer_active_alarms();
    const bool ems_alarm = alarms.any();
    bool flash_on = ((now_ms / 400) % 2) == 0;

    const auto &theme = ui_theme();
    canvas.fillScreen(theme.background);

    // Top bar with vertical battery / charging plug badge
    const uint32_t hdr_col = (ems_alarm && flash_on) ? 0xB71C1Cu : theme.surface;
    const bool show_alarm = ems_alarm && flash_on;
    const BootTimeSync sync = time_sync_boot_source();
    const bool show_sync = !show_alarm && sync != BootTimeSync::Idle;
    char clock_text[16] = "--:--:--";
    if (!show_alarm && !show_sync && time_sync_is_valid()) {
        const time_t now = std::time(nullptr);
        struct tm local{};
        localtime_r(&now, &local);
        std::strftime(clock_text, sizeof(clock_text), "%H:%M:%S", &local);
    }
    const char *hdr_title = show_alarm ? alarms.title() : show_sync ? (sync == BootTimeSync::WiFi ? "WI-FI SYNC" : "BLE SYNC") : clock_text;
    const char *badge = nullptr;
    uint32_t badge_bg = 0x4A3208;
    uint32_t badge_fg = 0xFFAB40;
    if (ems_alarm && !flash_on) {
        badge = "DUE";
        badge_bg = 0x7F1D1D;
        badge_fg = 0xFF8A80;
    } else if (ems_run && !ems_alarm) {
        badge = "EMS";
        badge_bg = theme.alternate;
        badge_fg = theme.text;
    } else if (g_settings.sound_muted) {
        badge = "MUTE";
    }
    ui_draw_header(canvas, hdr_title, hdr_col,
                   show_alarm ? 0xFFFFFFu : theme.accent,
                   badge, badge_bg, badge_fg);

    const bool simple_mode = (g_settings.timer_mode == 0);
    const MenuRow rows[kHomeItemCount] = {
        {simple_mode ? "TIMER" : "EMS TIMER",
         simple_mode ? "Time and voice" : "Events and alerts",
         0xFF5252},
        {"AIR MOUSE", "Bluetooth / USB-C", 0x29B6F6},
        {"AIR HARP",  "Note / Arpeggio", 0xC393FF},
        {"IR REMOTE", "Learn and test", 0xFF946A},
        {"RECORDER", "Voice recordings", 0xFF5252},
        {"SETTINGS",  "Display / Sound", 0xFFB300},
    };

    ui_draw_menu(canvas, rows, kHomeItemCount, cursor);
    ui_draw_themed_footer(canvas, "A Next / Hold: Mute", "B Open   A+B Prev");
    ui_present(canvas);
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "boot start: psram_ok=%d size=%u",
             (int)esp_psram_is_initialized(), (unsigned)esp_psram_get_size());

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_ret = nvs_flash_init();
    }
    ESP_LOGI(TAG, "nvs_flash_init: %s", esp_err_to_name(nvs_ret));

    auto cfg = M5.config();
    cfg.internal_mic = true;
    M5.begin(cfg);
    ui_init_backlight();
    power_save_init();
    imu_set_idle();
    ESP_LOGI(TAG, "M5.begin ok (%dx%d imu=%d spk=%d rtc=%d)",
             (int)M5.Display.width(), (int)M5.Display.height(),
             (int)M5.Imu.isEnabled(), (int)M5.Speaker.isEnabled(),
             (int)M5.Rtc.isEnabled());

    app_settings_load();
    M5.Display.setRotation(g_settings.rotation);
    ui_apply_brightness();
    time_sync_init_from_rtc();

    sound_fx_init();
    ems_timer_init();
    time_sync_start_at_boot();

    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());

    MenuCursor cursor;
    MenuButtons buttons;
    int64_t last_draw_ms = 0;

    while (true) {
        input_update();
        const int64_t now_ms = input_now_ms();
        const auto action = buttons.update(input_a_down(), input_b_down(), now_ms, true);

        if (action == MenuAction::Mute) {
            g_settings.sound_muted = !g_settings.sound_muted;
            sound_fx_apply_volume();
            app_settings_save();
            sound_fx_click();
            last_draw_ms = 0;
        } else if (action == MenuAction::Next || action == MenuAction::Previous) {
            cursor.move(action == MenuAction::Next ? 1 : -1, kHomeItemCount);
            sound_fx_click();
            last_draw_ms = 0;
        } else if (action == MenuAction::Select) {
            const auto item = static_cast<HomeItem>(cursor.selected);
            if (item != HomeItem::AirHarp) sound_fx_confirm();
            canvas.deleteSprite();
            switch (item) {
                case HomeItem::Timer:
                    if (g_settings.timer_mode == 0) simple_timer_ui_run();
                    else ems_timer_ui_run();
                    break;
                case HomeItem::AirMouse: {
                    constexpr MenuRow modes[] = {
                        {"BLUETOOTH", "Wireless mouse", 0x29B6F6},
                        {"USB-C", "Wired mouse", 0x29B6F6},
                    };
                    const int mode = ui_choose_menu("AIR MOUSE", modes, 2);
                    if (mode >= 0) airmouse_ui_run(mode == 0 ? AirMouseTransport::Ble : AirMouseTransport::Usb);
                    break;
                }
                case HomeItem::AirHarp: air_harp_ui_run(); break;
                case HomeItem::IrRemote: ir_ui_run(); break;
                case HomeItem::Recorder: voice_recorder_ui_run(); break;
                case HomeItem::Settings: settings_ui_run(); break;
                case HomeItem::Count: break;
            }
            canvas.createSprite(M5.Display.width(), M5.Display.height());
            input_wait_all_release();
            buttons = {};
            last_draw_ms = 0;
        }

        if (ui_screen_is_on() && (last_draw_ms == 0 || now_ms - last_draw_ms >= 200)) {
            last_draw_ms = now_ms;
            draw_home_screen(canvas, cursor, now_ms);
        }

        input_idle_delay(25);
    }
}
