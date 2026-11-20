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

int main() {
    test_ems_timer_stopwatch_and_pause();
    test_ems_timer_epi_interval_and_overdue();
    test_ems_timer_presets_and_ring_log();
    test_gyro_airmouse_calibration_and_motion();
    std::printf("ALL HOST TESTS PASSED\n");
    return 0;
}
