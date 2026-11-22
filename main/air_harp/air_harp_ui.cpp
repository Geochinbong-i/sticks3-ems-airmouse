#include "air_harp_ui.h"
#include "air_harp_core.h"

#include "app_settings.h"
#include "imu_cal.h"
#include "input_util.h"
#include "sound_fx.h"
#include "timer/ems_timer_ui.h"
#include "ui_common.h"
#include "ui_menu.h"
#include "ui_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

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

enum class HarpStatus { Ready, Silent, Playing };

void draw_background(M5Canvas &canvas, bool arp, AirHarpNoteRange range) {
    const int w = canvas.width();
    canvas.fillScreen(ui_theme().background);
    canvas.fillRoundRect(5, 54, w - 10, 54, 5, ui_theme().surface);
    canvas.fillRoundRect(5, 115, w - 10, 54, 5, ui_theme().surface);
    canvas.setTextWrap(false);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(ui_theme().muted);
    if (arp) {
        canvas.drawString("KEY", 10, 57);
        canvas.drawRightString("Left / Right", w - 10, 57);
        canvas.drawString("SCALE", 10, 118);
        canvas.drawRightString("Up / Down", w - 10, 118);
    } else {
        canvas.drawString("PITCH", 10, 57);
        canvas.drawRightString(range.notes, w - 10, 57);
        canvas.drawCentreString("VOLUME", w / 2, 119);
    }
    ui_draw_themed_footer(canvas, "A Play   B Center", "Hold A+B Back");
}

void draw_calibration(M5Canvas &canvas, bool imu_active) {
    const auto &theme = ui_theme();
    canvas.fillScreen(theme.background);
    ui_draw_header(canvas, "SET CENTER", theme.surface, theme.text);
    canvas.fillRoundRect(4, 66, canvas.width() - 8, 80, 4, theme.surface);
    canvas.drawRoundRect(4, 66, canvas.width() - 8, 80, 4, theme.border);
    canvas.setTextColor(theme.text);
    canvas.drawCentreString(imu_active ? "Hold still" : "IMU unavailable", canvas.width() / 2, 78);
    canvas.setTextColor(theme.muted);
    if (imu_active) {
        canvas.drawCentreString("Set your neutral", canvas.width() / 2, 100);
        canvas.drawCentreString("playing position.", canvas.width() / 2, 121);
    }
    ui_draw_themed_footer(canvas, imu_active ? "Centering..." : "Retrying IMU", "Hold A+B Back");
    ui_present(canvas);
}

void draw_status(M5Canvas &canvas, HarpStatus status, const char *pattern_name) {
    const char *text = "Ready";
    switch (status) {
        case HarpStatus::Silent: text = "Silent"; break;
        case HarpStatus::Playing: text = "Playing"; break;
        case HarpStatus::Ready: break;
    }
    canvas.fillRect(0, 25, canvas.width(), 25, ui_theme().background);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(status == HarpStatus::Playing ? ui_theme().accent : ui_theme().muted);
    char caption[24];
    if (pattern_name) {
        std::snprintf(caption, sizeof(caption), "%s / %s", pattern_name, text);
        text = caption;
    }
    canvas.drawCentreString(text, canvas.width() / 2, 29);
}

void draw_key(M5Canvas &canvas, unsigned note, bool arp, uint32_t accent) {
    char label[4];
    air_harp_note_label(note, label);
    if (arp) label[std::strlen(label) - 1] = '\0';
    canvas.fillRect(10, 75, canvas.width() - 20, 28, ui_theme().surface);
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextColor(accent);
    canvas.drawCentreString(label, canvas.width() / 2, 76);
}

void draw_scale(M5Canvas &canvas, AirHarpScale scale) {
    constexpr const char *names[] = {"Natural Minor", "Major", "Pentatonic"};
    canvas.fillRect(10, 136, canvas.width() - 20, 28, ui_theme().surface);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(ui_theme().text);
    canvas.drawCentreString(names[static_cast<unsigned>(scale)], canvas.width() / 2, 138);
}

void draw_arp_note(M5Canvas &canvas, uint8_t note, bool playing, AirHarpNoteRange range) {
    canvas.fillRect(0, 178, canvas.width(), 22, ui_theme().background);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(ui_theme().muted);
    char octaves[14];
    std::snprintf(octaves, sizeof(octaves), "OCT %s", range.label);
    canvas.drawString(octaves, 8, 181);
    if (playing) {
        char label[4];
        air_harp_note_label(note, label);
        canvas.setTextColor(ui_theme().accent);
        canvas.drawRightString(label, canvas.width() - 8, 181);
    }
}

void draw_volume(M5Canvas &canvas, uint8_t volume_pct, uint32_t accent) {
    const int w = canvas.width();
    char label[8];
    std::snprintf(label, sizeof(label), "%u%%", static_cast<unsigned>(volume_pct));
    canvas.fillRect(10, 136, w - 20, 28, ui_theme().surface);
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextColor(accent);
    canvas.drawCentreString(label, w / 2, 137);

    constexpr int kInset = 14;
    const int width = w - 2 * kInset;
    canvas.fillRoundRect(kInset - 2, 178, width + 4, 14, 3, ui_theme().surface);
    const int filled = (width * volume_pct + kAirHarpMaxVolumePct / 2) / kAirHarpMaxVolumePct;
    if (filled > 0) canvas.fillRect(kInset, 181, filled, 8, accent);
}

void present_band(M5Canvas &canvas, int y, int height) {
    M5.Display.setClipRect(0, y, canvas.width(), height);
    ui_present(canvas);
    M5.Display.clearClipRect();
}

} // namespace

