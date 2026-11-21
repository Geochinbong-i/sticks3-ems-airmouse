#include "imu_cal.h"
#include "app_settings.h"
#include "input_util.h"
#include "sound_fx.h"
#include "ui_common.h"

#include "M5Unified.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// BMI270 power controls. Keep the uploaded configuration while the sensors stop.
constexpr uint8_t kBmiChipId = 0x24;
constexpr uint8_t kBmiPowerConf = 0x7C;
constexpr uint8_t kBmiPowerCtrl = 0x7D;
constexpr uint32_t kBmiClock = 400000;
uint8_t s_bmi_address = 0;
bool s_bmi_idle = false;

void find_internal_bmi270() {
    if (M5.getBoard() != m5::board_t::board_M5StickS3 ||
        M5.Imu.getType() != m5::imu_bmi270) return;
    for (const uint8_t address : {0x69, 0x68}) {
        uint8_t id = 0;
        if (M5.In_I2C.readRegister(address, 0x00, &id, 1, kBmiClock) && id == kBmiChipId) {
            s_bmi_address = address;
            return;
        }
    }
}


enum class CalViewState {
    Ready,
    Done,
    FailedMove,
};

void draw_abs_cal_idle(M5Canvas &canvas, CalViewState vst, float last_spread) {
    int w = canvas.width();
    canvas.fillScreen(0x0B0F17);
    ui_draw_header(canvas, "IMU ABS CAL", 0x1D283A, 0x80D8FF);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);

    if (vst == CalViewState::Done) {
        canvas.drawRoundRect(6, 30, w - 12, 134, 7, 0x00E676);
        canvas.fillRoundRect(7, 31, w - 14, 132, 6, 0x0B2217);

        canvas.setTextColor(0x69F0AE, 0x0B2217);
        canvas.setCursor(13, 38);
        canvas.print("ABS CAL SAVED!");

        canvas.setTextColor(0xE8F5E9, 0x0B2217);
        canvas.setCursor(13, 62);
        canvas.print("Offsets saved to");
        canvas.setCursor(13, 78);
        canvas.print("NVS flash memory.");

        canvas.drawFastHLine(13, 102, w - 26, 0x1D4E35);
        canvas.setTextColor(0xA5D6A7, 0x0B2217);
        char l1[24], l2[24];
        std::snprintf(l1, sizeof(l1), "X:%+.2f Y:%+.2f", g_settings.imu_abs_gx, g_settings.imu_abs_gy);
        std::snprintf(l2, sizeof(l2), "Z:%+.2f S:%.2f", g_settings.imu_abs_gz, last_spread);
        canvas.setCursor(13, 110);
        canvas.print(l1);
        canvas.setCursor(13, 128);
        canvas.print(l2);
    } else if (vst == CalViewState::FailedMove) {
        canvas.drawRoundRect(6, 30, w - 12, 134, 7, 0xFF5252);
        canvas.fillRoundRect(7, 31, w - 14, 132, 6, 0x240E0E);

        canvas.setTextColor(0xFF8A80, 0x240E0E);
        canvas.setCursor(13, 38);
        canvas.print("UNIT MOVED!");

        canvas.setTextColor(0xFFCDD2, 0x240E0E);
        canvas.setCursor(13, 66);
        canvas.print("Place StickS3 flat");
        canvas.setCursor(13, 82);
        canvas.print("and keep still.");

        char l1[24];
        std::snprintf(l1, sizeof(l1), "Spread: %.2f dps", last_spread);
        canvas.setCursor(13, 116);
        canvas.print(l1);
    } else {
        canvas.drawRoundRect(6, 30, w - 12, 126, 7, 0x38BDF8);
        canvas.fillRoundRect(7, 31, w - 14, 124, 6, 0x0F1B29);

        canvas.setTextColor(0x80D8FF, 0x0F1B29);
        canvas.setCursor(13, 38);
        canvas.print("BASELINE ZERO");

        canvas.setTextColor(0xE2E8F0, 0x0F1B29);
        canvas.setCursor(13, 60);
        canvas.print("Place StickS3 flat");
        canvas.setCursor(13, 76);
        canvas.print("on a steady table.");

        canvas.drawFastHLine(13, 100, w - 26, 0x1E334A);
        canvas.setTextColor(0x90A4AE, 0x0F1B29);
        if (g_settings.imu_abs_valid) {
            char l1[24], l2[24];
            std::snprintf(l1, sizeof(l1), "X:%+.2f Y:%+.2f", g_settings.imu_abs_gx, g_settings.imu_abs_gy);
            std::snprintf(l2, sizeof(l2), "Z:%+.2f dps", g_settings.imu_abs_gz);
            canvas.setCursor(13, 108);
            canvas.print(l1);
            canvas.setCursor(13, 126);
            canvas.print(l2);
        } else {
            canvas.setCursor(13, 114);
            canvas.print("Not calibrated");
        }
    }

    canvas.fillRoundRect(6, 168, w - 12, 30, 5, 0x1565C0);
    canvas.setTextColor(0xFFFFFF, 0x1565C0);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("B : START ABS CAL", w / 2, 183);
    canvas.setTextDatum(TL_DATUM);

    ui_draw_footer2(canvas, "A: Clear   B: Start", "Hold A+B : Go Back");
    ui_present(canvas);
}

