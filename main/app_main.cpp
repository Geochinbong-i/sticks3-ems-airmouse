#include "M5Unified.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "imu_cal.h"
#include "input_util.h"
#include "sound_fx.h"
#include "time_sync.h"
#include "ui_common.h"
#include "airmouse/airmouse_ui.h"
#include "timer/ems_timer_ui.h"
#include "timer/simple_timer_ui.h"

#include <cstdio>

static const char *TAG = "sticks3_app";

static void run_settings_menu() {
    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    input_wait_all_release();

    int sel = 0;
    constexpr int kCount = 16;

    while (true) {
        input_update();
        if (input_ab_held(500)) {
            app_settings_save();
            sound_fx_click();
            input_wait_ab_release();
            canvas.deleteSprite();
            return;
        }
        if (input_a_pressed()) {
            sel = (sel + 1) % kCount;
            sound_fx_click();
        }
        if (input_b_pressed()) {
            sound_fx_confirm();
            switch (sel) {
                case 0:
                    g_settings.timer_mode = (g_settings.timer_mode == 0) ? 1 : 0;
                    break;
                case 1:
                    g_settings.brightness_pct += 20;
                    if (g_settings.brightness_pct > 100) g_settings.brightness_pct = 20;
                    ui_apply_brightness();
                    break;
                case 2:
                    app_settings_save();
                    time_sync_ui_run();
                    break;
                case 3:
                    app_settings_save();
                    imu_abs_cal_ui_run();
                    break;
                case 4:
                    g_settings.tz_offset_hours++;
                    if (g_settings.tz_offset_hours > 14) g_settings.tz_offset_hours = -12;
                    time_sync_apply_tz();
                    break;
                case 5:
                    if (g_settings.epi_interval_sec == 180) g_settings.epi_interval_sec = 240;
                    else if (g_settings.epi_interval_sec == 240) g_settings.epi_interval_sec = 300;
                    else g_settings.epi_interval_sec = 180;
                    break;
                case 6:
                    g_settings.air_sensitivity = (g_settings.air_sensitivity % 5) + 1;
                    break;
                case 7:
                    g_settings.air_invert_x = !g_settings.air_invert_x;
                    break;
                case 8:
                    g_settings.air_invert_y = !g_settings.air_invert_y;
                    break;
                case 9:
                    g_settings.air_swap_xy = !g_settings.air_swap_xy;
                    break;
                case 10:
                    g_settings.air_natural_scroll = !g_settings.air_natural_scroll;
                    break;
                case 11:
                    g_settings.sound_volume += 20;
                    if (g_settings.sound_volume > 100) g_settings.sound_volume = 20;
                    sound_fx_apply_volume();
                    break;
                case 12:
                    g_settings.sound_muted = !g_settings.sound_muted;
                    sound_fx_apply_volume();
                    break;
                case 13:
                    g_settings.rotation = (g_settings.rotation == 0) ? 2 : 0;
                    M5.Display.setRotation(g_settings.rotation);
                    break;
                case 14:
                    app_settings_save();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                    break;
                case 15:
                    app_settings_save();
                    input_wait_all_release();
                    canvas.deleteSprite();
                    return;
            }
            app_settings_save();
        }

        int w = canvas.width();
        int h = canvas.height();
        canvas.fillScreen(0x0B0F17);
        ui_draw_header(canvas, "SETTINGS", 0x1D283A, 0xFFFFFF);

        struct SettingRow {
            char key[20];
            char val[12];
        } rows[kCount]{};

        std::snprintf(rows[0].key, 20, "Timer Mode:");
        std::snprintf(rows[0].val, 12, "%s", g_settings.timer_mode == 0 ? "SIMPLE" : "EMS");

        std::snprintf(rows[1].key, 20, "Brightness:");
        std::snprintf(rows[1].val, 12, "%d%%", g_settings.brightness_pct);

        std::snprintf(rows[2].key, 20, "Time Sync");
        std::snprintf(rows[2].val, 12, "->");

        std::snprintf(rows[3].key, 20, "IMU Abs Cal");
        std::snprintf(rows[3].val, 12, "->");

        std::snprintf(rows[4].key, 20, "Timezone:");
        std::snprintf(rows[4].val, 12, "UTC%+d", g_settings.tz_offset_hours);

        std::snprintf(rows[5].key, 20, "EPI Int:");
        std::snprintf(rows[5].val, 12, "%dm", g_settings.epi_interval_sec / 60);

        std::snprintf(rows[6].key, 20, "Mouse Sens:");
        std::snprintf(rows[6].val, 12, "%d", g_settings.air_sensitivity);

        std::snprintf(rows[7].key, 20, "Invert X:");
        std::snprintf(rows[7].val, 12, "%s", g_settings.air_invert_x ? "ON" : "OFF");

        std::snprintf(rows[8].key, 20, "Invert Y:");
        std::snprintf(rows[8].val, 12, "%s", g_settings.air_invert_y ? "ON" : "OFF");

        std::snprintf(rows[9].key, 20, "Swap X/Y:");
        std::snprintf(rows[9].val, 12, "%s", g_settings.air_swap_xy ? "ON" : "OFF");

        std::snprintf(rows[10].key, 20, "Nat Scroll:");
        std::snprintf(rows[10].val, 12, "%s", g_settings.air_natural_scroll ? "ON" : "OFF");

        std::snprintf(rows[11].key, 20, "Volume:");
        std::snprintf(rows[11].val, 12, "%d%%", g_settings.sound_volume);

        std::snprintf(rows[12].key, 20, "Mute:");
        std::snprintf(rows[12].val, 12, "%s", g_settings.sound_muted ? "ON" : "OFF");

        std::snprintf(rows[13].key, 20, "Flip Scr:");
        std::snprintf(rows[13].val, 12, "%s", g_settings.rotation == 2 ? "180" : "0");

        std::snprintf(rows[14].key, 20, "Reboot Dev");
        rows[14].val[0] = '\0';

        std::snprintf(rows[15].key, 20, "< BACK");
        rows[15].val[0] = '\0';

        int visible = 8;
        int start_idx = (sel >= visible) ? (sel - visible + 1) : 0;
        int y = 25;

        canvas.setFont(&fonts::Font2);
        canvas.setTextSize(1);
        canvas.setTextWrap(false);

        for (int i = start_idx; i < kCount && y < h - 31; i++) {
            bool is_sel = (i == sel);
            uint32_t bg = is_sel ? 0x1976D2 : ((i % 2 == 0) ? 0x111824 : 0x0B0F17);
            uint32_t fg = is_sel ? 0xFFFFFF : 0xCFD8DC;
            uint32_t vfg = is_sel ? 0xE3F2FD : 0x80D8FF;

            canvas.fillRoundRect(3, y, w - 6, 21, 3, bg);
            canvas.setTextColor(fg, bg);
            canvas.setCursor(7, y + 2);
            canvas.print(rows[i].key);

            if (rows[i].val[0] != '\0') {
                canvas.setTextColor(vfg, bg);
                int vw = canvas.textWidth(rows[i].val);
                canvas.setCursor(w - vw - 7, y + 2);
                canvas.print(rows[i].val);
            }
            y += 23;
        }

        ui_draw_footer2(canvas, "A: Next    B: Edit", "Hold A+B : Go Home");
        canvas.pushSprite(0, 0);
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

static void draw_home_screen(M5Canvas &canvas, int sel, int64_t now_ms) {
    int w = canvas.width();
    bool ems_run = ems_timer_is_running();
    bool ems_alarm = ems_timer_has_active_alarm();
    bool flash_on = ((now_ms / 400) % 2) == 0;

    canvas.fillScreen(0x090D14);

    // Top bar with vertical battery / charging plug badge
    uint32_t hdr_col = (ems_alarm && flash_on) ? 0xB71C1C : 0x142033;
    const char *hdr_title = (ems_alarm && flash_on) ? "EPI DUE!" : "STICKS3";
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
                   badge, badge_bg, badge_fg);

    // Menu board: 0px gap, inverted checkerboard even/odd colors,
    // rounded outer corners ONLY on top of row 0 and bottom of row 3.
    struct MenuCard {
        const char *title;
        const char *sub;
        uint32_t accent;
    };
    const bool simple_mode = (g_settings.timer_mode == 0);
    const MenuCard cards[4] = {
        {simple_mode ? "SIMPLE TIMER" : "EMS TIMER",
         simple_mode ? "7-Color Stamp" : "EPI / Vital / Log",
         0xFF5252},
        {"BLE MOUSE", "Bluetooth Gyro", 0x29B6F6},
        {"USB MOUSE", "USB-C HID Gyro", 0x00BFA5},
        {"SETTINGS",  "Bright/Cal/Time", 0xFFB300},
    };

    constexpr int kBoardX = 2;
    const int kBoardW = w - 4;
    constexpr int kTileH = 46;
    constexpr int kTopY = 25;
    constexpr int kRad = 7;

    for (int i = 0; i < 4; i++) {
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
        } else if (i == 3) {
            // Flat top seam, rounded bottom corners only
            canvas.fillRoundRect(kBoardX, y, kBoardW, kTileH, kRad, bg);
            canvas.fillRect(kBoardX, y, kBoardW, kRad, bg);
        } else {
            // Square inner checkerboard rows touching cleanly with 0 gap
            canvas.fillRect(kBoardX, y, kBoardW, kTileH, bg);
        }

        if (is_sel) {
            // Selection indicator bar on left + crisp accent outline
            uint32_t acc = cards[i].accent;
            if (i == 0) {
                canvas.fillRoundRect(kBoardX, y, 8, kTileH, kRad, acc);
                canvas.fillRect(kBoardX, y + kTileH - kRad, 8, kRad, acc);
                canvas.fillRect(kBoardX + 4, y, 4, kTileH, acc);
                canvas.drawRoundRect(kBoardX, y, kBoardW, kTileH, kRad, acc);
                canvas.drawRoundRect(kBoardX + 1, y + 1, kBoardW - 2, kTileH - 2, kRad - 1, acc);
            } else if (i == 3) {
                canvas.fillRoundRect(kBoardX, y, 8, kTileH, kRad, acc);
                canvas.fillRect(kBoardX, y, 8, kRad, acc);
                canvas.fillRect(kBoardX + 4, y, 4, kTileH, acc);
                canvas.drawRoundRect(kBoardX, y, kBoardW, kTileH, kRad, acc);
                canvas.drawRoundRect(kBoardX + 1, y + 1, kBoardW - 2, kTileH - 2, kRad - 1, acc);
            } else {
                canvas.fillRect(kBoardX, y, 8, kTileH, acc);
                canvas.drawRect(kBoardX, y, kBoardW, kTileH, acc);
                canvas.drawRect(kBoardX + 1, y + 1, kBoardW - 2, kTileH - 2, acc);
            }
        }

        int tx = kBoardX + 11;
        canvas.setFont(&fonts::Font2);
        canvas.setTextWrap(false);

        // Enlarged legible title
        canvas.setTextSize(1.08f, 1.15f);
        canvas.setTextColor(fg, bg);
        canvas.setCursor(tx, y + 5);
        canvas.print(cards[i].title);

        // Crisp subtitle
        canvas.setTextSize(1.0f, 1.0f);
        canvas.setTextColor(sub_fg, bg);
        canvas.setCursor(tx, y + 26);
        canvas.print(cards[i].sub);
    }

    ui_draw_footer2(canvas, "A: Next    B: Open", "Hold A: Mute Tone");
    canvas.pushSprite(0, 0);
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
    M5.begin(cfg);
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
            sel = (sel + 1) % 4;
            sound_fx_click();
            last_draw_ms = 0;
        }
        a_was_down = a_down;

        if (input_b_pressed()) {
            sound_fx_confirm();
            canvas.deleteSprite();
            if (sel == 0) {
                if (g_settings.timer_mode == 0) {
                    simple_timer_ui_run();
                } else {
                    ems_timer_ui_run();
                }
            } else if (sel == 1) {
                airmouse_ui_run(AirMouseTransport::Ble);
            } else if (sel == 2) {
                airmouse_ui_run(AirMouseTransport::Usb);
            } else {
                run_settings_menu();
            }
            canvas.createSprite(M5.Display.width(), M5.Display.height());
            input_wait_all_release();
            a_was_down = false;
            last_draw_ms = 0;
        }

        if ((now_ms - last_draw_ms) >= 120) {
            last_draw_ms = now_ms;
            draw_home_screen(canvas, sel, now_ms);
        }

        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
