#pragma once

#include <cstdint>

inline constexpr int kAirHarpFirstMidi = 36; // C2
inline constexpr int kAirHarpNoteCount = 24;  // Through B3
inline constexpr int kAirHarpAvailableNotes = 36;  // Through B4
inline constexpr int kAirHarpMaxVolumePct = 100;

enum class AirHarpRange : uint8_t { Oct2, Oct3, Oct4, Oct2To3, Oct2To4, Count };
enum class AirHarpScale : uint8_t { Minor, Major, Pentatonic };

struct AirHarpNoteRange {
    int first;
    int count;
    const char *label;
    const char *notes;
};

AirHarpNoteRange air_harp_range(AirHarpRange range);
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
    explicit AirHarpCore(AirHarpRange range = AirHarpRange::Oct2To3, int turn_limit = 45);
    bool set_neutral(float ax, float ay, float az, int64_t now_ms);
    void sample_accel(float ax, float ay, float az);
    bool sample_gyro(float gx, float gy, float gz, bool flip_180, int64_t now_ms);
    AirHarpVoice voice() const { return voice_; }
    unsigned key() const { return (key_index_ + 6) % 12; }
    AirHarpScale scale() const { return static_cast<AirHarpScale>(scale_index_); }

private:
    AirHarpNoteRange range_;
    int turn_limit_;
    AirHarpVoice voice_{};
    int key_index_ = 6;
    int scale_index_ = 1;
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

class AirHarpArpeggio {
public:
    uint8_t play(int64_t now_ms, unsigned key, AirHarpScale scale, AirHarpNoteRange range);
    void stop() { step_ = -1; }

private:
    int64_t started_ms_ = 0;
    int64_t step_ = -1;
    uint8_t note_ = 0;
};
