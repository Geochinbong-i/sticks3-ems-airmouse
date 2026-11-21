#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

inline constexpr int kIrSlotCount = 8;
inline constexpr size_t kIrMaxPulses = 385;
inline constexpr unsigned kIrSignalGapUs = 20000;
inline constexpr unsigned kIrTxCarrierHz = 38000;

struct IrSignal {
    uint16_t count = 0;
    std::array<uint16_t, kIrMaxPulses> duration_us{};
};

// Stores alternating mark and space durations, starting with a mark.
bool ir_add_pulse(IrSignal &signal, bool mark, unsigned duration_us);
bool ir_finish_signal(IrSignal &signal);
bool ir_valid_signal(const IrSignal &signal);
bool ir_is_nec(const IrSignal &signal);
bool ir_supports_high_power(const IrSignal &signal);