void draw_sampling_progress(M5Canvas &canvas, int done, int total) {
    int w = canvas.width();
    canvas.fillScreen(0x0B0F17);
    ui_draw_header(canvas, "IMU ABS CAL", 0x1D283A, 0x80D8FF);

    canvas.drawRoundRect(6, 36, w - 12, 148, 7, 0xFFB300);
    canvas.fillRoundRect(7, 37, w - 14, 146, 6, 0x1D160B);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextColor(0xFFD54F, 0x1D160B);
    canvas.setCursor(13, 46);
    canvas.print("SAMPLING IMU...");

    canvas.setTextColor(0xFFF8E1, 0x1D160B);
    canvas.setCursor(13, 72);
    canvas.print("Do not move unit.");
    canvas.setCursor(13, 90);
    canvas.print("Measuring 150 pts");

    int bar_w = w - 30;
    int fill_w = (done * bar_w) / total;
    canvas.fillRoundRect(15, 120, bar_w, 12, 3, 0x090D14);
    if (fill_w > 0) {
        canvas.fillRoundRect(15, 120, fill_w, 12, 3, 0xFFB300);
    }

    char pbuf[24];
    std::snprintf(pbuf, sizeof(pbuf), "%d / %d (%d%%)", done, total, (done * 100) / total);
    canvas.setTextColor(0xFFD54F, 0x1D160B);
    canvas.setCursor(15, 142);
    canvas.print(pbuf);

    ui_draw_footer2(canvas, "Sampling steady...", "Please wait 1.5s");
    ui_present(canvas);
}

} // namespace

bool imu_set_active() {
    if (!M5.Imu.isEnabled()) return false;
    if (!s_bmi_idle) return true;

    if (!M5.In_I2C.writeRegister8(s_bmi_address, kBmiPowerConf, 0x00, kBmiClock)) return false;
    // Leave advanced power save before enabling the sensors.
    vTaskDelay(pdMS_TO_TICKS(1) + 1);
    if (!M5.In_I2C.writeRegister8(s_bmi_address, kBmiPowerCtrl, 0x0E, kBmiClock)) return false;
    s_bmi_idle = false;
    vTaskDelay(pdMS_TO_TICKS(80));
    return true;
}

void imu_set_idle() {
    if (s_bmi_address == 0) find_internal_bmi270();
    if (s_bmi_address == 0 || s_bmi_idle) return;

    if (!M5.In_I2C.writeRegister8(s_bmi_address, kBmiPowerCtrl, 0x00, kBmiClock)) {
        ESP_LOGW("imu", "Could not stop BMI270 sensors");
        return;
    }
    s_bmi_idle = true;
    if (!M5.In_I2C.writeRegister8(s_bmi_address, kBmiPowerConf, 0x01, kBmiClock)) {
        ESP_LOGW("imu", "Could not enable BMI270 advanced power save");
    }
}

