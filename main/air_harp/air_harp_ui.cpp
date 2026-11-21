#include "air_harp_ui.h"
#include "air_harp_core.h"

#include "app_settings.h"
#include "imu_cal.h"
#include "input_util.h"
#include "sound_fx.h"
#include "timer/ems_timer_ui.h"
#include "ui_common.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr uint32_t kBackground = 0x090D14;
constexpr uint32_t kPanel = 0x171D2C;
constexpr uint32_t kIdleColor = 0xA8B8D3;

struct NeutralSamples {
    float ax = 0, ay = 0, az = 0;
    float gx = 0, gy = 0, gz = 0;
    int accel_count = 0;
    int gyro_count = 0;

    bool add(AirHarpCore &harp, const m5::IMU_Class::imu_data_t &sample,
             bool accel_fresh, bool gyro_fresh, int64_t now_ms) {
        const auto &g = sample.gyro;
        if (gyro_fresh) {
            if (!std::isfinite(g.x) || !std::isfinite(g.y) || !std::isfinite(g.z) ||
                std::max({std::fabs(g.x), std::fabs(g.y), std::fabs(g.z)}) > 6.0f) {
                *this = {};
                return false;
            }
            gx += g.x;
            gy += g.y;
            gz += g.z;
            ++gyro_count;
        }
        if (accel_fresh) {
            const auto &a = sample.accel;
            ax += a.x;
            ay += a.y;
            az += a.z;
            ++accel_count;
        }
        if (accel_count < 16 || gyro_count < 16) return false;
        if (!harp.set_neutral(ax / accel_count, ay / accel_count, az / accel_count, now_ms)) {
            *this = {};
            return false;
        }
        gx /= gyro_count;
        gy /= gyro_count;
        gz /= gyro_count;
        return true;
    }
};

enum class HarpStatus { NoImu, Centering, Ready, Silent, Playing };

void draw_background(M5Canvas &canvas) {
    const int w = canvas.width();
    canvas.fillScreen(kBackground);
    canvas.fillRoundRect(5, 54, w - 10, 54, 5, kPanel);
    canvas.fillRoundRect(5, 115, w - 10, 54, 5, kPanel);
    canvas.setTextWrap(false);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(kIdleColor);
    canvas.drawCentreString("PITCH  Left / Right", w / 2, 58);
    canvas.drawCentreString("VOLUME  Up / Down", w / 2, 119);
    ui_draw_footer2(canvas, g_settings.harp_toggle ? "A: Toggle  B: Center" : "Hold A   B: Center",
                    "Hold A+B : Go Home");
}

void draw_status(M5Canvas &canvas, HarpStatus status) {
    const char *text = "Ready";
    switch (status) {
        case HarpStatus::NoImu: text = "IMU unavailable"; break;
        case HarpStatus::Centering: text = "Hold still to center"; break;
        case HarpStatus::Silent: text = "Silent"; break;
        case HarpStatus::Playing: text = "Playing"; break;
        case HarpStatus::Ready: break;
    }
    canvas.fillRect(0, 25, canvas.width(), 25, kBackground);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(kIdleColor);
    canvas.drawCentreString(text, canvas.width() / 2, 29);
}

void draw_note(M5Canvas &canvas, uint8_t note, uint32_t accent) {
    char label[4];
    air_harp_note_label(note, label);
    canvas.fillRect(10, 75, canvas.width() - 20, 28, kPanel);
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextColor(accent);
    canvas.drawCentreString(label, canvas.width() / 2, 76);
}

void draw_volume(M5Canvas &canvas, uint8_t volume_pct, uint32_t accent) {
    const int w = canvas.width();
    char label[8];
    std::snprintf(label, sizeof(label), "%u%%", static_cast<unsigned>(volume_pct));
    canvas.fillRect(10, 136, w - 20, 28, kPanel);
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextColor(accent);
    canvas.drawCentreString(label, w / 2, 137);

    constexpr int kInset = 14;
    const int width = w - 2 * kInset;
    canvas.fillRoundRect(kInset - 2, 178, width + 4, 14, 3, kPanel);
    const int filled = (width * volume_pct + kAirHarpMaxVolumePct / 2) / kAirHarpMaxVolumePct;
    if (filled > 0) canvas.fillRect(kInset, 181, filled, 8, accent);
}

void present_band(M5Canvas &canvas, int y, int height) {
    M5.Display.setClipRect(0, y, canvas.width(), height);
    ui_present(canvas);
    M5.Display.clearClipRect();
}

} // namespace

