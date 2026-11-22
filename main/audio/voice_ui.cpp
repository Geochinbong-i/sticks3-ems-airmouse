#include "voice_ui.h"
#include "voice_store.h"

#include "app_settings.h"
#include "input_util.h"
#include "sound_fx.h"
#include "time_sync.h"
#include "timer/ems_timer_ui.h"
#include "ui_common.h"
#include "ui_theme.h"

#include "esp_heap_caps.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <memory>

namespace {
constexpr int kPlaybackChannel = 2;
constexpr size_t kSamplesPerRequest = kVoiceSampleRate / 10;
using Samples = std::unique_ptr<uint8_t, decltype(&std::free)>;

Samples allocate(size_t bytes) {
    auto *ptr = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ptr) ptr = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    return Samples(ptr, &std::free);
}

void duration_label(size_t samples, char (&label)[12]) {
    const unsigned seconds = samples / kVoiceSampleRate;
    std::snprintf(label, sizeof(label), "%02u:%02u", seconds / 60, seconds % 60);
}

void header(M5Canvas &canvas, const char *title) {
    const auto &theme = ui_theme();
    canvas.fillScreen(theme.background);
    ui_draw_header(canvas, title, theme.surface, theme.text);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);
}

void center(M5Canvas &canvas, const char *text, int y, uint32_t color) {
    canvas.setTextColor(color);
    canvas.drawCentreString(text, canvas.width() / 2, y);
}

void activity(M5Canvas &canvas, const char *title, const char *label,
              size_t progress, size_t limit, bool recording) {
    const auto &theme = ui_theme();
    const uint32_t accent = recording ? theme.danger : theme.accent;
    header(canvas, title);
    canvas.fillRoundRect(4, 43, canvas.width() - 8, 111, 4, theme.surface);
    canvas.drawRoundRect(4, 43, canvas.width() - 8, 111, 4, theme.border);
    canvas.fillCircle(19, 59, 3, accent);
    canvas.setTextColor(accent);
    canvas.drawString(recording ? "RECORDING" : "PLAYING", 29, 51);
    char elapsed[12], total[12], second[24];
    duration_label(progress, elapsed);
    duration_label(limit, total);
    canvas.setFont(&fonts::Font4);
    center(canvas, elapsed, 75, theme.text);
    const int bar_width = canvas.width() - 26;
    canvas.fillRect(13, 112, bar_width, 5, theme.alternate);
    canvas.fillRect(13, 112, limit ? std::min(progress, limit) * bar_width / limit : 0, 5, accent);
    canvas.setFont(&fonts::Font2);
    if (recording) std::snprintf(second, sizeof(second), "%u sec max", (unsigned)(limit / kVoiceSampleRate));
    else std::snprintf(second, sizeof(second), "Total %s", total);
    center(canvas, second, 125, theme.muted);
    center(canvas, label, 167, theme.muted);
    std::snprintf(second, sizeof(second), "Stops at %u sec", (unsigned)(limit / kVoiceSampleRate));
    ui_draw_themed_footer(canvas, recording ? "A Stop and save" : "B Stop playback",
                         recording ? second : "Hold A+B Back");
    ui_present(canvas);
}

const char *result_message(VoiceResult result) {
    switch (result) {
        case VoiceResult::Saved: return "Recording saved";
        case VoiceResult::SavedForAlarm: return "Saved for EMS alert";
        case VoiceResult::NoSpace: return "Storage is full";
        case VoiceResult::Unavailable: return "Mic unavailable";
        case VoiceResult::Failed: return "Save failed";
    }
    return "";
}

void notice(const char *title, const char *line1, const char *line2 = nullptr) {
    M5Canvas canvas(&M5.Display);
    if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) return;
    input_wait_all_release();
    bool redraw = true;
    for (;;) {
        input_update();
        if (input_b_pressed() || input_ab_held(500)) break;
        if (!ui_screen_is_on()) redraw = true;
        if (redraw && ui_screen_is_on()) {
            const auto &theme = ui_theme();
            header(canvas, title);
            center(canvas, line1, 83, theme.text);
            if (line2) center(canvas, line2, 105, theme.muted);
            ui_draw_themed_footer(canvas, "B Back", "Hold A+B Back");
            ui_present(canvas);
            redraw = false;
        }
        input_idle_delay(20);
    }
    input_wait_all_release();
}

