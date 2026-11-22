#include "timer/ems_timer_core.h"
#include "timer/ems_timer_ui.h"
#include "timer/simple_timer_core.h"
#include "airmouse/airmouse_math.h"
#include "air_harp/air_harp_core.h"
#include "ir/ir_signal.h"
#include "menu_nav.h"
#include "time_sync_parse.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

static void test_ems_timer_stopwatch_and_pause() {
    EmsTimerCore t;
    assert(t.state() == EmsState::Idle);
    assert(t.total_elapsed_ms(1000) == 0);

    t.start(1000);
    assert(t.state() == EmsState::Running);
    assert(t.event_count() == 1);
    assert(std::strcmp(t.event_at(0)->label, "START") == 0);
    assert(t.case_start_utc_ms() == 0);

    assert(t.total_elapsed_ms(61500) == 60500);

    t.pause(61500);
    assert(t.state() == EmsState::Paused);
    assert(t.total_elapsed_ms(120000) == 60500);
    assert(t.event_at(0)->type == EmsEventType::Pause && t.event_at(0)->elapsed_ms == 60500);
    t.log_event(EmsEventType::Iv, 63000);
    assert(std::strcmp(t.event_at(0)->label, "IV") == 0 && t.event_at(0)->elapsed_ms == 62000);
    t.sync_clock(64050, 1760000063050LL);
    assert(t.case_start_utc_ms() == 1760000000000LL);
    assert(t.case_start_utc_ms() + t.event_at(0)->elapsed_ms == 1760000062000LL);

    t.resume(120000);
    assert(t.state() == EmsState::Running);
    assert(t.total_elapsed_ms(130000) == 70500);
    assert(t.event_at(0)->type == EmsEventType::Resume && t.event_at(0)->elapsed_ms == 119000);
    t.reset();
    assert(t.event_count() == 0 && t.case_start_utc_ms() == 0);
    t.start(150000, 1760000150000LL);
    assert(t.case_start_utc_ms() == 1760000150000LL);

    char buf[16];
    EmsTimerCore::format_hh_mm_ss(3661000, buf, sizeof(buf));
    assert(std::strcmp(buf, "01:01:01") == 0);
}

static void test_ems_timer_epi_interval_and_overdue() {
    EmsTimerCore t;
    t.set_epi_interval_sec(180); // 3 min
    assert(t.epi_interval_sec() == 180);
    assert(!t.epi_active());

    t.start(10000);
    assert(!t.epi_active());

    // Log EPI #1 at t=25s (15s after start)
    t.log_epi(25000);
    assert(t.epi_active());
    assert(t.epi_dose_count() == 1);
    assert(std::strcmp(t.event_at(0)->label, "EPI #1") == 0);
    assert(t.event_at(0)->elapsed_ms == 15000);
    assert(t.epi_remaining_ms(25000) == 180000);

    // 100s later -> 80s remaining
    EmsTickResult r1 = t.tick(125000);
    assert(!r1.epi_just_due);
    assert(t.epi_remaining_ms(125000) == 80000);
    assert(!t.epi_is_overdue(125000));

    // At exactly 180s -> due alert fires once
    EmsTickResult r2 = t.tick(205000);
    assert(r2.epi_just_due);
    assert(!r2.epi_reminder_beep);
    assert(t.epi_is_overdue(205000));
    assert(t.epi_alarm_active());

    // 15s overdue -> reminder beep fires
    EmsTickResult r3 = t.tick(220000);
    assert(!r3.epi_just_due);
    assert(r3.epi_reminder_beep);
    assert(t.epi_remaining_ms(220000) == -15000);

    // Log EPI #2 resets interval and clears overdue alarm
    t.log_epi(225000);
    assert(t.epi_dose_count() == 2);
    assert(!t.epi_alarm_active());
    assert(!t.epi_is_overdue(225000));
    assert(t.epi_remaining_ms(225000) == 180000);
    assert(std::strcmp(t.event_at(0)->label, "EPI #2") == 0);
}

