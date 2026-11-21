#include "airmouse_ui.h"
#include "airmouse_ble.h"
#include "airmouse_usb.h"
#include "airmouse_math.h"
#include "app_settings.h"
#include "input_util.h"
#include "imu_cal.h"
#include "sound_fx.h"
#include "timer/ems_timer_ui.h"
#include "ui_common.h"
#include "ui_theme.h"
#include "time_sync.h"

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
    const auto &theme = ui_theme();
    int w = canvas.width();
    int h = canvas.height();
    const auto alarms = ems_timer_active_alarms();
    const bool ems_alarm = alarms.any();
    bool flash_on = ((now_ms / 400) % 2) == 0;

    canvas.fillScreen(theme.background);

    uint32_t hdr_col = (ems_alarm && flash_on)
                         ? 0xB71C1Cu
                         : theme.surface;
    const char *hdr_title = (ems_alarm && flash_on)
                              ? alarms.title()
                              : ((transport == AirMouseTransport::Ble) ? "BLE MOUSE" : "USB MOUSE");
    ui_draw_header(canvas, hdr_title, hdr_col, ems_alarm && flash_on ? 0xFFFFFFu : theme.text);

    if (paused) {
        // Paused banner + Relative Calibration action card
        canvas.fillRoundRect(4, 26, w - 8, 22, 4, theme.alternate);
        canvas.setFont(&fonts::Font2);
        canvas.setTextSize(1);
        canvas.setTextWrap(false);
        canvas.setTextColor(theme.warning, theme.alternate);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString("PAUSED", w / 2, 37);
        canvas.setTextDatum(TL_DATUM);

        canvas.drawRoundRect(5, 54, w - 10, 152, 6, theme.warning);
        canvas.fillRoundRect(6, 55, w - 12, 150, 5, theme.surface);

        canvas.setTextColor(theme.warning, theme.surface);
        canvas.setCursor(11, 61);
        canvas.print("RELATIVE ZERO");

        canvas.setTextColor(theme.text, theme.surface);
        canvas.setCursor(11, 82);
        canvas.print("Hold comfy pose,");
        canvas.setCursor(11, 98);
        canvas.print("tap B to zero.");

        canvas.fillRoundRect(10, 120, w - 20, 24, 4, theme.selected);
        canvas.setTextColor(theme.on_selected, theme.selected);
        canvas.setCursor(14, 124);
        canvas.print("B : Rel Cal Zero");

        canvas.fillRoundRect(10, 148, w - 20, 24, 4, theme.alternate);
        canvas.setTextColor(theme.text, theme.alternate);
        canvas.setCursor(14, 152);
        canvas.print("A+B : Resume");

        canvas.fillRoundRect(10, 176, w - 20, 24, 4, theme.alternate);
        canvas.setTextColor(theme.danger, theme.alternate);
        canvas.setCursor(14, 180);
        canvas.print("Hold A+B : Exit");

        ui_draw_themed_footer(canvas, "B: Zero Held Pose", "Hold A+B : Go Home");
        ui_present(canvas);
        return;
    }

    // Connection / scroll status pill
    uint32_t pill_fg = theme.danger;
    const uint32_t pill_col = theme.alternate;
    const char *status_txt = "WAITING...";
    if (scroll_mode) {
        pill_fg = theme.accent;
        status_txt = "SCROLL MODE";
    } else if (connected) {
        if (suspended) {
            pill_fg = theme.warning;
            status_txt = "USB SUSPENDED";
        } else {
            pill_fg = theme.text;
            status_txt = (transport == AirMouseTransport::Ble) ? "BLE CONNECTED" : "USB CONNECTED";
        }
    } else {
        status_txt = (transport == AirMouseTransport::Ble) ? "ADVERTISING..." : "PLUG USB-C PC";
    }

    canvas.fillRoundRect(4, 26, w - 8, 22, 4, pill_col);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextColor(pill_fg, pill_col);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString(status_txt, w / 2, 37);
    canvas.setTextDatum(TL_DATUM);

    // Virtual touchpad / crosshair box
    int bx = 6;
    int by = 52;
    int bw = w - 12;
    int bh = h - 108;
    canvas.fillRoundRect(bx, by, bw, bh, 6, theme.surface);
    canvas.drawRoundRect(bx, by, bw, bh, 6, theme.border);
    canvas.drawFastHLine(bx + 4, by + bh / 2, bw - 8, theme.border);
    canvas.drawFastVLine(bx + bw / 2, by + 4, bh - 8, theme.border);

    // Drift/Cal badge inside top-left of crosshair box
    const char *zbadge = rel_pose_active ? "REL+ABS"
                       : (g_settings.imu_abs_valid ? "ABS OK" : "AUTO");
    canvas.fillRoundRect(bx + 4, by + 4, 58, 16, 3, theme.alternate);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(theme.accent, theme.alternate);
    canvas.setCursor(bx + 7, by + 4);
    canvas.print(zbadge);

    int cx = bx + (int)(preview_x * (float)bw);
    int cy = by + (int)(preview_y * (float)bh);
    if (cx < bx + 10) cx = bx + 10;
    if (cx > bx + bw - 10) cx = bx + bw - 10;
    if (cy < by + 10) cy = by + 10;
    if (cy > by + bh - 10) cy = by + bh - 10;

    uint32_t cur_col = theme.accent;
    if (!scroll_mode && (buttons & 0x01)) cur_col = theme.danger;
    else if (!scroll_mode && (buttons & 0x02)) cur_col = theme.warning;

    canvas.drawCircle(cx, cy, 8, cur_col);
    canvas.drawLine(cx - 11, cy, cx + 11, cy, cur_col);
    canvas.drawLine(cx, cy - 11, cx, cy + 11, cur_col);
    canvas.fillCircle(cx, cy, 2, cur_col);

    // Sensitivity & LMB/RMB badges row
    int sy = by + bh + 4;
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(theme.muted, theme.background);
    canvas.setCursor(6, sy + 2);
    canvas.printf("SENS:%d", g_settings.air_sensitivity);

    const bool left = buttons & 0x01, right = buttons & 0x02;
    const uint32_t lmb_bg = left ? theme.selected : theme.alternate;
    const uint32_t rmb_bg = right ? theme.selected : theme.alternate;
    canvas.fillRoundRect(w - 72, sy, 33, 19, 3, lmb_bg);
    canvas.setTextColor(left ? theme.on_selected : theme.muted, lmb_bg);
    canvas.setCursor(w - 69, sy + 1);
    canvas.print("LMB");

    canvas.fillRoundRect(w - 36, sy, 32, 19, 3, rmb_bg);
    canvas.setTextColor(right ? theme.on_selected : theme.muted, rmb_bg);
    canvas.setCursor(w - 33, sy + 1);
    canvas.print("RMB");

    ui_draw_themed_footer(canvas, "A: Click  B: Right", "Hold B:Scr AB:Menu");
    ui_present(canvas);
}

