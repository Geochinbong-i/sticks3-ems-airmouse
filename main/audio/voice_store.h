#pragma once

#include <cstddef>
#include <cstdint>

inline constexpr unsigned kVoiceSampleRate = 8000;
inline constexpr unsigned kVoiceMemoSec = 10;
inline constexpr unsigned kVoiceClipSec = 60;
inline constexpr size_t kVoiceClipCount = 32;

struct VoiceClip {
    uint32_t id;
    uint32_t samples;
    int64_t utc;
};

bool voice_store_init();
size_t voice_store_capacity(size_t requested);
bool voice_store_save(const char *path, const uint8_t *pcm, size_t samples, int64_t utc);
bool voice_store_info(const char *path, VoiceClip &clip);
bool voice_store_load(const char *path, uint8_t *pcm, size_t samples);
bool voice_store_delete(const char *path);
size_t voice_store_list(VoiceClip (&clips)[kVoiceClipCount]);
void voice_clip_path(char (&path)[32], uint32_t id);
void voice_memo_path(char (&path)[32], uint32_t generation, unsigned slot);
void voice_store_prune_memos(uint32_t generation);