VoiceResult record(const char *path, const char *title, const char *label, unsigned max_sec) {
    if (!voice_store_init()) return VoiceResult::Failed;
    const size_t limit = voice_store_capacity(max_sec * kVoiceSampleRate) / kVoiceSampleRate * kVoiceSampleRate;
    if (!limit) return VoiceResult::NoSpace;
    if (!M5.Mic.isEnabled() || ems_timer_active_alarms().any()) return VoiceResult::Unavailable;

    auto pcm = allocate(limit);
    M5Canvas canvas(&M5.Display);
    if (!pcm || !canvas.createSprite(M5.Display.width(), M5.Display.height()) ||
        !sound_fx_acquire_voice()) return VoiceResult::Unavailable;
    auto mic_config = M5.Mic.config();
    mic_config.sample_rate = kVoiceSampleRate;
    M5.Mic.config(mic_config);
    if (!M5.Mic.begin()) {
        M5.Mic.end();
        sound_fx_release_voice();
        return VoiceResult::Unavailable;
    }

    const int64_t started = input_now_ms();
    const int64_t utc = time_sync_is_valid() ? std::time(nullptr) : 0;
    const unsigned alarm_sequence = ems_timer_alarm_sequence();
    const int64_t deadline = started + limit * 1000 / kVoiceSampleRate + 200;
    const size_t max_requests = (limit + kSamplesPerRequest - 1) / kSamplesPerRequest;
    size_t submitted = 0, samples = 0;
    bool stop_armed = !input_a_down();
    bool alarm = false;
    int64_t next_draw = 0;

    while (true) {
        input_update();
        const int64_t now = input_now_ms();
        ui_note_activity(now);
        const size_t pending = M5.Mic.isRecording();
        const size_t done = submitted - std::min(pending, submitted);
        samples = std::min(limit, done * kSamplesPerRequest);
        alarm = ems_timer_alarm_sequence() != alarm_sequence || ems_timer_active_alarms().any();
        if (alarm || now >= deadline || samples == limit ||
            (stop_armed && input_a_pressed()) || input_ab_held(500)) break;
        if (!input_a_down()) stop_armed = true;

        if (pending < 2 && submitted < max_requests) {
            const size_t offset = submitted * kSamplesPerRequest;
            const size_t count = std::min(kSamplesPerRequest, limit - offset);
            if (!M5.Mic.record(pcm.get() + offset, count, kVoiceSampleRate)) break;
            ++submitted;
        }
        if (now >= next_draw) {
            activity(canvas, title, label, samples, limit, true);
            next_draw = now + 100;
        }
        input_idle_delay(8);
    }

    M5.Mic.end();
    // Return I2S immediately so an EMS alarm isn't held up by a flash write.
    sound_fx_release_voice();
    const bool saved = samples && voice_store_save(path, pcm.get(), samples, utc);
    input_wait_all_release();
    return saved ? (alarm ? VoiceResult::SavedForAlarm : VoiceResult::Saved) : VoiceResult::Failed;
}

void play(const char *path, const char *title, const char *label) {
    if (g_settings.sound_muted) {
        notice(title, "Sound is muted");
        return;
    }
    VoiceClip clip{};
    if (!voice_store_info(path, clip)) {
        notice(title, "No voice memo");
        return;
    }
    auto pcm = allocate(clip.samples);
    M5Canvas canvas(&M5.Display);
    if (!pcm || !voice_store_load(path, pcm.get(), clip.samples) ||
        !canvas.createSprite(M5.Display.width(), M5.Display.height())) {
        notice(title, "Playback failed");
        return;
    }
    input_wait_all_release();
    if (ems_timer_active_alarms().any() || !sound_fx_acquire_voice()) {
        notice(title, "Playback unavailable", "Check EMS alerts");
        return;
    }
    if (!M5.Speaker.playRaw(pcm.get(), clip.samples, kVoiceSampleRate, false, 1, kPlaybackChannel)) {
        M5.Speaker.end();
        sound_fx_release_voice();
        notice(title, "Playback failed");
        return;
    }

    const int64_t started = input_now_ms();
    const auto alarm_sequence = ems_timer_alarm_sequence();
    int64_t next_draw = 0;
    while (M5.Speaker.isPlaying(kPlaybackChannel)) {
        input_update();
        const int64_t now = input_now_ms();
        ui_note_activity(now);
        if (input_b_pressed() || input_ab_held(500) || alarm_sequence != ems_timer_alarm_sequence() ||
            ems_timer_active_alarms().any()) break;
        if (now >= next_draw) {
            size_t elapsed = (now - started) * kVoiceSampleRate / 1000;
            activity(canvas, title, label, std::min(elapsed, (size_t)clip.samples), clip.samples, false);
            next_draw = now + 100;
        }
        input_idle_delay(15);
    }
    M5.Speaker.stop(kPlaybackChannel);
    M5.Speaker.end();
    sound_fx_release_voice();
    input_wait_all_release();
}

