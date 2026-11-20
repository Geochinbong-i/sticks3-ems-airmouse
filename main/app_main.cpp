#include "app_settings.h"
#include "input_util.h"
#include "sound_fx.h"
#include "timer/ems_timer_ui.h"
#include "airmouse/airmouse_ui.h"

#include "M5Unified.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static void run_settings_menu() {
    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    input_wait_all_release();

    int sel = 0;
    constexpr int kCount = 10;

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
                    if (g_settings.epi_interval_sec == 180) g_settings.epi_interval_sec = 240;
                    else if (g_settings.epi_interval_sec == 240) g_settings.epi_interval_sec = 300;
                    else g_settings.epi_interval_sec = 180;
                    break;
                case 1:
                    g_settings.air_sensitivity = (g_settings.air_sensitivity % 5) + 1;
                    break;
                case 2:
                    g_settings.air_invert_x = !g_settings.air_invert_x;
                    break;
                case 3:
                    g_settings.air_invert_y = !g_settings.air_invert_y;
                    break;
                case 4:
                    g_settings.air_swap_xy = !g_settings.air_swap_xy;
                    break;
                case 5:
                    g_settings.air_natural_scroll = !g_settings.air_natural_scroll;
                    break;
                case 6:
                    g_settings.sound_volume += 20;
                    if (g_settings.sound_volume > 100) g_settings.sound_volume = 20;
                    sound_fx_apply_volume();
                    break;
                case 7:
                    g_settings.sound_muted = !g_settings.sound_muted;
                    sound_fx_apply_volume();
                    break;
                case 8:
                    g_settings.rotation = (g_settings.rotation == 0) ? 2 : 0;
                    M5.Display.setRotation(g_settings.rotation);
                    break;
                case 9:
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
        canvas.fillRect(0, 0, w, 22, 0x1D283A);
        canvas.setTextSize(1);
        canvas.setTextColor(0xFFFFFF, 0x1D283A);
        canvas.setCursor(6, 7);
        canvas.print("SETTINGS");

        char labels[kCount][32];
        std::snprintf(labels[0], 32, "EPI Int: %dm", g_settings.epi_interval_sec / 60);
        std::snprintf(labels[1], 32, "Mouse Sens: %d", g_settings.air_sensitivity);
        std::snprintf(labels[2], 32, "Invert X: %s", g_settings.air_invert_x ? "ON" : "OFF");
        std::snprintf(labels[3], 32, "Invert Y: %s", g_settings.air_invert_y ? "ON" : "OFF");
        std::snprintf(labels[4], 32, "Swap X/Y: %s", g_settings.air_swap_xy ? "ON" : "OFF");
        std::snprintf(labels[5], 32, "Nat Scroll:%s", g_settings.air_natural_scroll ? "ON" : "OFF");
        std::snprintf(labels[6], 32, "Volume: %d%%", g_settings.sound_volume);
        std::snprintf(labels[7], 32, "Mute: %s", g_settings.sound_muted ? "ON" : "OFF");
        std::snprintf(labels[8], 32, "Flip Scr: %s", g_settings.rotation == 2 ? "180" : "0");
        std::snprintf(labels[9], 32, "< BACK");

        int start_idx = (sel >= 8) ? (sel - 7) : 0;
        int y = 28;
        for (int i = start_idx; i < kCount && y < h - 18; i++) {
            bool is_sel = (i == sel);
            uint32_t bg = is_sel ? 0x1976D2 : 0x0B0F17;
            uint32_t fg = is_sel ? 0xFFFFFF : 0xCFD8DC;
            if (is_sel) canvas.fillRoundRect(4, y - 2, w - 8, 17, 3, bg);
            canvas.setTextColor(fg, bg);
            canvas.setCursor(8, y + 2);
            canvas.print(labels[i]);
            y += 19;
        }

        canvas.setTextColor(0x78909C, 0x0B0F17);
        canvas.setCursor(6, h - 14);
        canvas.print("A:Next B:Toggle A+B:Bk");
        canvas.pushSprite(0, 0);
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

static void draw_home_screen(M5Canvas &canvas, int sel) {
    int w = canvas.width();
    int h = canvas.height();
    canvas.fillScreen(0x090D14);

    // Header
    canvas.fillRect(0, 0, w, 24, 0x142033);
    canvas.setTextSize(1);
    canvas.setTextColor(0x80D8FF, 0x142033);
    canvas.setCursor(6, 8);
    canvas.print("STICKS3");

    if (g_settings.sound_muted) {
        canvas.setTextColor(0xFFAB40, 0x142033);
        canvas.setCursor(58, 8);
        canvas.print("MUTE");
    }

    int batt = M5.Power.getBatteryLevel();
    if (batt >= 0) {
        if (batt > 100) batt = 100;
        char bstr[16];
        std::snprintf(bstr, sizeof(bstr), "%d%%", batt);
        canvas.setTextColor(batt <= 20 ? 0xFF5252 : 0x69F0AE, 0x142033);
        canvas.setCursor(w - (int)std::strlen(bstr) * 6 - 6, 8);
        canvas.print(bstr);
    }

    struct MenuCard {
        const char *title;
        const char *sub;
        uint32_t accent;
    };
    static constexpr MenuCard kCards[4] = {
        {"EMS TIMER",     "EPI / Vital / Log", 0xD32F2F},
        {"AIR MOUSE BLE", "Bluetooth Gyro",    0x1976D2},
        {"AIR MOUSE USB", "USB-C HID Gyro",    0x00897B},
        {"SETTINGS",      "Sens / Sound / EPI",0x546E7A},
    };

    int y = 32;
    for (int i = 0; i < 4; i++) {
        bool is_sel = (i == sel);
        uint32_t bg = is_sel ? kCards[i].accent : 0x131B26;
        uint32_t border = is_sel ? 0xFFFFFF : 0x263244;
        canvas.fillRoundRect(6, y, w - 12, 42, 6, bg);
        canvas.drawRoundRect(6, y, w - 12, 42, 6, border);

        canvas.setTextSize(1);
        canvas.setTextColor(0xFFFFFF, bg);
        canvas.setCursor(14, y + 9);
        canvas.print(kCards[i].title);

        canvas.setTextColor(is_sel ? 0xE3F2FD : 0x8FA3B8, bg);
        canvas.setCursor(14, y + 24);
        canvas.print(kCards[i].sub);
        y += 47;
    }

    canvas.setTextColor(0x78909C, 0x090D14);
    canvas.setCursor(6, h - 14);
    canvas.print("A:Next B:Open HoldA:Mute");
    canvas.pushSprite(0, 0);
}

extern "C" void app_main(void) {
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    auto cfg = M5.config();
    M5.begin(cfg);

    app_settings_load();
    M5.Display.setRotation(g_settings.rotation);
    M5.Display.setBrightness(200);
    sound_fx_apply_volume();

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
        } else if (a_down && a_was_down && !a_long_fired && (now_ms - a_down_ms) >= 600) {
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
                ems_timer_ui_run();
            } else if (sel == 1) {
                airmouse_ui_run(AirMouseTransport::Ble);
            } else if (sel == 2) {
                airmouse_ui_run(AirMouseTransport::Usb);
            } else {
                run_settings_menu();
            }
            M5.Display.setRotation(g_settings.rotation);
            canvas.createSprite(M5.Display.width(), M5.Display.height());
            input_wait_all_release();
            last_draw_ms = 0;
            continue;
        }

        if ((now_ms - last_draw_ms) >= 200) {
            last_draw_ms = now_ms;
            draw_home_screen(canvas, sel);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
