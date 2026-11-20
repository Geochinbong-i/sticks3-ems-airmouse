#include "airmouse_ui.h"
#include "airmouse_ble.h"
#include "airmouse_usb.h"
#include "airmouse_math.h"
#include "app_settings.h"
#include "input_util.h"
#include "sound_fx.h"
#include "timer/ems_timer_ui.h"
#include "ui_common.h"

#include "M5Unified.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

void draw_airmouse_frame(M5Canvas &canvas,
                         AirMouseTransport transport,
                         bool connected,
                         bool suspended,
                         bool paused,
                         bool scroll_mode,
                         bool rel_pose_active,
                         uint8_t buttons,
                         float preview_x,
                         float preview_y,
                         int64_t now_ms) {
    int w = canvas.width();
    int h = canvas.height();
    bool ems_alarm = ems_timer_has_active_alarm();
    bool flash_on = ((now_ms / 400) % 2) == 0;

    canvas.fillScreen(0x0B0F17);

    uint32_t hdr_col = (ems_alarm && flash_on)
                         ? 0xB71C1C
                         : ((transport == AirMouseTransport::Ble) ? 0x123866 : 0x11493A);
    const char *hdr_title = (ems_alarm && flash_on)
                              ? "EMS DUE!"
                              : ((transport == AirMouseTransport::Ble) ? "BLE MOUSE" : "USB MOUSE");
    ui_draw_header(canvas, hdr_title, hdr_col, 0xFFFFFF);

    if (paused) {
        // Paused banner + Relative Calibration action card
        canvas.fillRoundRect(4, 26, w - 8, 22, 4, 0x8C5800);
        canvas.setFont(&fonts::Font2);
        canvas.setTextSize(1);
        canvas.setTextWrap(false);
        canvas.setTextColor(0xFFFFFF, 0x8C5800);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString("PAUSED", w / 2, 37);
        canvas.setTextDatum(TL_DATUM);

        canvas.drawRoundRect(5, 54, w - 10, 152, 6, 0xFFB300);
        canvas.fillRoundRect(6, 55, w - 12, 150, 5, 0x151209);

        canvas.setTextColor(0xFFD54F, 0x151209);
        canvas.setCursor(11, 61);
        canvas.print("RELATIVE ZERO");

        canvas.setTextColor(0xE2E8F0, 0x151209);
        canvas.setCursor(11, 82);
        canvas.print("Hold comfy pose,");
        canvas.setCursor(11, 98);
        canvas.print("tap B to zero.");

        canvas.fillRoundRect(10, 120, w - 20, 24, 4, 0x0E5E36);
        canvas.setTextColor(0xFFFFFF, 0x0E5E36);
        canvas.setCursor(14, 124);
        canvas.print("B : Rel Cal Zero");

        canvas.fillRoundRect(10, 148, w - 20, 24, 4, 0x1E3A5F);
        canvas.setTextColor(0xFFFFFF, 0x1E3A5F);
        canvas.setCursor(14, 152);
        canvas.print("A+B : Resume");

        canvas.fillRoundRect(10, 176, w - 20, 24, 4, 0x4A1C1C);
        canvas.setTextColor(0xFFCDD2, 0x4A1C1C);
        canvas.setCursor(14, 180);
        canvas.print("Hold A+B : Exit");

        ui_draw_footer2(canvas, "B: Zero Held Pose", "Hold A+B : Go Home");
        canvas.pushSprite(0, 0);
        return;
    }

    // Connection / scroll status pill
    uint32_t pill_col = 0x552222;
    const char *status_txt = "WAITING...";
    if (scroll_mode) {
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

    canvas.fillRoundRect(4, 26, w - 8, 22, 4, pill_col);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextColor(0xFFFFFF, pill_col);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString(status_txt, w / 2, 37);
    canvas.setTextDatum(TL_DATUM);

    // Virtual touchpad / crosshair box
    int bx = 6;
    int by = 52;
    int bw = w - 12;
    int bh = h - 108;
    canvas.fillRoundRect(bx, by, bw, bh, 6, 0x080C12);
    canvas.drawRoundRect(bx, by, bw, bh, 6, 0x2E3B4E);
    canvas.drawFastHLine(bx + 4, by + bh / 2, bw - 8, 0x1A2332);
    canvas.drawFastVLine(bx + bw / 2, by + 4, bh - 8, 0x1A2332);

    // Drift/Cal badge inside top-left of crosshair box
    const char *zbadge = rel_pose_active ? "REL+ABS"
                       : (g_settings.imu_abs_valid ? "ABS OK" : "AUTO");
    canvas.fillRoundRect(bx + 4, by + 4, 58, 16, 3, 0x11262E);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(0x4DD0E1, 0x11262E);
    canvas.setCursor(bx + 7, by + 4);
    canvas.print(zbadge);

    int cx = bx + (int)(preview_x * (float)bw);
    int cy = by + (int)(preview_y * (float)bh);
    if (cx < bx + 10) cx = bx + 10;
    if (cx > bx + bw - 10) cx = bx + bw - 10;
    if (cy < by + 10) cy = by + 10;
    if (cy > by + bh - 10) cy = by + bh - 10;

    uint32_t cur_col = 0x00E5FF;
    if (scroll_mode) cur_col = 0x66D9FF;
    else if (buttons & 0x01) cur_col = 0xFF4444;
    else if (buttons & 0x02) cur_col = 0xFFBB33;

    canvas.drawCircle(cx, cy, 8, cur_col);
    canvas.drawLine(cx - 11, cy, cx + 11, cy, cur_col);
    canvas.drawLine(cx, cy - 11, cx, cy + 11, cur_col);
    canvas.fillCircle(cx, cy, 2, cur_col);

    // Sensitivity & LMB/RMB badges row
    int sy = by + bh + 4;
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(0xB0C4D8, 0x0B0F17);
    canvas.setCursor(6, sy + 2);
    canvas.printf("SENS:%d", g_settings.air_sensitivity);

    uint32_t lmb_bg = (buttons & 0x01) ? 0xCC2222 : 0x1D2633;
    uint32_t rmb_bg = (buttons & 0x02) ? 0xD98200 : 0x1D2633;
    canvas.fillRoundRect(w - 72, sy, 33, 19, 3, lmb_bg);
    canvas.setTextColor(0xFFFFFF, lmb_bg);
    canvas.setCursor(w - 69, sy + 1);
    canvas.print("LMB");

    canvas.fillRoundRect(w - 36, sy, 32, 19, 3, rmb_bg);
    canvas.setTextColor(0xFFFFFF, rmb_bg);
    canvas.setCursor(w - 33, sy + 1);
    canvas.print("RMB");

    ui_draw_footer2(canvas, "A: Click  B: Right", "Hold B:Scr AB:Menu");
    canvas.pushSprite(0, 0);
}

void perform_relative_zero(M5Canvas &canvas, AirMouseTransport transport,
                           GyroAirMouseFilter &filter) {
    int w = canvas.width();
    uint32_t hdr_col = (transport == AirMouseTransport::Ble) ? 0x123866 : 0x11493A;
    const char *hdr_title = (transport == AirMouseTransport::Ble) ? "BLE MOUSE" : "USB MOUSE";

    // Sample 20 frames (~240ms) in user's comfortable held hand position
    double sum_ax = 0, sum_ay = 0, sum_az = 0;
    double sum_gx = 0, sum_gy = 0, sum_gz = 0;
    float min_gx = 1e6f, min_gy = 1e6f, min_gz = 1e6f;
    float max_gx = -1e6f, max_gy = -1e6f, max_gz = -1e6f;
    int valid = 0;

    for (int i = 0; i < 20; i++) {
        M5.Imu.update();
        auto d = M5.Imu.getImuData();
        if (std::isfinite(d.accel.x) && std::isfinite(d.accel.y) && std::isfinite(d.accel.z) &&
            std::isfinite(d.gyro.x) && std::isfinite(d.gyro.y) && std::isfinite(d.gyro.z)) {
            sum_ax += d.accel.x; sum_ay += d.accel.y; sum_az += d.accel.z;
            sum_gx += d.gyro.x; sum_gy += d.gyro.y; sum_gz += d.gyro.z;
            min_gx = std::min(min_gx, d.gyro.x); max_gx = std::max(max_gx, d.gyro.x);
            min_gy = std::min(min_gy, d.gyro.y); max_gy = std::max(max_gy, d.gyro.y);
            min_gz = std::min(min_gz, d.gyro.z); max_gz = std::max(max_gz, d.gyro.z);
            valid++;
        }
        vTaskDelay(pdMS_TO_TICKS(12));
    }

    if (valid > 0) {
        float ax = (float)(sum_ax / valid);
        float ay = (float)(sum_ay / valid);
        float az = (float)(sum_az / valid);
        float abs_az = g_settings.imu_abs_valid ? g_settings.imu_abs_az : 1.0f;
        filter.set_relative_pose(ax, ay, az, true, abs_az);

        float spread = std::max({max_gx - min_gx, max_gy - min_gy, max_gz - min_gz});
        if (spread < 4.0f) {
            filter.reset_bias((float)(sum_gx / valid),
                              (float)(sum_gy / valid),
                              (float)(sum_gz / valid));
        }
    }

    canvas.fillScreen(0x0B0F17);
    ui_draw_header(canvas, hdr_title, hdr_col, 0xFFFFFF);

    canvas.drawRoundRect(6, 42, w - 12, 140, 7, 0x00E5FF);
    canvas.fillRoundRect(7, 43, w - 14, 138, 6, 0x0B1E28);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextColor(0x00E5FF, 0x0B1E28);
    canvas.setCursor(14, 52);
    canvas.print("REL ZERO OK!");

    canvas.setTextColor(0xE0F7FA, 0x0B1E28);
    canvas.setCursor(14, 80);
    canvas.print("Held angle is now");
    canvas.setCursor(14, 96);
    canvas.print("the neutral zero.");

    canvas.fillRoundRect(14, 126, w - 28, 8, 3, 0x00E5FF);
    canvas.setTextColor(0x69F0AE, 0x0B1E28);
    canvas.setCursor(14, 146);
    canvas.print("Resuming cursor...");

    ui_draw_footer2(canvas, "A: Click  B: Right", "Hold B:Scr AB:Menu");
    canvas.pushSprite(0, 0);

    sound_fx_confirm();
    vTaskDelay(pdMS_TO_TICKS(420));
    input_wait_all_release();
}

void show_usb_exit_notice(M5Canvas &canvas) {
    int w = canvas.width();
    canvas.fillScreen(0x0B0F17);
    ui_draw_header(canvas, "USB HID", 0x11493A, 0xFFFFFF);

    canvas.drawRoundRect(6, 36, w - 12, 152, 7, 0x26A69A);
    canvas.fillRoundRect(7, 37, w - 14, 150, 6, 0x0D191B);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextColor(0x80CBC4, 0x0D191B);
    canvas.setCursor(13, 46);
    canvas.print("USB HID PAUSED");

    canvas.setTextColor(0xCFD8DC, 0x0D191B);
    canvas.setCursor(13, 74);
    canvas.print("USB-OTG is active.");
    canvas.setCursor(13, 92);
    canvas.print("Reboot in Settings");
    canvas.setCursor(13, 110);
    canvas.print("for USB Serial log.");

    canvas.fillRoundRect(13, 144, w - 26, 28, 4, 0x143D38);
    canvas.setTextColor(0x80CBC4, 0x143D38);
    canvas.setCursor(18, 150);
    canvas.print("A or B : Return");

    ui_draw_footer2(canvas, "Press A or B: Home", "Auto close in 1.3s");
    canvas.pushSprite(0, 0);

    int64_t until_ms = input_now_ms() + 1300;
    while (input_now_ms() < until_ms) {
        input_update();
        if (input_a_pressed() || input_b_pressed()) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    input_wait_all_release();
}

} // namespace

void airmouse_ui_run(AirMouseTransport transport) {
    if (!M5.Imu.isEnabled()) {
        M5.Display.fillScreen(0x000000);
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(0xFF4444, 0x000000);
        M5.Display.setCursor(8, 80);
        M5.Display.println("IMU not found!");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    bool ok = (transport == AirMouseTransport::Ble) ? airmouse_ble_start() : airmouse_usb_start();
    if (!ok) {
        M5.Display.fillScreen(0x000000);
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(0xFF4444, 0x000000);
        M5.Display.setCursor(8, 80);
        M5.Display.println(transport == AirMouseTransport::Ble ? "BLE init fail" : "USB init fail");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());

    GyroAirMouseFilter filter;
    if (g_settings.imu_abs_valid) {
        // Use clean flat-surface NVS calibration directly (never corrupt bias with button-press jerk)
        filter.reset_bias(g_settings.imu_abs_gx, g_settings.imu_abs_gy, g_settings.imu_abs_gz);
    } else {
        // No NVS abs calibration yet: do a brief settle ONLY accepting samples when steady
        filter.calib_begin();
        for (int i = 0; i < 20; i++) {
            M5.Imu.update();
            auto d = M5.Imu.getImuData();
            if (std::fabs(d.gyro.x) < 12.0f && std::fabs(d.gyro.y) < 12.0f && std::fabs(d.gyro.z) < 12.0f) {
                filter.calib_add_sample(d.gyro.x, d.gyro.y, d.gyro.z);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        filter.calib_finish();
    }
    sound_fx_confirm();
    input_wait_all_release();

    AirMouseButtonGuard btn_guard;
    uint8_t prev_buttons = 0;
    bool was_connected = false;
    bool rel_pose_active = false;
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

        if (bst.rel_cal_requested) {
            perform_relative_zero(canvas, transport, filter);
            rel_pose_active = true;
            preview_x = 0.5f;
            preview_y = 0.5f;
            btn_guard.set_paused(false);
            last_us = esp_timer_get_time();
            last_draw_ms = 0;
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        float dt = (float)(now_us - last_us) / 1000000.0f;
        last_us = now_us;

        M5.Imu.update();
        auto imu = M5.Imu.getImuData();

        AirMouseConfig cfg;
        cfg.sensitivity = g_settings.air_sensitivity;
        cfg.invert_x = g_settings.air_invert_x;
        cfg.invert_y = g_settings.air_invert_y;
        cfg.swap_xy = g_settings.air_swap_xy;
        cfg.natural_scroll = g_settings.air_natural_scroll;
        cfg.flip_180 = (g_settings.rotation == 2);

        AirMouseDelta d = filter.update(imu.gyro.x, imu.gyro.y, imu.gyro.z,
                                        dt, cfg, bst.scroll_mode, bst.paused);

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
                                bst.paused, bst.scroll_mode, rel_pose_active,
                                bst.buttons, preview_x, preview_y, now_ms);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
