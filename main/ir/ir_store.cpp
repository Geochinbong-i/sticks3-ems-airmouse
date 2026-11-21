#include "ir_store.h"

#include "nvs.h"

namespace {
constexpr char kNamespace[] = "ir_remote";
constexpr const char *kKeys[kIrSlotCount] = {
    "slot1", "slot2", "slot3", "slot4", "slot5", "slot6", "slot7", "slot8"
};
constexpr uint16_t kVersion = 1;

struct Record {
    uint16_t version = kVersion;
    IrSignal signal{};
};
} // namespace

bool ir_load_slot(int slot, IrSignal &signal) {
    if (slot < 0 || slot >= kIrSlotCount) return false;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;

    Record record;
    size_t size = sizeof(record);
    const auto result = nvs_get_blob(handle, kKeys[slot], &record, &size);
    nvs_close(handle);
    if (result != ESP_OK || size != sizeof(record) || record.version != kVersion ||
        !ir_valid_signal(record.signal)) return false;

    signal = record.signal;
    return true;
}

bool ir_save_slot(int slot, const IrSignal &signal) {
    if (slot < 0 || slot >= kIrSlotCount || !ir_valid_signal(signal)) return false;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;

    const Record record{kVersion, signal};
    const bool saved = nvs_set_blob(handle, kKeys[slot], &record, sizeof(record)) == ESP_OK &&
                       nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return saved;
}