static void test_ems_timer_presets_and_ring_log() {
    EmsTimerCore t;
    t.set_preset_index(1); // 60s
    assert(t.preset_sec() == 60);
    t.start(0);

    assert(t.preset_remaining_ms(30000) == 30000);
    assert(!t.preset_is_done(30000));

    EmsTickResult r = t.tick(60000);
    assert(r.preset_just_due);
    assert(t.preset_is_done(60000));
    assert(t.preset_alarm_active());

    EmsTickResult r2 = t.tick(65000);
    assert(!r2.preset_just_due);

    t.log_event(EmsEventType::Vital, 70000);
    assert(!t.preset_alarm_active());
    assert(t.preset_remaining_ms(70000) == 60000);

    constexpr int shocks = EmsTimerCore::kMaxEvents + 6;
    for (int i = 0; i < shocks; i++) {
        t.log_event(EmsEventType::Shock, 75000 + i * 1000);
    }
    assert(t.event_count() == EmsTimerCore::kMaxEvents);
    assert(t.shock_count() == shocks);
    assert(t.event_at(0)->seq == shocks);
    assert(t.event_at(EmsTimerCore::kMaxEvents - 1)->seq == 7);
    assert(t.event_at(EmsTimerCore::kMaxEvents) == nullptr);
}

static void test_simple_timer_core() {
    SimpleTimerCore st;
    assert(st.count() == 0);
    assert(!st.is_full());
    assert(st.at(0) == nullptr);

    for (int i = 0; i < 7; i++) {
        assert(st.stamp(14, i * 5, i * 7));
    }
    assert(st.count() == 7);
    assert(st.is_full());
    // 8th stamp rejected once 7 color boxes are full
    assert(!st.stamp(15, 0, 0));

    char buf[16];
    SimpleTimerCore::format_time(*st.at(6), buf, sizeof(buf));
    assert(std::strcmp(buf, "14:30:42") == 0);

    st.reset();
    assert(st.count() == 0);
    assert(!st.is_full());
    SimpleStamp empty{};
    SimpleTimerCore::format_time(empty, buf, sizeof(buf));
    assert(std::strcmp(buf, "--:--:--") == 0);
}

static void test_gyro_airmouse_calibration_and_motion() {
    GyroAirMouseFilter f;
    f.calib_begin();
    for (int i = 0; i < 50; i++) {
        f.calib_add_sample(1.5f, -0.5f, 2.0f);
    }
    f.calib_finish();
    assert(std::fabs(f.bias_x() - 1.5f) < 1e-4f);
    assert(std::fabs(f.bias_z() - 2.0f) < 1e-4f);

    AirMouseConfig cfg;
    cfg.sensitivity = 3;
    cfg.deadzone_dps = 2.0f;

    AirMouseDelta d0 = f.update(1.5f + 0.5f, -0.5f, 2.0f - 0.8f, 0.01f, cfg, false, false);
    assert(d0.dx == 0 && d0.dy == 0 && d0.wheel == 0);

    int total_dx = 0;
    for (int i = 0; i < 10; i++) {
        AirMouseDelta d = f.update(1.5f, -0.5f, 2.0f - 30.0f, 0.01f, cfg, false, false);
        total_dx += d.dx;
    }
    assert(total_dx > 20);

    int total_dy = 0;
    for (int i = 0; i < 10; i++) {
        AirMouseDelta d = f.update(1.5f + 25.0f, -0.5f, 2.0f, 0.01f, cfg, false, false);
        total_dy += d.dy;
    }
    assert(total_dy < -15);

    int total_wheel = 0;
    for (int i = 0; i < 20; i++) {
        AirMouseDelta d = f.update(1.5f + 35.0f, -0.5f, 2.0f - 30.0f, 0.01f, cfg, true, false);
        assert(d.dx == 0 && d.dy == 0);
        total_wheel += d.wheel;
    }
    assert(total_wheel != 0);

    AirMouseDelta dp = f.update(50.0f, 50.0f, 50.0f, 0.01f, cfg, false, true);
    assert(dp.dx == 0 && dp.dy == 0 && dp.wheel == 0);
}

