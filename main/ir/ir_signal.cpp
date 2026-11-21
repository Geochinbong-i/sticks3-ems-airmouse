#include "ir_signal.h"

namespace {
constexpr unsigned kMinPulseUs = 60;
constexpr unsigned kMaxFrameUs = 400000;
constexpr unsigned kMinPulseCount = 9;

bool close_to(unsigned value, unsigned nominal, unsigned tolerance) {
    return value >= nominal - tolerance && value <= nominal + tolerance;
}
} // namespace

bool ir_add_pulse(IrSignal &signal, bool mark, unsigned duration_us) {
    if (duration_us == 0 || (signal.count == 0 && !mark)) return true;
    if (duration_us > kIrSignalGapUs) return false;

    if (signal.count != 0 && (((signal.count % 2) != 0) == mark)) {
        auto &previous = signal.duration_us[signal.count - 1];
        if (previous + duration_us > kIrSignalGapUs) return false;
        previous += duration_us;
        return true;
    }
    if (signal.count == signal.duration_us.size()) return false;
    signal.duration_us[signal.count++] = duration_us;
    return true;
}

bool ir_finish_signal(IrSignal &signal) {
    if (signal.count != 0 && signal.count % 2 == 0) --signal.count;
    return ir_valid_signal(signal);
}

bool ir_valid_signal(const IrSignal &signal) {
    if (signal.count < kMinPulseCount || signal.count > signal.duration_us.size() ||
        signal.count % 2 == 0) return false;
    unsigned total = 0;
    for (size_t i = 0; i < signal.count; ++i) {
        const auto duration = signal.duration_us[i];
        if (duration < kMinPulseUs || duration > kIrSignalGapUs) return false;
        total += duration;
    }
    return total <= kMaxFrameUs;
}

bool ir_is_nec(const IrSignal &signal) {
    if (signal.count != 67 || !close_to(signal.duration_us[0], 9000, 1800) ||
        !close_to(signal.duration_us[1], 4500, 900) ||
        !close_to(signal.duration_us[66], 560, 220)) return false;

    uint32_t bits = 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (!close_to(signal.duration_us[2 + i * 2], 560, 220)) return false;
        const auto space = signal.duration_us[3 + i * 2];
        if (close_to(space, 1690, 500)) bits |= uint32_t{1} << i;
        else if (!close_to(space, 560, 250)) return false;
    }
    // NEC and extended NEC both carry an inverted command byte.
    return (((bits >> 16) ^ (bits >> 24)) & 0xFF) == 0xFF;
}