void row(M5Canvas &canvas, const char *label, const char *value, int y, bool selected) {
    const auto &theme = ui_theme();
    const uint32_t bg = selected ? theme.selected : ((y / 22) & 1 ? theme.alternate : theme.surface);
    canvas.fillRect(3, y, canvas.width() - 6, 21, bg);
    if (selected) canvas.fillRect(3, y, 3, 21, theme.on_selected);
    canvas.setTextColor(selected ? theme.on_selected : theme.text, bg);
    const int right = canvas.width() - 8;
    ui_draw_text_fit(canvas, label, 9, y + 2, right - 14 - canvas.textWidth(value));
    canvas.setTextColor(selected ? theme.on_selected : theme.accent, bg);
    canvas.drawRightString(value, right, y + 2);
}

void clip_label(uint32_t id, char (&label)[24]) {
    std::snprintf(label, sizeof(label), "Clip %03lu", static_cast<unsigned long>(id));
}

void clip_time(int64_t utc, char (&date)[10], char (&clock)[12]) {
    if (utc > 0) {
        const time_t value = utc;
        struct tm local{};
        localtime_r(&value, &local);
        std::strftime(date, sizeof(date), "%m/%d", &local);
        std::strftime(clock, sizeof(clock), "%H:%M:%S", &local);
    } else {
        std::snprintf(date, sizeof(date), "--/--");
        std::snprintf(clock, sizeof(clock), "--:--:--");
    }
}

bool confirm_delete(M5Canvas &canvas, uint32_t id) {
    input_wait_all_release();
    bool redraw = true;
    while (true) {
        input_update();
        if (input_a_pressed() || input_ab_held(500)) {
            input_wait_all_release();
            return false;
        }
        if (input_b_pressed()) {
            input_wait_all_release();
            return true;
        }
        if (!ui_screen_is_on()) redraw = true;
        if (redraw && ui_screen_is_on()) {
            const auto &theme = ui_theme();
            header(canvas, "DELETE CLIP");
            char label[32];
            std::snprintf(label, sizeof(label), "Delete clip %03lu?", static_cast<unsigned long>(id));
            center(canvas, label, 68, theme.danger);
            center(canvas, "This recording", 99, theme.text);
            center(canvas, "will be deleted.", 119, theme.text);
            ui_draw_themed_footer(canvas, "A Cancel  B OK", "Hold A+B Back");
            ui_present(canvas);
            redraw = false;
        }
        input_idle_delay(20);
    }
}
} // namespace

VoiceResult voice_record_memo(uint32_t generation, unsigned slot, const char *stamp) {
    char path[32], label[24];
    voice_memo_path(path, generation, slot);
    std::snprintf(label, sizeof(label), "Stamp %02u  %s", slot + 1, stamp);
    const auto result = record(path, "VOICE MEMO", label, kVoiceMemoSec);
    if (result != VoiceResult::Saved) notice("VOICE MEMO", result_message(result), "Timestamp saved");
    return result;
}

void voice_play_memo(uint32_t generation, unsigned slot, const char *stamp) {
    char path[32], label[24];
    voice_memo_path(path, generation, slot);
    std::snprintf(label, sizeof(label), "Stamp %02u  %s", slot + 1, stamp);
    play(path, "VOICE MEMO", label);
}

bool voice_memo_exists(uint32_t generation, unsigned slot) {
    char path[32];
    voice_memo_path(path, generation, slot);
    VoiceClip clip{};
    return voice_store_info(path, clip);
}

void voice_prune_memos(uint32_t generation) {
    voice_store_prune_memos(generation);
}