static void test_gyro_leftward_drift_suppression_and_relative_pose() {
    // 1. Residual +4.0 dps constant gz offset (previously bypassed 1.8 dps check and pulled cursor left)
    GyroAirMouseFilter f;
    f.reset_bias(0.0f, 0.0f, 0.0f);
    AirMouseConfig cfg;
    for (int i = 0; i < 80; i++) {
        AirMouseDelta d = f.update(0.0f, 0.0f, 4.0f, 0.02f, cfg, false, false);
        if (i >= 20) {
            assert(d.dx == 0 && d.dy == 0);
        }
    }
    // Bias should have adapted substantially toward 4.0 dps
    assert(f.bias_z() > 2.5f);

    // 2. Relative pose calibration with stick rolled 45 degrees in hand (ax=0.707, az=0.707)
    GyroAirMouseFilter fr;
    fr.reset_bias(0.0f, 0.0f, 0.0f);
    fr.set_relative_pose(0.7071f, 0.0f, 0.7071f, true, 1.0f);

    // Turning purely right around held gravity vector (wx=-21.21, wz=-21.21) -> pure +dx, zero dy!
    int sum_dx = 0, sum_dy = 0;
    for (int i = 0; i < 12; i++) {
        AirMouseDelta d = fr.update(-21.213f, 0.0f, -21.213f, 0.01f, cfg, false, false);
        sum_dx += d.dx;
        sum_dy += d.dy;
    }
    assert(sum_dx > 15 && std::abs(sum_dy) <= 1);
}

static void test_gyro_flip_180() {
    GyroAirMouseFilter f_norm;
    GyroAirMouseFilter f_flip;
    f_norm.reset_bias(0.0f, 0.0f, 0.0f);
    f_flip.reset_bias(0.0f, 0.0f, 0.0f);

    AirMouseConfig cfg_norm;
    AirMouseConfig cfg_flip;
    cfg_flip.flip_180 = true;

    int dx_n = 0, dy_n = 0, dx_f = 0, dy_f = 0;
    for (int i = 0; i < 10; i++) {
        auto dn = f_norm.update(25.0f, 0.0f, -30.0f, 0.01f, cfg_norm, false, false);
        auto df = f_flip.update(25.0f, 0.0f, -30.0f, 0.01f, cfg_flip, false, false);
        dx_n += dn.dx; dy_n += dn.dy;
        dx_f += df.dx; dy_f += df.dy;
    }
    assert(dx_n > 20 && dx_f == dx_n);
    assert(dy_n < -15 && dy_f == -dy_n);
}

static void test_airmouse_button_guard() {
    AirMouseButtonGuard g;

    // 1. A+B chord tap with staggered release must NOT emit LMB
    auto s = g.step(0, true, true);
    assert(!s.paused && s.buttons == 0 && !s.exit_requested);
    s = g.step(100, true, true);
    s = g.step(140, true, false);
    assert(s.paused && s.clicked_cue && s.buttons == 0);
    s = g.step(150, true, false);
    assert(s.paused && s.buttons == 0);

    // Tap B while paused -> emits rel_cal_requested without RMB
    s = g.step(160, false, false);
    s = g.step(180, false, true);
    assert(!s.rel_cal_requested && s.buttons == 0);
    s = g.step(210, false, false);
    assert(s.rel_cal_requested && s.buttons == 0);

    // Unpause via A+B chord with staggered release
    s = g.step(240, true, true);
    s = g.step(280, true, false);
    assert(!s.paused && s.clicked_cue && s.buttons == 0);
    s = g.step(300, true, false);
    assert(!s.paused && s.buttons == 0);
    s = g.step(320, false, false);

    // Fresh A press emits LMB normally
    s = g.step(340, true, false);
    assert(s.buttons == 0x01);
    s = g.step(360, false, false);
    assert(s.buttons == 0);

    // B short tap -> RMB pulse, B hold >= 300ms -> scroll mode
    s = g.step(400, false, true);
    s = g.step(450, false, false);
    assert(s.buttons == 0x02 && s.clicked_cue);

    s = g.step(600, false, true);
    s = g.step(920, false, true);
    assert(s.scroll_mode && s.buttons == 0);
    s = g.step(940, false, false);
    assert(!s.scroll_mode && s.buttons == 0);

    // A+B hold >= 600ms -> exit_requested
    s = g.step(1000, true, true);
    assert(!s.exit_requested);
    s = g.step(1610, true, true);
    assert(s.exit_requested);
}

