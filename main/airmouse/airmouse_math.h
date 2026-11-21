#pragma once

#include <cstdint>

struct AirMouseConfig {
    int sensitivity = 3;     // 1..5 (default 3)
    float deadzone_dps = 2.0f;
    bool invert_x = false;
    bool invert_y = false;
    bool swap_xy = false;
    bool natural_scroll = false;
    bool flip_180 = false;   // true when display rotation == 2 (180 deg)
};

struct AirMouseAngularRates {
    float horizontal;
    float vertical;
};

// Project gyroscope readings onto the yaw and pitch axes of a held pose.
AirMouseAngularRates airmouse_project_motion(float gx, float gy, float gz,
                                             float up_x, float up_y, float up_z, bool flip_180);

struct AirMouseDelta {
    int8_t dx = 0;
    int8_t dy = 0;
    int8_t wheel = 0;
};

struct AirMouseBtnState {
    uint8_t buttons = 0;
    bool scroll_mode = false;
    bool paused = false;
    bool exit_requested = false;
    bool clicked_cue = false;
    bool rel_cal_requested = false;
};

class AirMouseButtonGuard {
public:
    void reset();
    AirMouseBtnState step(int64_t now_ms, bool a_down, bool b_down);

    void set_paused(bool paused) {
        paused_ = paused;
        ab_chord_active_ = false;
        b_was_down_ = false;
        b_scroll_active_ = false;
        rmb_pulse_until_ms_ = 0;
        wait_all_release_ = true;
    }

private:
    bool paused_ = false;
    bool ab_chord_active_ = false;
    int64_t ab_chord_start_ms_ = 0;
    bool wait_all_release_ = false;

    bool b_was_down_ = false;
    int64_t b_down_ms_ = 0;
    bool b_scroll_active_ = false;
    int64_t rmb_pulse_until_ms_ = 0;
};

class GyroAirMouseFilter {
public:
    GyroAirMouseFilter();

    void reset_motion();
    void reset_bias(float bx, float by, float bz);

    // Accumulate calibration samples while device rests still.
    void calib_begin();
    void calib_add_sample(float gx, float gy, float gz);
    void calib_finish();

    // Set held-pose neutral frame from gravity vector (accel) and optionally update gyro bias.
    void set_relative_pose(float ax, float ay, float az,
                           bool align_sign_to_abs = true,
                           float abs_az = 1.0f);
    void reset_relative_pose();

    // Process one 3-axis gyro sample (deg/s) with elapsed dt_sec.
    AirMouseDelta update(float gx, float gy, float gz, float dt_sec,
                         const AirMouseConfig &cfg, bool scroll_mode, bool paused);

    float bias_x() const { return bias_x_; }
    float bias_y() const { return bias_y_; }
    float bias_z() const { return bias_z_; }
    float rel_gx() const { return rel_gx_; }
    float rel_gy() const { return rel_gy_; }
    float rel_gz() const { return rel_gz_; }

private:
    float bias_x_ = 0.0f;
    float bias_y_ = 0.0f;
    float bias_z_ = 0.0f;

    // Unit vector representing "up/gravity-normal" in StickS3 body frame (default +Z).
    float rel_gx_ = 0.0f;
    float rel_gy_ = 0.0f;
    float rel_gz_ = 1.0f;

    float cal_sum_x_ = 0.0f;
    float cal_sum_y_ = 0.0f;
    float cal_sum_z_ = 0.0f;
    int cal_samples_ = 0;

    float prev_gx_ = 0.0f;
    float prev_gy_ = 0.0f;
    float prev_gz_ = 0.0f;
    bool prev_valid_ = false;

    float accum_x_ = 0.0f;
    float accum_y_ = 0.0f;
    float accum_wheel_ = 0.0f;
    float still_sec_ = 0.0f;
};
