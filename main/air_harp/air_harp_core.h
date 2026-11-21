#pragma once

#include <cstdint>

struct AirHarpNote {
    const char *name;
    float hz;
};

inline constexpr AirHarpNote kAirHarpNotes[] = {
    {"C4", 261.63f}, {"D4", 293.66f}, {"E4", 329.63f}, {"G4", 392.00f},
    {"A4", 440.00f}, {"C5", 523.25f}, {"D5", 587.33f}, {"E5", 659.25f},
};
inline constexpr int kAirHarpNoteCount = sizeof(kAirHarpNotes) / sizeof(kAirHarpNotes[0]);
inline constexpr const char *kAirHarpDuties[] = {"12.5%", "25%", "37.5%", "50%"};

struct AirHarpVoice {
    uint8_t note = 3;
    uint8_t duty = 1;
};

class AirHarpCore {
public:
    void reset_motion();
    bool set_neutral(float ax, float ay, float az);
    bool sample(int64_t now_ms, float gx, float gy, float gz, bool flip_180);

    AirHarpVoice voice() const { return voice_; }

private:
    AirHarpVoice voice_{};
    float up_x_ = 0.0f;
    float up_y_ = 0.0f;
    float up_z_ = 1.0f;
    int64_t quiet_since_ms_ = -1;
    int64_t rearm_after_ms_ = 0;
    bool armed_ = false;
};
