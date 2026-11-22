#pragma once

#include <cstdint>

enum class VoiceResult { Saved, SavedForAlarm, NoSpace, Unavailable, Failed };

void voice_recorder_ui_run();
VoiceResult voice_record_memo(uint32_t generation, unsigned slot, const char *stamp);
void voice_play_memo(uint32_t generation, unsigned slot, const char *stamp);
bool voice_memo_exists(uint32_t generation, unsigned slot);
void voice_prune_memos(uint32_t generation);
