#include "voice_store.h"

#include "esp_partition.h"
#include "esp_spiffs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>

namespace {
constexpr char kPartition[] = "voice";
constexpr char kPath[] = "/voice";
constexpr char kDraft[] = "/voice/draft";
constexpr uint32_t kMagic = 0x31434956u;
constexpr size_t kMaxBytes = kVoiceClipSec * kVoiceSampleRate;

struct FileHeader {
    uint32_t magic;
    uint32_t samples;
    int64_t utc;
};
static_assert(sizeof(FileHeader) == 16);

bool is_erased(const esp_partition_t *partition) {
    std::array<uint8_t, 512> sector{};
    for (size_t offset = 0; offset < partition->size; offset += sector.size()) {
        const auto size = std::min<size_t>(sector.size(), partition->size - offset);
        if (esp_partition_read(partition, offset, sector.data(), size) != ESP_OK ||
            std::any_of(sector.begin(), sector.begin() + size, [](uint8_t byte) { return byte != 0xff; })) {
            return false;
        }
    }
    return true;
}

bool valid_header(FILE *file, FileHeader &header) {
    if (std::fread(&header, sizeof(header), 1, file) != 1 || header.magic != kMagic ||
        header.samples == 0 || header.samples > kMaxBytes) return false;
    return std::fseek(file, 0, SEEK_END) == 0 &&
           std::ftell(file) == static_cast<long>(sizeof(FileHeader) + header.samples) &&
           std::fseek(file, sizeof(FileHeader), SEEK_SET) == 0;
}
} // namespace

bool voice_store_init() {
    static bool tried = false;
    static bool ready = false;
    if (tried) return ready;
    tried = true;
    const auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, kPartition);
    if (!partition) return false;

    esp_vfs_spiffs_conf_t config{};
    config.base_path = kPath;
    config.partition_label = kPartition;
    config.max_files = 4;
    ready = esp_vfs_spiffs_register(&config) == ESP_OK;
    if (!ready && is_erased(partition)) {
        // An app-only update or a corrupt recording must never trigger a format.
        config.format_if_mount_failed = true;
        ready = esp_vfs_spiffs_register(&config) == ESP_OK;
    }
    if (ready) std::remove(kDraft);
    return ready;
}

size_t voice_store_capacity(size_t requested) {
    size_t total = 0, used = 0;
    if (!voice_store_init() || esp_spiffs_info(kPartition, &total, &used) != ESP_OK) return 0;
    // Keep free flash blocks available for SPIFFS garbage collection and metadata.
    const size_t usable = total - total / 4;
    constexpr size_t reserve = 8192;
    return usable > used + reserve ? std::min(requested, usable - used - reserve) : 0;
}

bool voice_store_save(const char *path, const uint8_t *pcm, size_t samples, int64_t utc) {
    if (!voice_store_init() || samples == 0 || samples > kMaxBytes) return false;
    FILE *file = std::fopen(kDraft, "wb");
    if (!file) return false;
    const FileHeader header{kMagic, static_cast<uint32_t>(samples), utc};
    bool written = std::fwrite(&header, sizeof(header), 1, file) == 1;
    for (size_t offset = 0; written && offset < samples; offset += 4096) {
        const size_t count = std::min<size_t>(4096, samples - offset);
        written = std::fwrite(pcm + offset, 1, count, file) == count;
    }
    const bool closed = std::fclose(file) == 0;
    if (written && closed && std::rename(kDraft, path) == 0) return true;
    std::remove(kDraft);
    return false;
}

bool voice_store_info(const char *path, VoiceClip &clip) {
    if (!voice_store_init()) return false;
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;
    FileHeader header{};
    const bool ok = valid_header(file, header);
    std::fclose(file);
    if (ok) {
        clip.samples = header.samples;
        clip.utc = header.utc;
    }
    return ok;
}

bool voice_store_load(const char *path, uint8_t *pcm, size_t samples) {
    if (!voice_store_init()) return false;
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;
    FileHeader header{};
    const bool read = valid_header(file, header) && header.samples == samples &&
                      std::fread(pcm, 1, samples, file) == samples;
    std::fclose(file);
    return read;
}

bool voice_store_delete(const char *path) {
    return voice_store_init() && (std::remove(path) == 0 || errno == ENOENT);
}

void voice_clip_path(char (&path)[32], uint32_t id) {
    std::snprintf(path, sizeof(path), "/voice/r%08lX", static_cast<unsigned long>(id));
}

void voice_memo_path(char (&path)[32], uint32_t generation, unsigned slot) {
    std::snprintf(path, sizeof(path), "/voice/m%08lX_%u", static_cast<unsigned long>(generation), slot);
}

size_t voice_store_list(VoiceClip (&clips)[kVoiceClipCount]) {
    if (!voice_store_init()) return 0;
    DIR *dir = opendir(kPath);
    if (!dir) return 0;
    size_t count = 0;
    while (const auto *item = readdir(dir)) {
        const auto *name = item->d_name;
        if (name[0] != 'r' || std::strlen(name) != 9 || count == kVoiceClipCount) continue;
        char *end = nullptr;
        const auto id = std::strtoul(name + 1, &end, 16);
        if (*end != '\0' || id == 0) continue;
        char path[32];
        voice_clip_path(path, id);
        VoiceClip clip{static_cast<uint32_t>(id), 0, 0};
        if (voice_store_info(path, clip)) clips[count++] = clip;
    }
    closedir(dir);
    std::sort(clips, clips + count, [](const VoiceClip &a, const VoiceClip &b) { return a.id > b.id; });
    return count;
}

void voice_store_prune_memos(uint32_t generation) {
    if (!voice_store_init()) return;
    char prefix[11];
    std::snprintf(prefix, sizeof(prefix), "m%08lX_", static_cast<unsigned long>(generation));
    DIR *dir = opendir(kPath);
    if (!dir) return;
    while (const auto *item = readdir(dir)) {
        const auto *name = item->d_name;
        if (std::strlen(name) != 11 || name[0] != 'm' || name[9] != '_' ||
            std::strncmp(name, prefix, sizeof(prefix) - 1) == 0) continue;
        char path[32];
        std::snprintf(path, sizeof(path), "/voice/%.11s", name);
        std::remove(path);
    }
    closedir(dir);
}