static void test_air_harp_continuous_controls() {
    AirHarpCore harp;
    assert(!harp.set_neutral(0, 0, 0.2f, 0));
    assert(harp.set_neutral(0, 0, 1, 0));
    assert(harp.voice().note == AirHarpVoice::center_note);
    assert(harp.voice().volume_pct == AirHarpVoice::center_volume_pct);

    // A turn too slow to register as a snap still passes every semitone.
    int64_t now = 0;
    bool played[kAirHarpNoteCount]{};
    played[harp.voice().note] = true;
    auto sweep_yaw = [&](float degrees_per_sec, int duration_ms) {
        for (int i = 0; i < duration_ms; i += 10) {
            now += 10;
            harp.sample_gyro(0, 0, degrees_per_sec, false, now);
            played[harp.voice().note] = true;
            assert(harp.voice().volume_pct == AirHarpVoice::center_volume_pct);
        }
    };
    sweep_yaw(9, 5100);
    assert(harp.voice().note == 0);
    sweep_yaw(-9, 10200);
    assert(harp.voice().note == kAirHarpNoteCount - 1);
    for (bool visited : played) assert(visited);

    char note[4];
    air_harp_note_label(0, note);
    assert(std::strcmp(note, "C2") == 0);
    air_harp_note_label(13, note);
    assert(std::strcmp(note, "C#3") == 0);
    air_harp_note_label(kAirHarpNoteCount - 1, note);
    assert(std::strcmp(note, "B3") == 0);
    assert(std::fabs(air_harp_note_hz(kAirHarpNoteCount - 1) - 246.9417f) < 0.001f);
    assert(std::fabs(air_harp_note_hz(0) - 65.4064f) < 0.001f);
    assert(std::fabs(air_harp_note_hz(13) / air_harp_note_hz(12) - 1.059463f) < 0.00001f);

    // A stationary rate inside the drift deadband must not move the pitch.
    assert(harp.set_neutral(0, 0, 1, now));
    sweep_yaw(0.25f, 20000);
    assert(harp.voice().note == AirHarpVoice::center_note);

    // Sweep both ways from an upright pose through every volume setting.
    assert(harp.set_neutral(0, 1, 0, now));
    bool volumes[kAirHarpMaxVolumePct + 1]{};
    auto tilt = [&](float degrees, bool flip) {
        const float angle = degrees * 0.0174532925f;
        harp.sample_accel(0, std::cos(angle), -std::sin(angle));
        now += 10;
        harp.sample_gyro(0, 0, 0, flip, now);
        volumes[harp.voice().volume_pct] = true;
        assert(harp.voice().note == AirHarpVoice::center_note);
    };
    for (int i = 0; i >= -400; --i) tilt(i * 0.1f, false);
    assert(harp.voice().volume_pct == 0);
    for (int i = -400; i <= 400; ++i) tilt(i * 0.1f, false);
    assert(harp.voice().volume_pct == kAirHarpMaxVolumePct);
    for (bool visited : volumes) assert(visited);
    for (int i = 0; i < 35; ++i) tilt(40, true);
    assert(harp.voice().volume_pct == 0);
    assert(air_harp_channel_volume(0) == 0);
    assert(air_harp_channel_volume(AirHarpVoice::center_volume_pct) == 128);
    assert(air_harp_channel_volume(kAirHarpMaxVolumePct) == 255);
    for (int level = 1; level <= kAirHarpMaxVolumePct; ++level) {
        assert(air_harp_channel_volume(level) > air_harp_channel_volume(level - 1));
    }
}

