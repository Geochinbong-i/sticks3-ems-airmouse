#include "M5Unified.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "imu_cal.h"
#include "input_util.h"
#include "power_save.h"
#include "sound_fx.h"
#include "settings_ui.h"
#include "time_sync.h"
#include "ui_common.h"
#include "airmouse/airmouse_ui.h"
#include "air_harp/air_harp_ui.h"
#include "timer/ems_timer_ui.h"
#include "timer/simple_timer_ui.h"

#include <algorithm>
#include <ctime>

static const char *TAG = "sticks3_app";

enum class HomeItem { Timer, BleMouse, UsbMouse, AirHarp, Settings, Count };
constexpr int kHomeItemCount = static_cast<int>(HomeItem::Count);

static void draw_sync_icon(M5Canvas &canvas, BootTimeSync source, uint32_t color) {
    if (source == BootTimeSync::WiFi) {
        canvas.drawArc(10, 15, 9, 8, 225, 315, color);
        canvas.drawArc(10, 15, 6, 5, 225, 315, color);
        canvas.drawArc(10, 15, 3, 2, 225, 315, color);
        canvas.drawPixel(10, 15, color);
    } else {
        canvas.drawFastVLine(9, 4, 15, color);
        canvas.drawLine(9, 4, 13, 8, color);
        canvas.drawLine(13, 8, 5, 16, color);
        canvas.drawLine(5, 6, 13, 14, color);
        canvas.drawLine(13, 14, 9, 18, color);
    }
}

