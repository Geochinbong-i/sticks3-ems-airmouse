#include "ir_signal.h"

namespace {
constexpr unsigned kMinPulseUs = 60;
constexpr unsigned kMaxSignalUs = 1200000;
constexpr unsigned kMinPulseCount = 9;

bool close_to(unsigned value, unsigned nominal, unsigned tolerance) {
    return value >= nominal - tolerance && value <= nominal + tolerance;
}

size_t first_frame(const IrSignal &signal) {
    for (size_t i = 1; i < signal.count; i += 2) {
        if (signal.duration_us[i] >= kIrSignalGapUs) return i;
    }
    return signal.count;
}
} // namespace

bool ir_add_pulse(IrSignal &signal, bool mark, unsigned duration_us) {
    if (duration_us == 0 || (signal.count == 0 && !mark)) return true;
    const unsigned max_us = mark ? kIrSignalGapUs : kIrLongGapUs;
    if (duration_us > max_us) return false;

    if (signal.count != 0 && (((signal.count % 2) != 0) == mark)) {
        auto &previous = signal.duration_us[signal.count - 1];
        if (previous + duration_us > max_us) return false;
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
    unsigned total = 0, marks = 0;
    for (size_t i = 0; i < signal.count; ++i) {
        const auto duration = signal.duration_us[i];
        if (duration < kMinPulseUs || duration > (i % 2 ? kIrLongGapUs : kIrSignalGapUs)) return false;
        total += duration;
        if (i % 2 == 0) marks += duration;
    }
    return total <= kMaxSignalUs && marks <= 200000;
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

bool ir_valid_profile(const IrProfile &p, const IrSignal &signal) {
    return (p.carrier_khz == 36 || p.carrier_khz == 38 || p.carrier_khz == 40) &&
           p.playback <= IrPlayback::Repeat &&
           p.repeat_count >= 2 && p.repeat_count <= 3 &&
           (p.repeat_gap_ms == 40 || p.repeat_gap_ms == 80 || p.repeat_gap_ms == 120) &&
           (!p.nec || ir_is_nec(signal));
}

unsigned ir_frame_count(const IrSignal &signal) {
    unsigned count = signal.count != 0;
    for (size_t i = 1; i < signal.count; i += 2) {
        if (signal.duration_us[i] >= kIrSignalGapUs) ++count;
    }
    return count;
}

bool ir_build_tx(const IrSignal &signal, const IrProfile &p, IrSignal &output) {
    if (!ir_valid_signal(signal) || !ir_valid_profile(p, signal)) return false;
    if (p.playback == IrPlayback::Captured && !p.nec) {
        output = signal;
        return true;
    }

    const size_t pulses = first_frame(signal);
    const unsigned repeats = p.playback == IrPlayback::Repeat ? p.repeat_count : 1;
    output.count = 0;
    for (unsigned frame = 0; frame < repeats; ++frame) {
        if (frame && !ir_add_pulse(output, false, p.repeat_gap_ms * 1000u)) return false;
        for (size_t i = 0; i < pulses; ++i) {
            unsigned duration = signal.duration_us[i];
            if (p.nec) {
                if (i == 0) duration = 9000;
                else if (i == 1) duration = 4500;
                else if (i % 2 == 0) duration = 560;
                else duration = duration > 1000 ? 1690 : 560;
            }
            if (!ir_add_pulse(output, i % 2 == 0, duration)) return false;
        }
    }
    return ir_valid_signal(output);
}

bool ir_supports_high_power(const IrSignal &signal) {
    if (!ir_valid_signal(signal)) return false;
    unsigned mark_time_us = 0;
    for (size_t i = 0; i < signal.count; i += 2) {
        const unsigned mark_us = signal.duration_us[i];
        if (mark_us > 10000) return false;
        mark_time_us += mark_us;
    }
    return mark_time_us <= 32000;
}
