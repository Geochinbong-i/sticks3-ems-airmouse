#pragma once

#include <cstdint>

inline constexpr int kAirHarpFirstMidi = 36; // C2
inline constexpr int kAirHarpNoteCount = 24;  // Through B3
inline constexpr int kAirHarpMaxVolumePct = 100;

float air_harp_note_hz(int note);
void air_harp_note_label(int note, char (&label)[4]);
uint8_t air_harp_channel_volume(int volume_pct);

struct AirHarpVoice {
    static constexpr int center_note = 12; // C3
    static constexpr int center_volume_pct = kAirHarpMaxVolumePct / 2;

    uint8_t note = center_note;
    uint8_t volume_pct = center_volume_pct;
};

class AirHarpCore {
public:
    bool set_neutral(float ax, float ay, float az, int64_t now_ms);
    void sample_accel(float ax, float ay, float az);
    bool sample_gyro(float gx, float gy, float gz, bool flip_180, int64_t now_ms);
    AirHarpVoice voice() const { return voice_; }

private:
    AirHarpVoice voice_{};
    float up_x_ = 0;
    float up_y_ = 0;
    float up_z_ = 1;
    float center_x_ = 0;
    float center_y_ = 0;
    float center_z_ = 1;
    float pitch_tangent_x_ = 0;
    float pitch_tangent_y_ = 1;
    float pitch_tangent_z_ = 0;
    float yaw_ = 0;
    float filtered_yaw_ = 0;
    int64_t last_gyro_ms_ = 0;
};