void perform_relative_zero(M5Canvas &canvas, AirMouseTransport transport,
                           GyroAirMouseFilter &filter) {
    const auto &theme = ui_theme();
    int w = canvas.width();
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

    canvas.fillScreen(theme.background);
    ui_draw_header(canvas, hdr_title, theme.surface, theme.text);

    canvas.drawRoundRect(6, 42, w - 12, 140, 7, theme.accent);
    canvas.fillRoundRect(7, 43, w - 14, 138, 6, theme.surface);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextColor(theme.accent, theme.surface);
    canvas.setCursor(14, 52);
    canvas.print("REL ZERO OK!");

    canvas.setTextColor(theme.text, theme.surface);
    canvas.setCursor(14, 80);
    canvas.print("Held angle is now");
    canvas.setCursor(14, 96);
    canvas.print("the neutral zero.");

    canvas.fillRoundRect(14, 126, w - 28, 8, 3, theme.accent);
    canvas.setTextColor(theme.accent, theme.surface);
    canvas.setCursor(14, 146);
    canvas.print("Resuming cursor...");

    ui_draw_themed_footer(canvas, "A: Click  B: Right", "Hold B:Scr AB:Menu");
    ui_present(canvas);

    sound_fx_confirm();
    vTaskDelay(pdMS_TO_TICKS(420));
    input_wait_all_release();
}

} // namespace