void imu_abs_cal_ui_run() {
    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    input_wait_all_release();

    CalViewState vst = CalViewState::Ready;
    float last_spread = 0.0f;

    while (true) {
        input_update();
        if (input_ab_held(500)) {
            sound_fx_click();
            input_wait_ab_release();
            canvas.deleteSprite();
            return;
        }

        if (input_a_pressed()) {
            g_settings.imu_abs_valid = false;
            g_settings.imu_abs_gx = 0.0f;
            g_settings.imu_abs_gy = 0.0f;
            g_settings.imu_abs_gz = 0.0f;
            g_settings.imu_abs_ax = 0.0f;
            g_settings.imu_abs_ay = 0.0f;
            g_settings.imu_abs_az = 1.0f;
            app_settings_save();
            vst = CalViewState::Ready;
            sound_fx_click();
        }

        if (input_b_pressed()) {
            sound_fx_click();
            input_wait_all_release();

            if (!imu_set_active()) {
                vst = CalViewState::FailedMove;
                last_spread = 99.9f;
            } else {
                // Warmup settle after releasing button B (10 samples * 12ms = 120ms)
                for (int w = 0; w < 10; w++) {
                    M5.Imu.update();
                    vTaskDelay(pdMS_TO_TICKS(12));
                }

                constexpr int kSamples = 150;
                double sum_gx = 0, sum_gy = 0, sum_gz = 0;
                double sum_ax = 0, sum_ay = 0, sum_az = 0;
                float min_gx = 1e6f, min_gy = 1e6f, min_gz = 1e6f;
                float max_gx = -1e6f, max_gy = -1e6f, max_gz = -1e6f;
                int valid = 0;

                for (int i = 0; i < kSamples; i++) {
                    M5.Imu.update();
                    auto d = M5.Imu.getImuData();
                    if (std::isfinite(d.gyro.x) && std::isfinite(d.gyro.y) && std::isfinite(d.gyro.z) &&
                        std::isfinite(d.accel.x) && std::isfinite(d.accel.y) && std::isfinite(d.accel.z)) {
                        sum_gx += d.gyro.x; sum_gy += d.gyro.y; sum_gz += d.gyro.z;
                        sum_ax += d.accel.x; sum_ay += d.accel.y; sum_az += d.accel.z;
                        min_gx = std::min(min_gx, d.gyro.x); max_gx = std::max(max_gx, d.gyro.x);
                        min_gy = std::min(min_gy, d.gyro.y); max_gy = std::max(max_gy, d.gyro.y);
                        min_gz = std::min(min_gz, d.gyro.z); max_gz = std::max(max_gz, d.gyro.z);
                        valid++;
                    }
                    if ((i % 10) == 0 || i == kSamples - 1) {
                        draw_sampling_progress(canvas, i + 1, kSamples);
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }

                imu_set_idle();
                float spread = std::max({max_gx - min_gx, max_gy - min_gy, max_gz - min_gz});
                last_spread = spread;
                if (valid >= 100 && spread < 4.5f) {
                    g_settings.imu_abs_valid = true;
                    g_settings.imu_abs_gx = (float)(sum_gx / valid);
                    g_settings.imu_abs_gy = (float)(sum_gy / valid);
                    g_settings.imu_abs_gz = (float)(sum_gz / valid);
                    g_settings.imu_abs_ax = (float)(sum_ax / valid);
                    g_settings.imu_abs_ay = (float)(sum_ay / valid);
                    g_settings.imu_abs_az = (float)(sum_az / valid);
                    app_settings_save();
                    vst = CalViewState::Done;
                    sound_fx_confirm();
                } else {
                    vst = CalViewState::FailedMove;
                    sound_fx_click();
                }
            }
        }

        if (ui_screen_is_on()) draw_abs_cal_idle(canvas, vst, last_spread);
        input_idle_delay(30);
    }
}
