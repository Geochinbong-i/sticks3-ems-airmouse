#include "ir_store.h"

#include "nvs.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace {
constexpr char kNamespace[] = "ir_remote";
constexpr const char *kKeys[kIrSlotCount] = {
    "slot1", "slot2", "slot3", "slot4", "slot5", "slot6", "slot7", "slot8"
};

struct Record {
    uint16_t version;
    uint16_t count;
    uint8_t carrier_khz;
    uint8_t high_power;
    uint8_t playback;
    uint8_t repeat_count;
    uint8_t repeat_gap_ms;
    uint8_t nec;
    uint16_t ticks[kIrMaxPulses];
};

constexpr size_t kHeaderSize = offsetof(Record, ticks);
constexpr size_t kLegacySize = (2 + 385) * sizeof(uint16_t);
} // namespace

bool ir_load_slot(int slot, IrSignal &signal, IrProfile &profile) {
    if (slot < 0 || slot >= kIrSlotCount) return false;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;

    Record record{};
    size_t size = sizeof(record);
    const auto result = nvs_get_blob(handle, kKeys[slot], &record, &size);
    nvs_close(handle);
    if (result != ESP_OK || size < 2 * sizeof(uint16_t)) return false;

    signal.count = record.count;
    if (record.version == 1 && size == kLegacySize && record.count <= 385) {
        const auto *data = reinterpret_cast<const unsigned char *>(&record) + 2 * sizeof(uint16_t);
        for (size_t i = 0; i < record.count; ++i) {
            uint16_t duration;
            std::memcpy(&duration, data + i * sizeof(duration), sizeof(duration));
            signal.duration_us[i] = duration;
        }
        return ir_valid_signal(signal);
    }
    if (record.version != 2 || record.count > kIrMaxPulses ||
        size != kHeaderSize + record.count * sizeof(uint16_t)) return false;

    for (size_t i = 0; i < record.count; ++i) signal.duration_us[i] = record.ticks[i] * kIrTickUs;
    const IrProfile loaded{record.carrier_khz, record.high_power != 0,
                           static_cast<IrPlayback>(record.playback), record.repeat_count,
                           record.repeat_gap_ms, record.nec != 0};
    if (!ir_valid_signal(signal) || !ir_valid_profile(loaded, signal)) return false;
    profile = loaded;
    return true;
}

bool ir_save_slot(int slot, const IrSignal &signal, const IrProfile &profile) {
    if (slot < 0 || slot >= kIrSlotCount || !ir_valid_signal(signal) ||
        !ir_valid_profile(profile, signal)) return false;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;

    Record record{2, signal.count, profile.carrier_khz, profile.high_power,
                  static_cast<uint8_t>(profile.playback), profile.repeat_count,
                  profile.repeat_gap_ms, profile.nec, {}};
    for (size_t i = 0; i < signal.count; ++i) {
        record.ticks[i] = (signal.duration_us[i] + kIrTickUs / 2) / kIrTickUs;
    }
    const size_t size = kHeaderSize + signal.count * sizeof(uint16_t);
    const bool saved = nvs_set_blob(handle, kKeys[slot], &record, size) == ESP_OK &&
                       nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return saved;
}
