#include "airmouse_ui.h"
#include "airmouse_ble.h"
#include "airmouse_usb.h"
#include "airmouse_math.h"
#include "app_settings.h"
#include "input_util.h"
#include "sound_fx.h"
#include "timer/ems_timer_ui.h"

#include "M5Unified.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static void draw_airmouse_frame(M5Canvas &canvas,
                                AirMouseTransport transport,
                                bool connected,
                                bool suspended,
                                bool paused,
                                bool scroll_mode,
                                uint8_t buttons,
                                float preview_x,
                                float preview_y,
                                int64_t now_ms) {
    int w = canvas.width();
    int h = canvas.height();
    bool ems_alarm = ems_timer_has_active_alarm();
    bool flash_on = ((now_ms / 400) % 2) == 0;

    canvas.fillScreen(0x0B0F17);

    // Header bar
    uint32_t hdr_col = (ems_alarm && flash_on)
                         ? 0xB71C1C
                         : ((transport == AirMouseTransport::Ble) ? 0x123866 : 0x11493A);
    canvas.fillRect(0, 0, w, 24, hdr_col);
    canvas.setTextSize(1);
    canvas.setTextColor(0xFFFFFF, hdr_col);
    canvas.setCursor(6, 8);
    if (ems_alarm && flash_on) {
        canvas.print("EMS ALARM DUE!");
    } else {
        canvas.print(transport == AirMouseTransport::Ble ? "AIR MOUSE BLE" : "AIR MOUSE USB");
    }

    int batt = M5.Power.getBatteryLevel();
    if (batt >= 0) {
        if (batt > 100) batt = 100;
        char bstr[16];
        std::snprintf(bstr, sizeof(bstr), "%d%%", batt);
        canvas.setCursor(w - (int)std::strlen(bstr) * 6 - 6, 8);
        canvas.print(bstr);
    }

    // Connection pill
    uint32_t pill_col = 0x552222;
    const char *status_txt = "WAITING...";
    if (paused) {
        pill_col = 0x7A5500;
        status_txt = "PAUSED (CLUTCH)";
    } else if (scroll_mode) {
        pill_col = 0x145B7A;
        status_txt = "SCROLL MODE";
    } else if (connected) {
        if (suspended) {
            pill_col = 0x554400;
            status_txt = "USB SUSPENDED";
        } else {
            pill_col = 0x0E5E36;
            status_txt = (transport == AirMouseTransport::Ble) ? "BLE CONNECTED" : "USB CONNECTED";
        }
    } else {
        status_txt = (transport == AirMouseTransport::Ble) ? "ADVERTISING..." : "PLUG USB-C PC";
    }
    canvas.fillRoundRect(6, 30, w - 12, 22, 4, pill_col);
    canvas.setTextColor(0xFFFFFF, pill_col);
    int sw = (int)std::strlen(status_txt) * 6;
    canvas.setCursor((w - sw) / 2, 37);
    canvas.print(status_txt);

    // Virtual touchpad / crosshair box
    int bx = 8;
    int by = 60;
    int bw = w - 16;
    int bh = h - 134;
    canvas.drawRoundRect(bx, by, bw, bh, 6, 0x2E3B4E);
    canvas.drawFastHLine(bx + 4, by + bh / 2, bw - 8, 0x1A2332);
    canvas.drawFastVLine(bx + bw / 2, by + 4, bh - 8, 0x1A2332);

    int cx = bx + (int)(preview_x * (float)bw);
    int cy = by + (int)(preview_y * (float)bh);
    if (cx < bx + 8) cx = bx + 8;
    if (cx > bx + bw - 8) cx = bx + bw - 8;
    if (cy < by + 8) cy = by + 8;
    if (cy > by + bh - 8) cy = by + bh - 8;

    uint32_t cur_col = 0x00E5FF;
    if (paused) cur_col = 0xFFAA00;
    else if (scroll_mode) cur_col = 0x66D9FF;
    else if (buttons & 0x01) cur_col = 0xFF4444;
    else if (buttons & 0x02) cur_col = 0xFFBB33;

    canvas.drawCircle(cx, cy, 8, cur_col);
    canvas.drawLine(cx - 11, cy, cx + 11, cy, cur_col);
    canvas.drawLine(cx, cy - 11, cx, cy + 11, cur_col);
    canvas.fillCircle(cx, cy, 2, cur_col);

    // Sensitivity & Button state row
    int sy = by + bh + 6;
    canvas.setTextColor(0xAAB8C8, 0x0B0F17);
    canvas.setCursor(8, sy);
    canvas.printf("SENS:%d", g_settings.air_sensitivity);

    canvas.fillRoundRect(58, sy - 2, 34, 14, 3, (buttons & 0x01) ? 0xCC2222 : 0x1D2633);
    canvas.setTextColor(0xFFFFFF, (buttons & 0x01) ? 0xCC2222 : 0x1D2633);
    canvas.setCursor(66, sy + 1);
    canvas.print("LMB");

    canvas.fillRoundRect(96, sy - 2, 31, 14, 3, (buttons & 0x02) ? 0xD98200 : 0x1D2633);
    canvas.setTextColor(0xFFFFFF, (buttons & 0x02) ? 0xD98200 : 0x1D2633);
    canvas.setCursor(102, sy + 1);
    canvas.print("RMB");

    // Controls legend
    canvas.setTextColor(0x7E8C9F, 0x0B0F17);
    canvas.setCursor(6, h - 42);
    canvas.print("A: LMB   B tap: RMB");
    canvas.setCursor(6, h - 30);
    canvas.print("B hold: Scroll wheel");
    canvas.setCursor(6, h - 18);
    canvas.print("A+B tap:Pause hold:Ex");

    canvas.pushSprite(0, 0);
}

