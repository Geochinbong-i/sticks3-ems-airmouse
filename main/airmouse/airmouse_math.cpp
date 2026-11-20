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

void AirMouseButtonGuard::reset() {
    paused_ = false;
    ab_chord_active_ = false;
    ab_chord_start_ms_ = 0;
    wait_all_release_ = false;
    b_was_down_ = false;
    b_down_ms_ = 0;
    b_scroll_active_ = false;
    rmb_pulse_until_ms_ = 0;
}

AirMouseBtnState AirMouseButtonGuard::step(int64_t now_ms, bool a_down, bool b_down) {
    AirMouseBtnState out{};
    out.paused = paused_;

    if (wait_all_release_) {
        if (!a_down && !b_down) {
            wait_all_release_ = false;
        }
        return out;
    }

    if (a_down && b_down) {
        if (!ab_chord_active_) {
            ab_chord_active_ = true;
            ab_chord_start_ms_ = now_ms;
            b_scroll_active_ = false;
            b_was_down_ = false;
        } else if ((now_ms - ab_chord_start_ms_) >= 600) {
            ab_chord_active_ = false;
            wait_all_release_ = true;
            out.exit_requested = true;
        }
        return out;
    }

    if (ab_chord_active_) {
        ab_chord_active_ = false;
        paused_ = !paused_;
        out.paused = paused_;
        out.clicked_cue = true;
        b_was_down_ = false;
        b_scroll_active_ = false;
        rmb_pulse_until_ms_ = 0;
        if (a_down || b_down) {
            // Prevent staggered release from firing a spurious LMB or RMB
            wait_all_release_ = true;
        }
        return out;
    }

    if (b_down && !b_was_down_) {
        b_down_ms_ = now_ms;
        b_scroll_active_ = false;
    } else if (b_down && b_was_down_) {
        if (!paused_ && (now_ms - b_down_ms_) >= 300) {
            b_scroll_active_ = true;
        }
    } else if (!b_down && b_was_down_) {
        if (paused_) {
            out.rel_cal_requested = true;
            out.clicked_cue = true;
        } else if (!b_scroll_active_) {
            rmb_pulse_until_ms_ = now_ms + 45;
            out.clicked_cue = true;
        }
        b_scroll_active_ = false;
    }
    b_was_down_ = b_down;

    if (!paused_) {
        if (a_down) out.buttons |= 0x01;
        if (now_ms < rmb_pulse_until_ms_) out.buttons |= 0x02;
        out.scroll_mode = b_scroll_active_;
    }

    return out;
}

GyroAirMouseFilter::GyroAirMouseFilter() = default;

void GyroAirMouseFilter::reset_motion() {
    accum_x_ = 0.0f;
    accum_y_ = 0.0f;
    accum_wheel_ = 0.0f;
    still_sec_ = 0.0f;
    prev_valid_ = false;
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

void GyroAirMouseFilter::set_relative_pose(float ax, float ay, float az,
                                           bool align_sign_to_abs,
                                           float abs_az) {
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az)) return;
    float norm = std::sqrt(ax * ax + ay * ay + az * az);
    if (norm < 0.25f) return;

    float nx = ax / norm;
    float ny = ay / norm;
    float nz = az / norm;

    // Ensure consistent polarity with flat resting orientation (+Z screen normal)
    if (align_sign_to_abs && abs_az < -0.5f) {
        nx = -nx;
        ny = -ny;
        nz = -nz;
    }

    rel_gx_ = nx;
    rel_gy_ = ny;
    rel_gz_ = nz;
    reset_motion();
}

void GyroAirMouseFilter::reset_relative_pose() {
    rel_gx_ = 0.0f;
    rel_gy_ = 0.0f;
    rel_gz_ = 1.0f;
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

    // Sample-to-sample stability detector cancels residual bias up to +/-10 dps
    // (fixes constant leftward cursor drift when residual offset exceeds 1.8 dps).
    float delta_step = 0.0f;
    if (prev_valid_) {
        delta_step = std::max({std::fabs(gx - prev_gx_),
                               std::fabs(gy - prev_gy_),
                               std::fabs(gz - prev_gz_)});
    }
    prev_gx_ = gx;
    prev_gy_ = gy;
    prev_gz_ = gz;
    prev_valid_ = true;

    float max_rate = std::max({std::fabs(ux), std::fabs(uy), std::fabs(uz)});
    if (delta_step < 0.85f && max_rate < 10.0f) {
        still_sec_ += dt_sec;
        if (still_sec_ >= 0.35f) {
            constexpr float alpha = 0.05f;
            bias_x_ += alpha * ux;
            bias_y_ += alpha * uy;
            bias_z_ += alpha * uz;
            ux = gx - bias_x_;
            uy = gy - bias_y_;
            uz = gz - bias_z_;
            // Hold output steady while locked stationary
            accum_x_ = 0.0f;
            accum_y_ = 0.0f;
            return out;
        }
    } else {
        still_sec_ = 0.0f;
    }

    if (paused) {
        accum_x_ = 0.0f;
        accum_y_ = 0.0f;
        accum_wheel_ = 0.0f;
        return out;
    }

    // Project corrected angular rate (ux, uy, uz) onto gravity-aligned handheld frame:
    //   yaw_axis   = rel_g (world vertical; defaults to +Z -> -uz for cursor X)
    //   right_axis = normalize(cross(forward_body, yaw_axis)) -> defaults to (+1,0,0) or (-1,0,0) when flipped 180
    float yaw_x = rel_gx_;
    float yaw_y = rel_gy_;
    float yaw_z = rel_gz_;

    float fwd_y = cfg.flip_180 ? -1.0f : 1.0f;
    // cross((0, fwd_y, 0), (yaw_x, yaw_y, yaw_z)) = (fwd_y * yaw_z, 0, -fwd_y * yaw_x)
    float rx_ax = fwd_y * yaw_z;
    float ry_ax = 0.0f;
    float rz_ax = -fwd_y * yaw_x;
    float r_norm = std::sqrt(rx_ax * rx_ax + rz_ax * rz_ax);
    if (r_norm > 0.1f) {
        rx_ax /= r_norm;
        rz_ax /= r_norm;
    } else {
        rx_ax = cfg.flip_180 ? -1.0f : 1.0f;
        rz_ax = 0.0f;
    }

    float rx = -(ux * yaw_x + uy * yaw_y + uz * yaw_z);
    float ry = -(ux * rx_ax + uy * ry_ax + uz * rz_ax);

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