static void test_harp_range_and_arpeggio() {
    const auto range = air_harp_range(AirHarpRange::Oct4);
    assert(range.first == 24 && range.count == 12);
    assert(std::strcmp(range.notes, "C4-B4") == 0);

    AirHarpCore standard(AirHarpRange::Oct4, 45);
    AirHarpCore wide(AirHarpRange::Oct4, 75);
    assert(standard.set_neutral(0, 0, 1, 0));
    assert(wide.set_neutral(0, 0, 1, 0));
    assert(standard.voice().note == 30 && standard.key() == 0);
    assert(standard.scale() == AirHarpScale::Major);
    for (int t = 10; t <= 1000; t += 10) {
        standard.sample_gyro(0, 0, -40, false, t);
        wide.sample_gyro(0, 0, -40, false, t);
    }
    assert(standard.voice().note > wide.voice().note);
    assert(wide.voice().note >= range.first && standard.voice().note < range.first + range.count);

    AirHarpArpeggio arp;
    assert(arp.play(1200, 0, AirHarpScale::Major, range) == 24);
    assert(arp.play(1449, 1, AirHarpScale::Minor, range) == 24);
    assert(arp.play(1450, 1, AirHarpScale::Minor, range) == 28);
    assert(arp.play(1700, 10, AirHarpScale::Major, range) == 29);
    arp.stop();
    assert(arp.play(4000, 11, AirHarpScale::Pentatonic, range) == 35);

    AirHarpArpeggio rise(AirHarpPattern::Rise);
    assert(rise.play(0, 0, AirHarpScale::Major, range) == 24);
    assert(rise.play(180, 0, AirHarpScale::Major, range) == 26);
    assert(rise.play(360, 0, AirHarpScale::Minor, range) == 27);
    assert(rise.play(1440, 0, AirHarpScale::Major, range) == 24);

    AirHarpArpeggio waltz(AirHarpPattern::Waltz);
    assert(waltz.play(0, 0, AirHarpScale::Major, range) == 24);
    assert(waltz.play(359, 1, AirHarpScale::Major, range) == 24);
    assert(waltz.play(360, 1, AirHarpScale::Major, range) == 32);
    assert(waltz.play(720, 1, AirHarpScale::Minor, range) == 27);

    AirHarpArpeggio wave(AirHarpPattern::Wave);
    assert(wave.play(0, 0, AirHarpScale::Pentatonic, range) == 24);
    assert(wave.play(400, 0, AirHarpScale::Pentatonic, range) == 33);
    assert(wave.play(799, 0, AirHarpScale::Minor, range) == 33);
    assert(wave.play(800, 0, AirHarpScale::Minor, range) == 29);
}

static void test_ems_simultaneous_alarms() {
    EmsTimerCore timer;
    timer.set_epi_interval_sec(180);
    timer.set_preset_index(3);
    timer.start(1000);
    timer.log_epi(1000);
    const auto due = timer.tick(181000);
    assert(due.epi_just_due && due.preset_just_due);
    EmsAlarmState alarms{timer.epi_alarm_active(), timer.preset_alarm_active()};
    assert(std::strcmp(alarms.title(), "EPI/VITAL DUE") == 0);
    timer.log_epi(181001);
    alarms = {timer.epi_alarm_active(), timer.preset_alarm_active()};
    assert(std::strcmp(alarms.title(), "VITAL DUE!") == 0);
}