void air_harp_ui_run() {
    M5Canvas canvas(&M5.Display);
    if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) return;
    input_wait_all_release();

    AirHarpCore harp;
    NeutralSamples neutral;
    bool imu_active = false;
    bool calibrated = false;
    bool toggled = false;
    int64_t next_imu_attempt_ms = 0;
    int64_t next_draw_ms = 0;
    int64_t next_header_ms = 0;
    int64_t next_alarm_check_ms = 0;
    EmsAlarmState alarms{};
    EmsAlarmState drawn_alarms{};
    bool was_playing = false;
    AirHarpVoice last_voice{};
    bool redraw_base = true;
    HarpStatus drawn_status = HarpStatus::NoImu;
    AirHarpVoice drawn_voice{};

    while (true) {
        input_update();
        const int64_t now_ms = input_now_ms();
        if (input_ab_held(600)) break;

        if (!ui_screen_is_on()) {
            if (imu_active) {
                sound_fx_harp_stop();
                imu_set_idle();
            }
            imu_active = false;
            calibrated = false;
            toggled = false;
            was_playing = false;
            redraw_base = true;
            input_idle_delay(20);
            continue;
        }

        if (!imu_active && now_ms >= next_imu_attempt_ms) {
            imu_active = imu_set_active();
            next_imu_attempt_ms = now_ms + 1000;
            neutral = {};
        }
        if (input_b_pressed()) {
            calibrated = false;
            neutral = {};
        }
        if (g_settings.harp_toggle && input_a_pressed()) toggled = !toggled;
        if (now_ms >= next_alarm_check_ms) {
            alarms = ems_timer_active_alarms();
            next_alarm_check_ms = now_ms + 100;
        }

        const bool gate = !input_b_down() && (g_settings.harp_toggle ? toggled : input_a_down());
        if (imu_active) {
            const auto fresh = M5.Imu.update();
            const auto &s = M5.Imu.getImuData();
            const bool accel_fresh = fresh & m5::IMU_Class::sensor_mask_accel;
            const bool gyro_fresh = fresh & m5::IMU_Class::sensor_mask_gyro;
            if (!calibrated) {
                calibrated = neutral.add(harp, s, accel_fresh, gyro_fresh, now_ms);
            } else {
                if (accel_fresh) harp.sample_accel(s.accel.x, s.accel.y, s.accel.z);
                if (gyro_fresh) {
                    const bool changed = harp.sample_gyro(s.gyro.x - neutral.gx, s.gyro.y - neutral.gy,
                                                          s.gyro.z - neutral.gz, g_settings.rotation == 2, now_ms);
                    if (changed && gate) ui_note_activity(now_ms);
                }
            }
        }

        const auto voice = harp.voice();
        const bool playing = gate && calibrated && !alarms.any() && !g_settings.sound_muted;
        if (playing && (!was_playing || last_voice.note != voice.note || last_voice.volume_pct != voice.volume_pct)) {
            sound_fx_harp_play(voice.note, voice.volume_pct);
        } else if (!playing && was_playing) {
            sound_fx_harp_stop();
        }
        was_playing = playing;
        last_voice = voice;

        if (redraw_base || now_ms >= next_draw_ms) {
            const HarpStatus status = !imu_active ? HarpStatus::NoImu :
                                      !calibrated ? HarpStatus::Centering :
                                      !playing ? HarpStatus::Ready :
                                      voice.volume_pct == 0 ? HarpStatus::Silent : HarpStatus::Playing;
            const bool status_changed = redraw_base || status != drawn_status;
            const bool color_changed = (status == HarpStatus::Playing) != (drawn_status == HarpStatus::Playing);
            const bool note_changed = redraw_base || color_changed || voice.note != drawn_voice.note;
            const bool volume_changed = redraw_base || color_changed || voice.volume_pct != drawn_voice.volume_pct;
            const bool header_changed = redraw_base || now_ms >= next_header_ms ||
                                        alarms.epi != drawn_alarms.epi || alarms.preset != drawn_alarms.preset;
            const uint32_t accent = status == HarpStatus::Playing ? 0x80EDC4 : kIdleColor;
            if (redraw_base) draw_background(canvas);
            if (header_changed) {
                ui_draw_header(canvas, alarms.any() ? alarms.title() : "AIR HARP",
                               alarms.any() ? 0xB71C1C : 0x2B1D46, 0xFFFFFF,
                               g_settings.sound_muted && !alarms.any() ? "MUTE" : nullptr);
                next_header_ms = now_ms + 2000;
            }
            if (status_changed) draw_status(canvas, status);
            if (note_changed) draw_note(canvas, voice.note, accent);
            if (volume_changed) draw_volume(canvas, voice.volume_pct, accent);

            if (redraw_base) {
                ui_present(canvas);
                redraw_base = false;
            } else {
                if (header_changed) present_band(canvas, 0, 22);
                if (status_changed) present_band(canvas, 25, 25);
                if (note_changed) present_band(canvas, 75, 28);
                if (volume_changed) {
                    present_band(canvas, 136, 28);
                    present_band(canvas, 178, 14);
                }
            }
            drawn_status = status;
            drawn_voice = voice;
            drawn_alarms = alarms;
            next_draw_ms = now_ms + 33;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    sound_fx_harp_stop();
    imu_set_idle();
    input_wait_ab_release();
}