void voice_recorder_ui_run() {
    enum class Page { Ready, List, Clip };
    M5Canvas canvas(&M5.Display);
    if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) return;
    const bool healthy = voice_store_init();
    VoiceClip clips[kVoiceClipCount]{};
    size_t count = voice_store_list(clips);
    size_t selected = 0;
    int action = 0;
    Page page = Page::Ready;
    const char *status = healthy ? nullptr : "Store unavailable";
    int64_t next_draw = 0;
    input_wait_all_release();

    for (;;) {
        input_update();
        if (input_ab_held(500)) {
            input_wait_all_release();
            if (page == Page::Ready) break;
            page = page == Page::Clip ? Page::List : Page::Ready;
            next_draw = 0;
        } else if (input_a_pressed()) {
            if (page == Page::Ready && healthy) {
                if (count == kVoiceClipCount || voice_store_capacity(kVoiceSampleRate) < kVoiceSampleRate) {
                    status = "Storage is full";
                } else if (ems_timer_active_alarms().any()) {
                    status = "EMS alert active";
                } else {
                    const uint32_t id = count ? clips[0].id + 1u : 1u;
                    char path[32], label[24];
                    voice_clip_path(path, id);
                    clip_label(id, label);
                    status = result_message(record(path, "RECORDER", label, kVoiceClipSec));
                    count = voice_store_list(clips);
                }
            } else if (page == Page::List && count) {
                selected = (selected + 1) % (count + 1);
                sound_fx_click();
            } else if (page == Page::Clip) {
                action = (action + 1) % 3;
                sound_fx_click();
            }
            next_draw = 0;
        } else if (input_b_pressed()) {
            if (page == Page::Ready) {
                page = Page::List;
                selected = 0;
                sound_fx_click();
            } else if (page == Page::List) {
                page = selected == count ? Page::Ready : Page::Clip;
                action = 0;
                sound_fx_click();
            } else {
                char path[32];
                voice_clip_path(path, clips[selected].id);
                if (action == 0) {
                    play(path, "RECORDER", "Voice recording");
                } else if (action == 2) {
                    page = Page::List;
                } else if (confirm_delete(canvas, clips[selected].id)) {
                    if (voice_store_delete(path)) {
                        count = voice_store_list(clips);
                        selected = std::min(selected, count);
                        status = nullptr;
                    } else {
                        notice("RECORDER", "Delete failed");
                    }
                    page = Page::List;
                }
            }
            next_draw = 0;
        }

        const int64_t now = input_now_ms();
        if (ui_screen_is_on() && now >= next_draw) {
            const auto &theme = ui_theme();
            if (page == Page::Ready) {
                header(canvas, "RECORDER");
                canvas.fillRoundRect(4, 40, canvas.width() - 8, 112, 4, theme.surface);
                canvas.drawRoundRect(4, 40, canvas.width() - 8, 112, 4, theme.border);
                const bool full = status && std::strcmp(status, "Storage is full") == 0;
                ui_draw_mic_icon(canvas, canvas.width() / 2 - 5, 58, theme.accent);
                center(canvas, status ? status : "Ready to record", 81, theme.text);
                center(canvas, full ? "Delete a clip in List" : "60 sec per clip", 113, theme.muted);
                ui_draw_themed_footer(canvas, healthy && !full ? "A Record   B List" : "B Open list", "Hold A+B Back");
            } else if (page == Page::List) {
                header(canvas, "RECORDINGS");
                if (count == 0) {
                    center(canvas, "No recordings", 83, theme.text);
                    center(canvas, "A in Recorder", 109, theme.muted);
                } else {
                    constexpr size_t visible = 7;
                    const size_t first = selected >= visible ? selected - visible + 1 : 0;
                    for (size_t i = first; i <= count && i < first + visible; ++i) {
                        char label[24] = "< Back", length[12]{};
                        if (i < count) {
                            clip_label(clips[i].id, label);
                            duration_label(clips[i].samples, length);
                        }
                        row(canvas, label, length, 32 + 22 * (i - first), selected == i);
                    }
                    const int x = canvas.width() / 2;
                    if (first) canvas.fillTriangle(x, 24, x - 4, 29, x + 4, 29, theme.accent);
                    if (first + visible <= count) canvas.fillTriangle(x, 205, x - 4, 200, x + 4, 200, theme.accent);
                }
                ui_draw_themed_footer(canvas, count ? "A Next   B Open" : "B Back", "Hold A+B Back");
            } else {
                char label[24], length[12], date[10], clock[12];
                clip_label(clips[selected].id, label);
                header(canvas, label);
                duration_label(clips[selected].samples, length);
                clip_time(clips[selected].utc, date, clock);
                row(canvas, "Recorded", date, 32, false);
                row(canvas, "Time", clock, 54, false);
                row(canvas, "Duration", length, 76, false);
                row(canvas, "Play", "", 113, action == 0);
                row(canvas, "Delete", "", 135, action == 1);
                row(canvas, "< Back", "", 157, action == 2);
                ui_draw_themed_footer(canvas, "A Next   B Select", "Hold A+B Back");
            }
            ui_present(canvas);
            next_draw = now + 1500;
        }
        input_idle_delay(20);
    }
}