static void test_time_sync_parsing() {
    // 1. 10-byte CTS payload: 2026-04-06 14:41:12 (2026 = 0x07EA)
    const uint8_t cts[10] = {0xEA, 0x07, 4, 6, 14, 41, 12, 1, 0, 0};
    auto p1 = parse_ble_time_payload(cts, sizeof(cts));
    assert(p1.kind == TimePayloadKind::LocalFields);
    assert(p1.year == 2026 && p1.month == 4 && p1.day == 6);
    assert(p1.hour == 14 && p1.min == 41 && p1.sec == 12);

    // 2. ASCII timestamp
    const char *iso = "2026-11-20 09:05:33";
    auto p2 = parse_ble_time_payload((const uint8_t *)iso, std::strlen(iso));
    assert(p2.kind == TimePayloadKind::LocalFields);
    assert(p2.year == 2026 && p2.month == 11 && p2.day == 20 && p2.hour == 9 && p2.min == 5 && p2.sec == 33);

    // 3. 4-byte LE epoch (1775486472 = 0x69D3F3C8 -> C8 F3 D3 69)
    uint32_t ep = 1775486472u;
    uint8_t ep_le[4] = {
        (uint8_t)(ep & 0xFF),
        (uint8_t)((ep >> 8) & 0xFF),
        (uint8_t)((ep >> 16) & 0xFF),
        (uint8_t)((ep >> 24) & 0xFF),
    };
    auto p3 = parse_ble_time_payload(ep_le, 4);
    assert(p3.kind == TimePayloadKind::UnixEpochUtc && p3.epoch_sec == (int64_t)ep);

    // 4. ASCII epoch string
    const char *ep_s = "1775486472";
    auto p4 = parse_ble_time_payload((const uint8_t *)ep_s, std::strlen(ep_s));
    assert(p4.kind == TimePayloadKind::UnixEpochUtc && p4.epoch_sec == (int64_t)ep);

    // 5. Invalid payload
    const char *bad = "hello";
    auto p5 = parse_ble_time_payload((const uint8_t *)bad, std::strlen(bad));
    assert(p5.kind == TimePayloadKind::Invalid);

    // 6. POSIX TZ inversion
    char tz[16];
    format_posix_tz(9, tz, sizeof(tz));
    assert(std::strcmp(tz, "UTC-9") == 0);
    format_posix_tz(-5, tz, sizeof(tz));
    assert(std::strcmp(tz, "UTC+5") == 0);

    // 7. URL decode
    char qs[32] = "My+Station%20119%21";
    assert(url_decode_inplace(qs));
    assert(std::strcmp(qs, "My Station 119!") == 0);
    char ssid[] = "%ED%95%9C%EA%B8%80%2B%25";
    assert(url_decode_inplace(ssid));
    assert(std::strcmp(ssid, "한글+%") == 0);
    char encoded_nul[] = "good%00trailing";
    assert(!url_decode_inplace(encoded_nul));
    char invalid_escape[] = "bad%2Z";
    assert(!url_decode_inplace(invalid_escape));
    char incomplete_escape[] = "bad%";
    assert(!url_decode_inplace(incomplete_escape));
}

static void test_menu_scroll_and_chords() {
    MenuCursor cursor;
    for (int i = 0; i < 4; ++i) cursor.move(1, 6);
    assert(cursor.selected == 4 && cursor.top == 1);
    cursor.move(1, 6);
    assert(cursor.selected == 5 && cursor.top == 2);
    cursor.move(1, 6);
    assert(cursor.selected == 0 && cursor.top == 0);
    cursor.move(-1, 6);
    assert(cursor.selected == 5 && cursor.top == 2);

    MenuButtons buttons;
    assert(buttons.update(false, true, 10, true) == MenuAction::None);
    assert(buttons.update(true, true, 35, true) == MenuAction::None);
    assert(buttons.update(true, false, 80, true) == MenuAction::None);
    assert(buttons.update(false, false, 100, true) == MenuAction::Previous);
    assert(buttons.update(false, true, 110, true) == MenuAction::None);
    assert(buttons.update(false, false, 130, true) == MenuAction::Select);
    assert(buttons.update(true, false, 200, true) == MenuAction::None);
    assert(buttons.update(true, false, 850, true) == MenuAction::Mute);
    assert(buttons.update(false, false, 900, true) == MenuAction::None);
    assert(buttons.update(true, true, 1000, false) == MenuAction::None);
    assert(buttons.update(true, true, 1600, false) == MenuAction::Back);
    assert(buttons.update(false, true, 1650, false) == MenuAction::None);
    assert(buttons.update(false, false, 1700, false) == MenuAction::None);
}