static void show_usb_exit_notice(M5Canvas &canvas) {
    canvas.fillScreen(0x0B0F17);
    canvas.drawRoundRect(8, 56, canvas.width() - 16, 110, 6, 0x26A69A);
    canvas.setTextSize(1);
    canvas.setTextColor(0x80CBC4, 0x0B0F17);
    canvas.setCursor(18, 72);
    canvas.print("USB HID PAUSED");
    canvas.setTextColor(0xCFD8DC, 0x0B0F17);
    canvas.setCursor(16, 94);
    canvas.print("USB-OTG active.");
    canvas.setCursor(16, 110);
    canvas.print("Reboot in Settings");
    canvas.setCursor(16, 126);
    canvas.print("for USB Serial.");
    canvas.pushSprite(0, 0);

    int64_t until_ms = input_now_ms() + 1300;
    while (input_now_ms() < until_ms) {
        input_update();
        if (input_a_pressed() || input_b_pressed()) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    input_wait_all_release();
}

void airmouse_ui_run(AirMouseTransport transport) {
    if (!M5.Imu.isEnabled()) {
        M5.Display.fillScreen(0x000000);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0xFF4444, 0x000000);
        M5.Display.setCursor(10, 80);
        M5.Display.println("IMU not available!");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    bool ok = (transport == AirMouseTransport::Ble) ? airmouse_ble_start() : airmouse_usb_start();
    if (!ok) {
        M5.Display.fillScreen(0x000000);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0xFF4444, 0x000000);
        M5.Display.setCursor(10, 80);
        M5.Display.println(transport == AirMouseTransport::Ble ? "BLE HID init fail" : "USB HID init fail");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());

    // Quick 0.8s gyro bias calibration
    GyroAirMouseFilter filter;
    filter.calib_begin();
    canvas.fillScreen(0x0B0F17);
    canvas.setTextSize(1);
    canvas.setTextColor(0x00E5FF, 0x0B0F17);
    canvas.setCursor(14, 90);
    canvas.print("CALIBRATING GYRO...");
    canvas.setTextColor(0xAAAAAA, 0x0B0F17);
    canvas.setCursor(18, 108);
    canvas.print("Hold steady 0.8s");
    canvas.pushSprite(0, 0);

    for (int i = 0; i < 40; i++) {
        float gx = 0, gy = 0, gz = 0;
        if (M5.Imu.getGyro(&gx, &gy, &gz)) {
            filter.calib_add_sample(gx, gy, gz);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    filter.calib_finish();
    sound_fx_confirm();

    input_wait_all_release();

    AirMouseButtonGuard btn_guard;
    uint8_t prev_buttons = 0;
    bool was_connected = false;
    int last_ble_batt = -1;
    int64_t last_batt_ms = 0;

    float preview_x = 0.5f;
    float preview_y = 0.5f;
    int64_t last_us = esp_timer_get_time();
    int64_t last_draw_ms = 0;

    while (true) {
        input_update();
        int64_t now_ms = input_now_ms();
        AirMouseBtnState bst = btn_guard.step(now_ms, input_a_down(), input_b_down());

        if (bst.exit_requested) {
            if (transport == AirMouseTransport::Ble) {
                airmouse_ble_stop();
            } else {
                airmouse_usb_stop();
            }
            sound_fx_click();
            input_wait_ab_release();
            if (transport == AirMouseTransport::Usb && airmouse_usb_was_inited()) {
                show_usb_exit_notice(canvas);
            }
            canvas.deleteSprite();
            return;
        }

        if (bst.clicked_cue) {
            sound_fx_click();
        }

        int64_t now_us = esp_timer_get_time();
        float dt = (float)(now_us - last_us) / 1000000.0f;
        last_us = now_us;

        float gx = 0, gy = 0, gz = 0;
        M5.Imu.getGyro(&gx, &gy, &gz);

        AirMouseConfig cfg;
        cfg.sensitivity = g_settings.air_sensitivity;
        cfg.invert_x = g_settings.air_invert_x;
        cfg.invert_y = g_settings.air_invert_y;
        cfg.swap_xy = g_settings.air_swap_xy;
        cfg.natural_scroll = g_settings.air_natural_scroll;
        cfg.flip_180 = (g_settings.rotation == 2);

        AirMouseDelta d = filter.update(gx, gy, gz, dt, cfg, bst.scroll_mode, bst.paused);

        if (!bst.paused && !bst.scroll_mode) {
            preview_x += (float)d.dx / 260.0f;
            preview_y += (float)d.dy / 260.0f;
            if (preview_x < 0.05f) preview_x = 0.05f;
            if (preview_x > 0.95f) preview_x = 0.95f;
            if (preview_y < 0.05f) preview_y = 0.05f;
            if (preview_y > 0.95f) preview_y = 0.95f;
        }

        bool connected = (transport == AirMouseTransport::Ble)
                             ? airmouse_ble_is_connected()
                             : airmouse_usb_is_connected();
        bool suspended = (transport == AirMouseTransport::Usb) && airmouse_usb_is_suspended();

        if (transport == AirMouseTransport::Ble && connected) {
            if (!was_connected || (now_ms - last_batt_ms) >= 10000) {
                last_batt_ms = now_ms;
                int batt = M5.Power.getBatteryLevel();
                if (batt >= 0 && (!was_connected || batt != last_ble_batt)) {
                    last_ble_batt = batt;
                    airmouse_ble_update_battery(batt);
                }
            }
        }
        was_connected = connected;

        if (connected && (d.dx != 0 || d.dy != 0 || d.wheel != 0 || bst.buttons != prev_buttons)) {
            if (transport == AirMouseTransport::Ble) {
                airmouse_ble_send(bst.buttons, d.dx, d.dy, d.wheel);
            } else {
                airmouse_usb_send(bst.buttons, d.dx, d.dy, d.wheel);
            }
            prev_buttons = bst.buttons;
        } else if (!connected) {
            prev_buttons = 0;
        }

        if ((now_ms - last_draw_ms) >= 40) {
            last_draw_ms = now_ms;
            draw_airmouse_frame(canvas, transport, connected, suspended,
                                bst.paused, bst.scroll_mode, bst.buttons,
                                preview_x, preview_y, now_ms);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
