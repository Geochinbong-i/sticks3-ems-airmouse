#include "air_harp_core.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kDegrees = 57.2957795f;
constexpr float kPitchLimit = 35.0f;

bool normalize_gravity(float &x, float &y, float &z) {
    const float norm = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(norm) || norm < 0.82f || norm > 1.18f) return false;
    x /= norm;
    y /= norm;
    z /= norm;
    return true;
}

int quantize(int current, float position, int count) {
    // Cross a little past the midpoint to prevent jitter at step boundaries.
    if (std::fabs(position - current) <= 0.65f) return current;
    return std::clamp(static_cast<int>(std::lround(position)), 0, count - 1);
}

} // namespace

AirHarpNoteRange air_harp_range(AirHarpRange range) {
    static constexpr AirHarpNoteRange options[] = {
        {0, 12, "2", "C2-B2"}, {12, 12, "3", "C3-B3"}, {24, 12, "4", "C4-B4"},
        {0, 24, "2-3", "C2-B3"}, {0, 36, "2-4", "C2-B4"},
    };
    const auto index = range < AirHarpRange::Count ? range : AirHarpRange::Oct2To3;
    return options[static_cast<unsigned>(index)];
}

AirHarpCore::AirHarpCore(AirHarpRange range, int turn_limit)
    : range_(air_harp_range(range)), turn_limit_(std::clamp(turn_limit, 45, 75)) {
    voice_.note = range_.first + range_.count / 2;
}

float air_harp_note_hz(int note) {
    return 440.0f * std::exp2((note + kAirHarpFirstMidi - 69) / 12.0f);
}

void air_harp_note_label(int note, char (&label)[4]) {
    constexpr char names[] = "CCDDEFFGGAAB";
    constexpr unsigned sharps = (1 << 1) | (1 << 3) | (1 << 6) | (1 << 8) | (1 << 10);
    const int key = note % 12;
    const char octave = '2' + note / 12;
    label[0] = names[key];
    const bool sharp = (sharps & (1u << key)) != 0;
    label[1] = sharp ? '#' : octave;
    label[2] = sharp ? octave : '\0';
    label[3] = '\0';
}

uint8_t air_harp_channel_volume(int volume_pct) {
    const int level = std::clamp(volume_pct, 0, kAirHarpMaxVolumePct);
    return (level * 255 + kAirHarpMaxVolumePct / 2) / kAirHarpMaxVolumePct;
}

bool AirHarpCore::set_neutral(float ax, float ay, float az, int64_t now_ms) {
    if (!normalize_gravity(ax, ay, az)) return false;
    up_x_ = ax;
    up_y_ = ay;
    up_z_ = az;
    center_x_ = ax;
    center_y_ = ay;
    center_z_ = az;
    // Tilt around the screen's right axis, perpendicular to the held gravity.
    // Near vertical, derive that axis from screen X so both directions still work.
    const float horizontal = std::hypot(ax, az);
    const bool upright = horizontal < 0.20f;
    const float axis_x = upright ? 1.0f : az / horizontal;
    const float axis_z = upright ? 0.0f : -ax / horizontal;
    pitch_tangent_x_ = ay * axis_z;
    pitch_tangent_y_ = az * axis_x - ax * axis_z;
    pitch_tangent_z_ = -ay * axis_x;
    const float tangent_length = std::sqrt(pitch_tangent_x_ * pitch_tangent_x_ +
                                           pitch_tangent_y_ * pitch_tangent_y_ +
                                           pitch_tangent_z_ * pitch_tangent_z_);
    pitch_tangent_x_ /= tangent_length;
    pitch_tangent_y_ /= tangent_length;
    pitch_tangent_z_ /= tangent_length;
    yaw_ = filtered_yaw_ = 0;
    last_gyro_ms_ = now_ms;
    voice_ = {static_cast<uint8_t>(range_.first + range_.count / 2), AirHarpVoice::center_volume_pct};
    key_index_ = 6;
    scale_index_ = 1;
    return true;
}

void AirHarpCore::sample_accel(float ax, float ay, float az) {
    if (!normalize_gravity(ax, ay, az)) return;
    constexpr float smoothing = 0.18f;
    up_x_ += smoothing * (ax - up_x_);
    up_y_ += smoothing * (ay - up_y_);
    up_z_ += smoothing * (az - up_z_);
}

bool AirHarpCore::sample_gyro(float gx, float gy, float gz, bool flip_180, int64_t now_ms) {
    const int64_t elapsed_ms = now_ms - last_gyro_ms_;
    if (elapsed_ms <= 0) return false;
    last_gyro_ms_ = now_ms;
    if (elapsed_ms > 100 || !std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(gz)) return false;

    // Gravity gives us up/down tilt. Heading about gravity has no accelerometer
    // reference, so integrate the calibrated gyro until B sets a new center.
    const float dt = elapsed_ms * 0.001f;
    const float yaw_rate = -(gx * up_x_ + gy * up_y_ + gz * up_z_);
    constexpr float kYawDeadband = 0.35f;
    if (std::fabs(yaw_rate) > kYawDeadband) {
        yaw_ = std::clamp(yaw_ + yaw_rate * dt, -90.0f, 90.0f);
    }
    filtered_yaw_ += dt / (0.045f + dt) * (yaw_ - filtered_yaw_);

    const float tilt = up_x_ * pitch_tangent_x_ + up_y_ * pitch_tangent_y_ + up_z_ * pitch_tangent_z_;
    const float reference = up_x_ * center_x_ + up_y_ * center_y_ + up_z_ * center_z_;
    const float vertical = std::atan2(tilt, reference) * kDegrees * (flip_180 ? -1.0f : 1.0f);
    // Give each side of the neutral pose the full range of the hand motion.
    const int center_note = range_.count / 2;
    constexpr int center_volume = AirHarpVoice::center_volume_pct;
    const float note_span = filtered_yaw_ < 0 ? center_note : range_.count - 1 - center_note;
    const float turn_fraction = filtered_yaw_ / turn_limit_;
    const int note = range_.first + quantize(voice_.note - range_.first,
        center_note + turn_fraction * note_span, range_.count);
    const int volume = quantize(voice_.volume_pct, center_volume + vertical / kPitchLimit * center_volume,
                                kAirHarpMaxVolumePct + 1);
    const int key = quantize(key_index_, 6 + turn_fraction * (filtered_yaw_ < 0 ? 6 : 5), 12);
    const int scale = quantize(scale_index_, 1 + vertical / kPitchLimit, 3);
    const bool changed = note != voice_.note || volume != voice_.volume_pct ||
                         key != key_index_ || scale != scale_index_;
    key_index_ = key;
    scale_index_ = scale;
    voice_ = {static_cast<uint8_t>(note), static_cast<uint8_t>(volume)};
    return changed;
}

uint8_t AirHarpArpeggio::play(int64_t now_ms, unsigned key, AirHarpScale scale, AirHarpNoteRange range) {
    constexpr int kBeatStepMs = 250;
    constexpr int patterns[][4] = {{0, 3, 7, 3}, {0, 4, 7, 4}, {0, 2, 7, 9}};
    if (step_ < 0) started_ms_ = now_ms;
    const int64_t next = (now_ms - started_ms_) / kBeatStepMs;
    if (next != step_) {
        step_ = next;
        const int interval = patterns[static_cast<unsigned>(scale)][step_ % 4];
        note_ = range.first + (key % 12 + interval) % range.count;
    }
    return note_;
}