void airmouse_ui_run(AirMouseTransport transport) {
    const auto &theme = ui_theme();
    if (!M5.Imu.isEnabled()) {
        M5.Display.fillScreen(theme.background);
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(theme.danger, theme.background);
        M5.Display.setCursor(8, 80);
        M5.Display.println("IMU not found!");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    if (transport == AirMouseTransport::Ble) time_sync_cancel_boot_ble();
    bool ok = transport == AirMouseTransport::Ble ? airmouse_ble_start() : airmouse_usb_start();
    if (ok && transport == AirMouseTransport::Ble) {
        const int64_t deadline = input_now_ms() + 5000;
        while (airmouse_ble_status() == BleStatus::Starting && input_now_ms() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        const BleStatus status = airmouse_ble_status();
        ok = status == BleStatus::Advertising || status == BleStatus::Connected;
        if (!ok) airmouse_ble_stop();
    }
    if (!ok) {
        M5.Display.fillScreen(theme.background);
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(theme.danger, theme.background);
        M5.Display.setCursor(8, 80);
        M5.Display.println(transport == AirMouseTransport::Ble ? "BLE init fail" : "USB init fail");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());

    GyroAirMouseFilter filter;
    bool needs_initial_calibration = !g_settings.imu_abs_valid;
    if (!needs_initial_calibration) {
        filter.reset_bias(g_settings.imu_abs_gx, g_settings.imu_abs_gy, g_settings.imu_abs_gz);
    }
    sound_fx_confirm();
    input_wait_all_release();

    AirMouseButtonGuard btn_guard;
    uint8_t prev_buttons = 0;
    bool was_connected = false;
    bool imu_active = false;
    int64_t imu_retry_ms = 0;
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
            imu_set_idle();
            if (transport == AirMouseTransport::Ble) {
                airmouse_ble_stop();
            } else {
                airmouse_usb_stop();
            }
            sound_fx_click();
            input_wait_ab_release();
            canvas.deleteSprite();
            return;
        }

        if (bst.clicked_cue) {
            sound_fx_click();
        }

        if (bst.rel_cal_requested && imu_set_active()) {
            imu_active = true;
            perform_relative_zero(canvas, transport, filter);
            rel_pose_active = true;
            preview_x = 0.5f;
            preview_y = 0.5f;
            btn_guard.set_paused(false);
            last_us = esp_timer_get_time();
            last_draw_ms = 0;
            continue;
        }

        const bool connected = (transport == AirMouseTransport::Ble)
            ? airmouse_ble_is_connected() : airmouse_usb_is_connected();
        const bool suspended = (transport == AirMouseTransport::Usb) && airmouse_usb_is_suspended();
        const bool track_motion = connected && !suspended && !bst.paused;
        if (track_motion && !imu_active && now_ms >= imu_retry_ms) {
            imu_active = imu_set_active();
            imu_retry_ms = now_ms + 500;
            if (imu_active && needs_initial_calibration) {
                filter.calib_begin();
                for (int i = 0; i < 20; i++) {
                    M5.Imu.update();
                    const auto &sample = M5.Imu.getImuData();
                    if (std::fabs(sample.gyro.x) < 12.0f && std::fabs(sample.gyro.y) < 12.0f &&
                        std::fabs(sample.gyro.z) < 12.0f) {
                        filter.calib_add_sample(sample.gyro.x, sample.gyro.y, sample.gyro.z);
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                filter.calib_finish();
                needs_initial_calibration = false;
            }
            last_us = esp_timer_get_time();
        } else if (!track_motion && imu_active) {
            imu_set_idle();
            imu_active = false;
            imu_retry_ms = 0;
            filter.reset_motion();
        }

        AirMouseDelta d{};
        if (track_motion && imu_active) {
            const int64_t now_us = esp_timer_get_time();
            const float dt = (float)(now_us - last_us) / 1000000.0f;
            last_us = now_us;

            M5.Imu.update();
            const auto &imu = M5.Imu.getImuData();
            AirMouseConfig cfg;
            cfg.sensitivity = g_settings.air_sensitivity;
            cfg.invert_x = g_settings.air_invert_x;
            cfg.invert_y = g_settings.air_invert_y;
            cfg.swap_xy = g_settings.air_swap_xy;
            cfg.natural_scroll = g_settings.air_natural_scroll;
            cfg.flip_180 = (g_settings.rotation == 2);
            d = filter.update(imu.gyro.x, imu.gyro.y, imu.gyro.z, dt, cfg, bst.scroll_mode, false);

            if (!bst.scroll_mode && ui_screen_is_on()) {
                preview_x = std::clamp(preview_x + (float)d.dx / 260.0f, 0.05f, 0.95f);
                preview_y = std::clamp(preview_y + (float)d.dy / 260.0f, 0.05f, 0.95f);
            }
        }

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
                prev_buttons = bst.buttons;
            } else if (airmouse_usb_send(bst.buttons, d.dx, d.dy, d.wheel)) {
                prev_buttons = bst.buttons;
            }
        } else if (!connected) {
            prev_buttons = 0;
        }

        if (ui_screen_is_on() && (now_ms - last_draw_ms) >= 40) {
            last_draw_ms = now_ms;
            draw_airmouse_frame(canvas, transport, connected, suspended,
                                bst.paused, bst.scroll_mode, rel_pose_active,
                                bst.buttons, preview_x, preview_y, now_ms);
        }

        if (connected && !suspended) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            input_idle_delay(20);
        }
    }
}
