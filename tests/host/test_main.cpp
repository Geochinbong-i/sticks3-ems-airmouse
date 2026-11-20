#include "timer/ems_timer_core.h"
#include "airmouse/airmouse_math.h"

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

    assert(t.total_elapsed_ms(61500) == 60500);

    t.pause(61500);
    assert(t.state() == EmsState::Paused);
    assert(t.total_elapsed_ms(120000) == 60500);

    t.resume(120000);
    assert(t.state() == EmsState::Running);
    assert(t.total_elapsed_ms(130000) == 70500);

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
    assert(t.event_at(0)->at_sec == 15);
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

    // Repeating tick doesn't re-fire preset_just_due
    EmsTickResult r2 = t.tick(65000);
    assert(!r2.preset_just_due);

    // Log VITAL restarts preset countdown
    t.log_event(EmsEventType::Vital, 70000);
    assert(!t.preset_alarm_active());
    assert(t.preset_remaining_ms(70000) == 60000);

    // Ring overflow test (> 32 events)
    for (int i = 0; i < 40; i++) {
        t.log_event(EmsEventType::Shock, 75000 + i * 1000);
    }
    assert(t.event_count() == EmsTimerCore::kMaxEvents);
    assert(t.shock_count() == 40);
    assert(std::strcmp(t.event_at(0)->label, "SHOCK #40") == 0);
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

    // Noise within deadzone -> zero motion
    AirMouseDelta d0 = f.update(1.5f + 0.5f, -0.5f, 2.0f - 0.8f, 0.01f, cfg, false, false);
    assert(d0.dx == 0 && d0.dy == 0 && d0.wheel == 0);

    // Yaw right (gz < bias_z by 30 deg/s) => positive dx
    int total_dx = 0;
    for (int i = 0; i < 10; i++) {
        AirMouseDelta d = f.update(1.5f, -0.5f, 2.0f - 30.0f, 0.01f, cfg, false, false);
        total_dx += d.dx;
    }
    assert(total_dx > 20);

    // Pitch nose up (gx > bias_x by 25 deg/s) => negative dy (cursor up)
    int total_dy = 0;
    for (int i = 0; i < 10; i++) {
        AirMouseDelta d = f.update(1.5f + 25.0f, -0.5f, 2.0f, 0.01f, cfg, false, false);
        total_dy += d.dy;
    }
    assert(total_dy < -15);

    // Scroll mode freezes XY and emits wheel
    int total_wheel = 0;
    for (int i = 0; i < 20; i++) {
        AirMouseDelta d = f.update(1.5f + 35.0f, -0.5f, 2.0f - 30.0f, 0.01f, cfg, true, false);
        assert(d.dx == 0 && d.dy == 0);
        total_wheel += d.wheel;
    }
    assert(total_wheel != 0);

    // Paused mode freezes everything
    AirMouseDelta dp = f.update(50.0f, 50.0f, 50.0f, 0.01f, cfg, false, true);
    assert(dp.dx == 0 && dp.dy == 0 && dp.wheel == 0);
}

static void test_gyro_flip_180() {
    GyroAirMouseFilter f_norm;
    GyroAirMouseFilter f_flip;
    f_norm.reset_bias(0.0f, 0.0f, 0.0f);
    f_flip.reset_bias(0.0f, 0.0f, 0.0f);

    AirMouseConfig cfg_norm;
    AirMouseConfig cfg_flip;
    cfg_flip.flip_180 = true;

    // Yaw right (gz = -30) -> positive dx in BOTH orientations
    // Pitch nose up relative to StickS3 body (gx = +25) -> flips dy sign when 180 deg rotated
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

    // 1. A+B chord tap with staggered release (B released before A) must NOT emit LMB
    auto s = g.step(0, true, true);
    assert(!s.paused && s.buttons == 0 && !s.exit_requested);
    s = g.step(100, true, true);
    assert(!s.paused && s.buttons == 0);

    // B releases first while A stays down -> toggles pause, suppresses LMB
    s = g.step(140, true, false);
    assert(s.paused && s.clicked_cue && s.buttons == 0);
    // Still holding A on next frame -> still suppressed
    s = g.step(150, true, false);
    assert(s.paused && s.buttons == 0);
    // Unpause via another A+B chord and release B first while A stays down!
    s = g.step(160, false, false); // both released first
    assert(s.paused && s.buttons == 0);
    s = g.step(200, true, true);
    assert(s.paused && s.buttons == 0);
    s = g.step(280, true, false); // B released before A while unpausing!
    assert(!s.paused && s.clicked_cue && s.buttons == 0);
    s = g.step(300, true, false); // A still held from chord -> MUST NOT click LMB!
    assert(!s.paused && s.buttons == 0);
    s = g.step(320, false, false); // both released -> guard clears
    assert(s.buttons == 0);

    // Now fresh A press emits LMB normally
    s = g.step(340, true, false);
    assert(s.buttons == 0x01);
    s = g.step(360, false, false);
    assert(s.buttons == 0);

    // 2. B short tap -> RMB pulse, B hold >= 300ms -> scroll mode (no RMB)
    s = g.step(400, false, true);
    assert(s.buttons == 0 && !s.scroll_mode);
    s = g.step(450, false, false);
    assert(s.buttons == 0x02 && s.clicked_cue);

    s = g.step(600, false, true);
    s = g.step(920, false, true);
    assert(s.scroll_mode && s.buttons == 0);
    s = g.step(940, false, false);
    assert(!s.scroll_mode && s.buttons == 0);

    // 3. A+B hold >= 600ms -> exit_requested
    s = g.step(1000, true, true);
    assert(!s.exit_requested);
    s = g.step(1610, true, true);
    assert(s.exit_requested);
}

int main() {
    test_ems_timer_stopwatch_and_pause();
    test_ems_timer_epi_interval_and_overdue();
    test_ems_timer_presets_and_ring_log();
    test_gyro_airmouse_calibration_and_motion();
    test_gyro_flip_180();
    test_airmouse_button_guard();
    std::printf("ALL HOST TESTS PASSED\n");
    return 0;
}