static void play_harp(const MenuRow *pattern, AirHarpPattern melody = AirHarpPattern::Classic) {
    const bool arp = pattern != nullptr;
    M5Canvas canvas(&M5.Display);
    if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) return;
    input_wait_all_release();

    const auto range_setting = static_cast<AirHarpRange>(g_settings.harp_octaves);
    const auto range = air_harp_range(range_setting);
    AirHarpCore harp(range_setting, g_settings.harp_turn_degrees);
    AirHarpArpeggio arp_player(melody);
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
    HarpStatus drawn_status = HarpStatus::Ready;
    AirHarpVoice drawn_voice{};
    unsigned drawn_key = 0;
    auto drawn_scale = AirHarpScale::Major;

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
            arp_player.stop();
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
            next_draw_ms = 0;
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

        auto voice = harp.voice();
        const bool playing = gate && calibrated && !alarms.any() && !g_settings.sound_muted;
        if (arp && playing) {
            voice = {arp_player.play(now_ms, harp.key(), harp.scale(), range), 75};
            ui_note_activity(now_ms);
        } else if (arp) {
            arp_player.stop();
        }
        if (playing && (!was_playing || last_voice.note != voice.note || last_voice.volume_pct != voice.volume_pct)) {
            sound_fx_harp_play(voice.note, voice.volume_pct);
        } else if (!playing && was_playing) {
            sound_fx_harp_stop();
        }
        was_playing = playing;
        last_voice = voice;

        if (!calibrated) {
            if (now_ms >= next_draw_ms) {
                draw_calibration(canvas, imu_active);
                next_draw_ms = now_ms + 150;
            }
            redraw_base = true;
            input_idle_delay(10);
            continue;
        }

        if (redraw_base || now_ms >= next_draw_ms) {
            const HarpStatus status = !playing ? HarpStatus::Ready :
                                      voice.volume_pct == 0 ? HarpStatus::Silent : HarpStatus::Playing;
            const bool status_changed = redraw_base || status != drawn_status;
            const bool color_changed = (status == HarpStatus::Playing) != (drawn_status == HarpStatus::Playing);
            const bool key_changed = redraw_base || color_changed ||
                                     (arp ? harp.key() != drawn_key : voice.note != drawn_voice.note);
            const bool bottom_changed = redraw_base || color_changed ||
                                        (arp ? harp.scale() != drawn_scale : voice.volume_pct != drawn_voice.volume_pct);
            const bool playing_note_changed = arp && (redraw_base || color_changed || voice.note != drawn_voice.note);
            const bool header_changed = redraw_base || now_ms >= next_header_ms ||
                                        alarms.epi != drawn_alarms.epi || alarms.preset != drawn_alarms.preset;
            const uint32_t accent = status == HarpStatus::Playing ? ui_theme().accent : ui_theme().muted;
            if (redraw_base) draw_background(canvas, arp, range);
            if (header_changed) {
                ui_draw_header(canvas, alarms.any() ? alarms.title() : arp ? "ARPEGGIO" : "SINGLE",
                               alarms.any() ? 0xB71C1Cu : ui_theme().surface,
                               alarms.any() ? 0xFFFFFFu : ui_theme().text,
                               g_settings.sound_muted && !alarms.any() ? "MUTE" : nullptr);
                next_header_ms = now_ms + 2000;
            }
            if (status_changed) draw_status(canvas, status, pattern ? pattern->title : nullptr);
            if (key_changed) draw_key(canvas, arp ? harp.key() : voice.note, arp, accent);
            if (bottom_changed) {
                if (arp) draw_scale(canvas, harp.scale());
                else draw_volume(canvas, voice.volume_pct, accent);
            }
            if (playing_note_changed) draw_arp_note(canvas, voice.note, playing, range);

            if (redraw_base) {
                ui_present(canvas);
                redraw_base = false;
            } else {
                if (header_changed) present_band(canvas, 0, 22);
                if (status_changed) present_band(canvas, 25, 25);
                if (key_changed) present_band(canvas, 75, 28);
                if (bottom_changed) present_band(canvas, 136, 28);
                if (playing_note_changed || (!arp && bottom_changed)) present_band(canvas, 178, 22);
            }
            drawn_key = harp.key();
            drawn_scale = harp.scale();
            drawn_status = status;
            drawn_voice = voice;
            drawn_alarms = alarms;
            next_draw_ms = now_ms + 33;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    sound_fx_harp_stop();
    imu_set_idle();
    sound_fx_back();
    input_wait_ab_release();
}


void air_harp_ui_run() {
    constexpr MenuRow modes[] = {
        {"SINGLE", "Pitch and volume", 0xC393FFu},
        {"ARPEGGIO", "Key and scale", 0xC393FFu},
    };
    constexpr MenuRow patterns[] = {
        {"CLASSIC", "Four-note loop", 0xC393FFu},
        {"RISE", "Quick scale steps", 0xC393FFu},
        {"WALTZ", "Long first beat", 0xC393FFu},
        {"WAVE", "Uneven rhythm", 0xC393FFu},
    };
    for (;;) {
        const int mode = ui_choose_menu("AIR HARP", modes, std::size(modes));
        if (mode < 0) return;
        if (mode == 0) {
            play_harp(nullptr);
        } else {
            for (;;) {
                const int melody = ui_choose_menu("ARP PATTERN", patterns, std::size(patterns));
                if (melody < 0) break;
                play_harp(&patterns[melody], static_cast<AirHarpPattern>(melody));
            }
        }
    }
}
