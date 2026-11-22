#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

inline constexpr int kIrSlotCount = 8;
inline constexpr size_t kIrMaxPulses = 769;
inline constexpr unsigned kIrSignalGapUs = 20000;
inline constexpr unsigned kIrLongGapUs = 120000;
inline constexpr unsigned kIrTickUs = 4;
inline constexpr unsigned kIrTxCarrierHz = 38000;

struct IrSignal {
    uint16_t count = 0;
    std::array<uint32_t, kIrMaxPulses> duration_us{};
};

enum class IrPlayback : uint8_t { Once, Captured, Repeat };

struct IrProfile {
    uint8_t carrier_khz = 38;
    bool high_power = false;
    IrPlayback playback = IrPlayback::Captured;
    uint8_t repeat_count = 2;
    uint8_t repeat_gap_ms = 80;
    bool nec = false;
};

// Stores alternating mark and space durations, starting with a mark.
bool ir_add_pulse(IrSignal &signal, bool mark, unsigned duration_us);
bool ir_finish_signal(IrSignal &signal);
bool ir_valid_signal(const IrSignal &signal);
bool ir_valid_profile(const IrProfile &profile, const IrSignal &signal);
bool ir_is_nec(const IrSignal &signal);
bool ir_supports_high_power(const IrSignal &signal);
unsigned ir_frame_count(const IrSignal &signal);
bool ir_build_tx(const IrSignal &signal, const IrProfile &profile, IrSignal &output);
