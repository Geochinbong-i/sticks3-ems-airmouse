#pragma once

#include <cstdint>

struct AirMouseConfig {
    int sensitivity = 3;     // 1..5 (default 3)
    float deadzone_dps = 2.0f;
    bool invert_x = false;
    bool invert_y = false;
    bool swap_xy = false;
    bool natural_scroll = false;
};

struct AirMouseDelta {
    int8_t dx = 0;
    int8_t dy = 0;
    int8_t wheel = 0;
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

    // Process one 3-axis gyro sample (deg/s) with elapsed dt_sec.
    AirMouseDelta update(float gx, float gy, float gz, float dt_sec,
                         const AirMouseConfig &cfg, bool scroll_mode, bool paused);

    float bias_x() const { return bias_x_; }
    float bias_y() const { return bias_y_; }
    float bias_z() const { return bias_z_; }

private:
    float bias_x_ = 0.0f;
    float bias_y_ = 0.0f;
    float bias_z_ = 0.0f;

    float cal_sum_x_ = 0.0f;
    float cal_sum_y_ = 0.0f;
    float cal_sum_z_ = 0.0f;
    int cal_samples_ = 0;

    float accum_x_ = 0.0f;
    float accum_y_ = 0.0f;
    float accum_wheel_ = 0.0f;
    float still_sec_ = 0.0f;
};