static void test_ir_signal_learning() {
    IrSignal frame;
    assert(ir_add_pulse(frame, false, 400));
    assert(frame.count == 0);
    assert(ir_add_pulse(frame, true, 4500));
    assert(ir_add_pulse(frame, true, 4500));
    assert(ir_add_pulse(frame, false, 4500));
    constexpr uint32_t bits = 0xBA45FF00u;
    for (unsigned i = 0; i < 32; ++i) {
        assert(ir_add_pulse(frame, true, 560));
        assert(ir_add_pulse(frame, false, ((bits >> i) & 1) ? 1690 : 560));
    }
    assert(ir_add_pulse(frame, true, 560));
    assert(ir_add_pulse(frame, false, kIrSignalGapUs));
    assert(ir_finish_signal(frame));
    assert(frame.count == 67 && frame.duration_us[0] == 9000 && ir_is_nec(frame));
    assert(ir_supports_high_power(frame));
    IrProfile profile;
    IrSignal tx;
    profile.nec = true;
    frame.duration_us[2] = 590;
    assert(ir_build_tx(frame, profile, tx) && tx.duration_us[2] == 560);
    profile.playback = IrPlayback::Repeat;
    profile.repeat_gap_ms = 120;
    assert(ir_build_tx(frame, profile, tx));
    assert(ir_frame_count(tx) == 2 && tx.duration_us[67] == 120000);
    assert(!ir_supports_high_power(tx));

    const IrSignal captured = tx;
    profile.nec = false;
    profile.playback = IrPlayback::Once;
    assert(ir_build_tx(captured, profile, tx) && tx.count == 67);
    profile.playback = IrPlayback::Captured;
    assert(ir_build_tx(captured, profile, tx) && tx.count == captured.count);
    assert(tx.duration_us[67] == captured.duration_us[67]);

    IrSignal long_burst = frame;
    long_burst.duration_us[0] = 11000;
    assert(ir_valid_signal(long_burst) && !ir_supports_high_power(long_burst));
    const auto first_space = frame.duration_us[3 + 24 * 2];
    frame.duration_us[3 + 24 * 2] = first_space == 560 ? 1690 : 560;
    assert(ir_valid_signal(frame) && !ir_is_nec(frame));

    IrSignal repeat;
    assert(ir_add_pulse(repeat, true, 9000));
    assert(ir_add_pulse(repeat, false, 2250));
    assert(ir_add_pulse(repeat, true, 560));
    assert(!ir_finish_signal(repeat));
    frame.duration_us[2] = 10;
    assert(!ir_valid_signal(frame));

    IrSignal full;
    for (size_t i = 0; i < kIrMaxPulses; ++i) {
        assert(ir_add_pulse(full, (i % 2) == 0, 400));
    }
    assert(ir_valid_signal(full));
    assert(!ir_add_pulse(full, false, 400));
}

int main() {
    test_ems_timer_stopwatch_and_pause();
    test_ems_timer_epi_interval_and_overdue();
    test_ems_timer_presets_and_ring_log();
    test_ems_simultaneous_alarms();
    test_simple_timer_core();
    test_gyro_airmouse_calibration_and_motion();
    test_gyro_leftward_drift_suppression_and_relative_pose();
    test_gyro_flip_180();
    test_airmouse_button_guard();
    test_air_harp_continuous_controls();
    test_harp_range_and_arpeggio();
    test_menu_scroll_and_chords();
    test_ir_signal_learning();
    test_time_sync_parsing();
    std::printf("ALL HOST TESTS PASSED\n");
    return 0;
}
