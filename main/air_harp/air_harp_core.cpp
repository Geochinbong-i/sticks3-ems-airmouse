#include "air_harp_core.h"

#include "airmouse/airmouse_math.h"

#include <algorithm>
#include <cmath>

void AirHarpCore::reset_motion() {
    quiet_since_ms_ = -1;
    rearm_after_ms_ = 0;
    armed_ = false;
}

bool AirHarpCore::set_neutral(float ax, float ay, float az) {
    const float norm = std::sqrt(ax * ax + ay * ay + az * az);
    if (!std::isfinite(norm) || norm < 0.75f || norm > 1.25f) return false;
    up_x_ = ax / norm;
    up_y_ = ay / norm;
    up_z_ = az / norm;
    reset_motion();
    return true;
}

bool AirHarpCore::sample(int64_t now_ms, float gx, float gy, float gz, bool flip_180) {
    if (!std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(gz)) {
        quiet_since_ms_ = -1;
        armed_ = false;
        return false;
    }
    const auto rates = airmouse_project_motion(gx, gy, gz, up_x_, up_y_, up_z_, flip_180);
    const float horizontal = std::fabs(rates.horizontal);
    const float vertical = std::fabs(rates.vertical);

    if (std::max(horizontal, vertical) < 45.0f) {
        if (quiet_since_ms_ < 0) quiet_since_ms_ = now_ms;
        if (now_ms >= rearm_after_ms_ && now_ms - quiet_since_ms_ >= 65) armed_ = true;
        return false;
    }
    quiet_since_ms_ = -1;
    if (!armed_ || std::max(horizontal, vertical) < 125.0f) return false;

    // Rearm only after settling, including after an ambiguous diagonal snap.
    armed_ = false;
    rearm_after_ms_ = now_ms + 200;
    constexpr float kDominance = 1.25f;
    if (horizontal >= vertical * kDominance) {
        const int direction = rates.horizontal > 0 ? 1 : -1;
        voice_.note = std::clamp(voice_.note + direction, 0, kAirHarpNoteCount - 1);
    } else if (vertical >= horizontal * kDominance) {
        // The shared projection uses screen coordinates, where negative Y is up.
        const int direction = rates.vertical < 0 ? 1 : -1;
        constexpr int kMaxDuty = sizeof(kAirHarpDuties) / sizeof(kAirHarpDuties[0]) - 1;
        voice_.duty = std::clamp(voice_.duty + direction, 0, kMaxDuty);
    } else {
        return false;
    }
    return true;
}