static void draw_home_screen(M5Canvas &canvas, int sel, int64_t now_ms) {
    int w = canvas.width();
    bool ems_run = ems_timer_is_running();
    const auto alarms = ems_timer_active_alarms();
    const bool ems_alarm = alarms.any();
    bool flash_on = ((now_ms / 400) % 2) == 0;

    canvas.fillScreen(0x090D14);

    // Top bar with vertical battery / charging plug badge
    uint32_t hdr_col = (ems_alarm && flash_on) ? 0xB71C1C : 0x142033;
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
    const char *hdr_title = show_alarm ? alarms.title() : show_sync ? "SYNC" : clock_text;
    const char *badge = nullptr;
    uint32_t badge_bg = 0x4A3208;
    uint32_t badge_fg = 0xFFAB40;
    if (ems_alarm && !flash_on) {
        badge = "DUE";
        badge_bg = 0x7F1D1D;
        badge_fg = 0xFF8A80;
    } else if (ems_run && !ems_alarm) {
        badge = "EMS";
        badge_bg = 0x103B26;
        badge_fg = 0x69F0AE;
    } else if (g_settings.sound_muted) {
        badge = "MUTE";
    }
    ui_draw_header(canvas, hdr_title, hdr_col,
                   (ems_alarm && flash_on) ? 0xFFFFFF : 0x80D8FF,
                   badge, badge_bg, badge_fg, show_sync ? 21 : 4);
    if (show_sync) draw_sync_icon(canvas, sync, 0x80D8FF);

    // Alternate the card backgrounds and round the outer corners of the stack.
    struct MenuCard {
        const char *title;
        const char *sub;
        uint32_t accent;
    };
    const bool simple_mode = (g_settings.timer_mode == 0);
    const MenuCard cards[kHomeItemCount] = {
        {simple_mode ? "SIMPLE TIMER" : "EMS TIMER",
         simple_mode ? "7-Color Stamp" : "EPI / Vital / Log",
         0xFF5252},
        {"BLE MOUSE", "Bluetooth Gyro", 0x29B6F6},
        {"USB MOUSE", "USB-C HID Gyro", 0x00BFA5},
        {"AIR HARP",  "Pitch / Volume", 0xC393FF},
        {"SETTINGS",  "Bright/Cal/Time", 0xFFB300},
    };

    constexpr int kBoardX = 2;
    const int kBoardW = w - 4;
    constexpr int kTopY = 24;
    const int kTileH = (canvas.height() - 29 - kTopY) / kHomeItemCount;
    constexpr int kLast = kHomeItemCount - 1;
    constexpr int kRad = 7;

    for (int i = 0; i < kHomeItemCount; i++) {
        int y = kTopY + i * kTileH;
        bool is_even = (i % 2 == 0);
        bool is_sel = (i == sel);

        // True inverted checkerboard colors
        uint32_t bg = is_even ? (is_sel ? 0x142030 : 0x0B1018)
                              : (is_sel ? 0xDCEEFF : 0xF1F5F9);
        uint32_t fg = is_even ? 0xFFFFFF : 0x0B1018;
        uint32_t sub_fg = is_even ? 0xA8BED6 : 0x334155;

        if (i == 0) {
            // Rounded top corners only, flat bottom seam
            canvas.fillRoundRect(kBoardX, y, kBoardW, kTileH, kRad, bg);
            canvas.fillRect(kBoardX, y + kTileH - kRad, kBoardW, kRad, bg);
        } else if (i == kLast) {
            // Flat top seam, rounded bottom corners only
            canvas.fillRoundRect(kBoardX, y, kBoardW, kTileH, kRad, bg);
            canvas.fillRect(kBoardX, y, kBoardW, kRad, bg);
        } else {
            // Square inner checkerboard rows touching cleanly with 0 gap
            canvas.fillRect(kBoardX, y, kBoardW, kTileH, bg);
        }

        if (is_sel) {
            // Selection indicator bar on left + crisp accent outline (strictly clipped to tile bounds)
            uint32_t acc = cards[i].accent;
            canvas.setClipRect(kBoardX, y, kBoardW, kTileH);
            if (i == 0) {
                canvas.fillRoundRect(kBoardX, y, 8, kTileH + kRad, kRad, acc);
                canvas.drawRoundRect(kBoardX, y, kBoardW, kTileH + kRad, kRad, acc);
                canvas.drawRoundRect(kBoardX + 1, y + 1, kBoardW - 2, kTileH + kRad - 2, kRad - 1, acc);
                canvas.fillRect(kBoardX, y + kTileH - 2, kBoardW, 2, acc);
            } else if (i == kLast) {
                canvas.fillRoundRect(kBoardX, y - kRad, 8, kTileH + kRad, kRad, acc);
                canvas.drawRoundRect(kBoardX, y - kRad, kBoardW, kTileH + kRad, kRad, acc);
                canvas.drawRoundRect(kBoardX + 1, y - kRad + 1, kBoardW - 2, kTileH + kRad - 2, kRad - 1, acc);
                canvas.fillRect(kBoardX, y, kBoardW, 2, acc);
            } else {
                canvas.fillRect(kBoardX, y, 8, kTileH, acc);
                canvas.drawRect(kBoardX, y, kBoardW, kTileH, acc);
                canvas.drawRect(kBoardX + 1, y + 1, kBoardW - 2, kTileH - 2, acc);
            }
            canvas.clearClipRect();
        }

        const int tx = kBoardX + 11;
        canvas.setTextWrap(false);
        canvas.setFont(&fonts::FreeSansBold9pt7b);
        canvas.setTextSize(1);
        const int title_width = canvas.textWidth(cards[i].title);
        const float title_scale = std::min(1.0f, static_cast<float>(w - tx - 5) / title_width);
        canvas.setTextSize(title_scale, 1.0f);
        canvas.setTextColor(fg, bg);
        canvas.drawString(cards[i].title, tx, y + 1);

        canvas.setFont(&fonts::Font2);
        canvas.setTextSize(1);
        canvas.setTextColor(sub_fg, bg);
        canvas.drawString(cards[i].sub, tx, y + 19);
    }

    ui_draw_footer2(canvas, "A: Next    B: Open", "Hold A: Mute Tone");
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
    cfg.internal_mic = false;
    M5.begin(cfg);
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

    int sel = 0;
    bool a_was_down = false;
    bool a_long_fired = false;
    int64_t a_down_ms = 0;
    int64_t last_draw_ms = 0;

    while (true) {
        input_update();
        int64_t now_ms = input_now_ms();
        bool a_down = input_a_down();

        if (a_down && !a_was_down) {
            a_down_ms = now_ms;
            a_long_fired = false;
        } else if (a_down && a_was_down && !a_long_fired && (now_ms - a_down_ms) >= 650) {
            a_long_fired = true;
            g_settings.sound_muted = !g_settings.sound_muted;
            sound_fx_apply_volume();
            app_settings_save();
            sound_fx_click();
            last_draw_ms = 0;
        } else if (!a_down && a_was_down && !a_long_fired) {
            sel = (sel + 1) % kHomeItemCount;
            sound_fx_click();
            last_draw_ms = 0;
        }
        a_was_down = a_down;

        if (input_b_pressed()) {
            const auto item = static_cast<HomeItem>(sel);
            if (item != HomeItem::AirHarp) sound_fx_confirm();
            canvas.deleteSprite();
            switch (item) {
                case HomeItem::Timer:
                    if (g_settings.timer_mode == 0) simple_timer_ui_run();
                    else ems_timer_ui_run();
                    break;
                case HomeItem::BleMouse: airmouse_ui_run(AirMouseTransport::Ble); break;
                case HomeItem::UsbMouse: airmouse_ui_run(AirMouseTransport::Usb); break;
                case HomeItem::AirHarp: air_harp_ui_run(); break;
                case HomeItem::Settings: settings_ui_run(); break;
                case HomeItem::Count: break;
            }
            canvas.createSprite(M5.Display.width(), M5.Display.height());
            input_wait_all_release();
            a_was_down = false;
            last_draw_ms = 0;
        }

        if (ui_screen_is_on() && (now_ms - last_draw_ms) >= 200) {
            last_draw_ms = now_ms;
            draw_home_screen(canvas, sel, now_ms);
        }

        input_idle_delay(25);
    }
}
