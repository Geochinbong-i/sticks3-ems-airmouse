#include "airmouse_math.h"

#include <algorithm>
#include <cmath>

namespace {
float apply_deadzone(float v, float dz) {
    float a = std::fabs(v);
    if (a <= dz) return 0.0f;
    return std::copysign(a - dz, v);
}

int8_t extract_clamped_i8(float &accum) {
    int v = (int)std::trunc(accum);
    if (v > 127) v = 127;
    if (v < -127) v = -127;
    accum -= (float)v;
    return (int8_t)v;
}
} // namespace

GyroAirMouseFilter::GyroAirMouseFilter() = default;

void GyroAirMouseFilter::reset_motion() {
    accum_x_ = 0.0f;
    accum_y_ = 0.0f;
    accum_wheel_ = 0.0f;
    still_sec_ = 0.0f;
}

void GyroAirMouseFilter::reset_bias(float bx, float by, float bz) {
    bias_x_ = bx;
    bias_y_ = by;
    bias_z_ = bz;
    reset_motion();
}

void GyroAirMouseFilter::calib_begin() {
    cal_sum_x_ = 0.0f;
    cal_sum_y_ = 0.0f;
    cal_sum_z_ = 0.0f;
    cal_samples_ = 0;
}

void GyroAirMouseFilter::calib_add_sample(float gx, float gy, float gz) {
    if (!std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(gz)) return;
    cal_sum_x_ += gx;
    cal_sum_y_ += gy;
    cal_sum_z_ += gz;
    cal_samples_++;
}

void GyroAirMouseFilter::calib_finish() {
    if (cal_samples_ > 0) {
        bias_x_ = cal_sum_x_ / (float)cal_samples_;
        bias_y_ = cal_sum_y_ / (float)cal_samples_;
        bias_z_ = cal_sum_z_ / (float)cal_samples_;
    }
    reset_motion();
}

AirMouseDelta GyroAirMouseFilter::update(float gx, float gy, float gz, float dt_sec,
                                          const AirMouseConfig &cfg,
                                          bool scroll_mode, bool paused) {
    AirMouseDelta out{};
    if (!std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(gz)) return out;
    if (dt_sec <= 0.0f) return out;
    if (dt_sec > 0.05f) dt_sec = 0.05f;

    float ux = gx - bias_x_;
    float uy = gy - bias_y_;
    float uz = gz - bias_z_;

    // Stationary auto-bias tracker prevents slow thermal drift when held still
    float max_rate = std::max({std::fabs(ux), std::fabs(uy), std::fabs(uz)});
    if (max_rate < 1.8f) {
        still_sec_ += dt_sec;
        if (still_sec_ >= 0.4f) {
            constexpr float alpha = 0.02f;
            bias_x_ += alpha * ux;
            bias_y_ += alpha * uy;
            bias_z_ += alpha * uz;
            ux = gx - bias_x_;
            uy = gy - bias_y_;
            uz = gz - bias_z_;
        }
    } else {
        still_sec_ = 0.0f;
    }

    if (paused) {
        reset_motion();
        return out;
    }

    // StickS3 remote posture (screen facing up, top pointing toward monitor):
    //   Yaw right  => gz < 0 => positive X (cursor right)
    //   Pitch up   => gx > 0 => negative Y (cursor up)
    float rx = -uz;
    float ry = -ux;

    if (cfg.swap_xy) {
        float tmp = rx;
        rx = ry;
        ry = tmp;
    }
    if (cfg.invert_x) rx = -rx;
    if (cfg.invert_y) ry = -ry;

    float dz = cfg.deadzone_dps;
    if (dz < 0.0f) dz = 0.0f;
    rx = apply_deadzone(rx, dz);
    ry = apply_deadzone(ry, dz);

    static constexpr float kSensScale[5] = {4.5f, 7.0f, 10.0f, 14.0f, 19.0f};
    int sens_idx = cfg.sensitivity - 1;
    if (sens_idx < 0) sens_idx = 0;
    if (sens_idx > 4) sens_idx = 4;
    float gain = kSensScale[sens_idx];

    if (scroll_mode) {
        // Freeze pointer XY while scrolling with vertical pitch
        accum_x_ = 0.0f;
        accum_y_ = 0.0f;
        float scroll_dir = cfg.natural_scroll ? 1.0f : -1.0f;
        accum_wheel_ += scroll_dir * ry * (0.045f * (0.6f + 0.25f * (float)sens_idx)) * dt_sec * 10.0f;
        out.wheel = extract_clamped_i8(accum_wheel_);
        return out;
    }

    accum_wheel_ = 0.0f;

    // Gentle nonlinear acceleration curve: precise at low speed, fast on flicks
    float vx = gain * (rx + 0.012f * rx * std::fabs(rx));
    float vy = gain * (ry + 0.012f * ry * std::fabs(ry));

    accum_x_ += vx * dt_sec;
    accum_y_ += vy * dt_sec;

    out.dx = extract_clamped_i8(accum_x_);
    out.dy = extract_clamped_i8(accum_y_);
    return out;
}
