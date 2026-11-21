#include "air_harp_ui.h"
#include "air_harp_core.h"

#include "app_settings.h"
#include "imu_cal.h"
#include "input_util.h"
#include "sound_fx.h"
#include "timer/ems_timer_ui.h"
#include "ui_common.h"

#include <cmath>

namespace {

struct NeutralSamples {
    float x = 0;
    float y = 0;
    float z = 0;
    int count = 0;

    bool add(AirHarpCore &harp, const m5::IMU_Class::imu_data_t &sample) {
        const auto &g = sample.gyro;
        const auto &a = sample.accel;
        if (!std::isfinite(g.x) || !std::isfinite(g.y) || !std::isfinite(g.z) ||
            !std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z) ||
            std::fabs(g.x) > 35.0f || std::fabs(g.y) > 35.0f || std::fabs(g.z) > 35.0f) {
            *this = {};
            return false;
        }
        x += a.x;
        y += a.y;
        z += a.z;
        if (++count < 12) return false;

        const bool ready = harp.set_neutral(x / count, y / count, z / count);
        *this = {};
        return ready;
    }
};

void draw_harp(M5Canvas &canvas, AirHarpVoice voice, const char *state, bool playing,
               bool ems_alarm) {
    const int w = canvas.width();
    const uint32_t accent = playing ? 0x80EDC4 : 0xA8B8D3;

    canvas.fillScreen(0x090D14);
    ui_draw_header(canvas, ems_alarm ? "EPI DUE!" : "AIR HARP",
                   ems_alarm ? 0xB71C1C : 0x2B1D46, 0xFFFFFF,
                   g_settings.sound_muted && !ems_alarm ? "MUTE" : nullptr);

    canvas.setTextWrap(false);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(0xA8B8D3);
    canvas.drawCentreString(state, w / 2, 29);

    canvas.fillRoundRect(5, 54, w - 10, 54, 5, 0x171D2C);
    canvas.fillRoundRect(5, 115, w - 10, 54, 5, 0x171D2C);
    canvas.drawCentreString("PITCH  Left / Right", w / 2, 58);
    canvas.drawCentreString("DUTY  Up / Down", w / 2, 119);
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextColor(accent);
    canvas.drawCentreString(kAirHarpNotes[voice.note].name, w / 2, 76);
    canvas.drawCentreString(kAirHarpDuties[voice.duty], w / 2, 137);

    const int start = (w - 96) / 2;
    const int high = 3 * (voice.duty + 1);
    for (int i = 0; i < 4; ++i) {
        const int x = start + i * 24;
        canvas.drawFastVLine(x, 179, 12, accent);
        canvas.drawFastHLine(x, 179, high, accent);
        canvas.drawFastVLine(x + high, 179, 12, accent);
        canvas.drawFastHLine(x + high, 190, 24 - high, accent);
    }

    ui_draw_footer2(canvas, g_settings.harp_toggle ? "A: Toggle  B: Center" : "Hold A   B: Center",
                    "Hold A+B : Go Home");
    ui_present(canvas);
}

} // namespace

void air_harp_ui_run() {
    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    input_wait_all_release();

    AirHarpCore harp;
    NeutralSamples neutral;
    bool imu_active = false;
    bool calibrated = false;
    bool toggled = false;
    int64_t next_imu_attempt_ms = 0;
    int64_t next_draw_ms = 0;
    int64_t next_alarm_check_ms = 0;
    bool ems_alarm = false;
    bool was_playing = false;
    AirHarpVoice last_voice{};

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
            input_idle_delay(20);
            continue;
        }

        if (!imu_active && now_ms >= next_imu_attempt_ms) {
            imu_active = imu_set_active();
            next_imu_attempt_ms = now_ms + 1000;
            neutral = {};
            harp.reset_motion();
            next_draw_ms = 0;
        }
        if (input_b_pressed()) {
            calibrated = false;
            neutral = {};
            harp.reset_motion();
            next_draw_ms = 0;
        }
        if (g_settings.harp_toggle && input_a_pressed()) {
            toggled = !toggled;
            next_draw_ms = 0;
        }
        if (now_ms >= next_alarm_check_ms) {
            ems_alarm = ems_timer_has_active_alarm();
            next_alarm_check_ms = now_ms + 100;
        }

        const bool gate = !input_b_down() && (g_settings.harp_toggle ? toggled : input_a_down());
        if (imu_active) {
            const auto fresh = M5.Imu.update();
            const auto &s = M5.Imu.getImuData();
            // Acceleration and gyro data-ready bits can arrive in different polls.
            if (!calibrated && (fresh & m5::IMU_Class::sensor_mask_accel)) {
                calibrated = neutral.add(harp, s);
                if (calibrated) next_draw_ms = 0;
            } else if (calibrated && (fresh & m5::IMU_Class::sensor_mask_gyro)) {
                const float gx = s.gyro.x - g_settings.imu_abs_gx;
                const float gy = s.gyro.y - g_settings.imu_abs_gy;
                const float gz = s.gyro.z - g_settings.imu_abs_gz;
                if (harp.sample(now_ms, gx, gy, gz, g_settings.rotation == 2)) {
                    if (gate) ui_note_activity(now_ms);
                    next_draw_ms = 0;
                }
            }
        }

        const auto voice = harp.voice();
        const bool playing = gate && calibrated && !ems_alarm && !g_settings.sound_muted;
        if (playing && (!was_playing || last_voice.note != voice.note || last_voice.duty != voice.duty)) {
            sound_fx_harp_play(voice.note, voice.duty);
        } else if (!playing && was_playing) {
            sound_fx_harp_stop();
        }
        if (playing != was_playing) next_draw_ms = 0;
        was_playing = playing;
        last_voice = voice;

        if (now_ms >= next_draw_ms) {
            const char *status = !imu_active ? "IMU unavailable" : !calibrated ? "Hold still to center" :
                                 playing ? "Playing" : "Ready";
            draw_harp(canvas, voice, status, playing, ems_alarm);
            next_draw_ms = now_ms + 200;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    sound_fx_harp_stop();
    imu_set_idle();
    input_wait_ab_release();
    canvas.deleteSprite();
}
