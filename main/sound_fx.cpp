#include "sound_fx.h"
#include "app_settings.h"
#include "air_harp/air_harp_core.h"
#include "timer/ems_timer_ui.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <array>
#include <atomic>

namespace {

enum class SoundCue : uint8_t {
    ApplyVolume = 0,
    Click,
    Confirm,
    EpiLogged,
    EpiDue,
    PresetDue,
    Stop,
    HarpUpdate,
};

QueueHandle_t s_queue = nullptr;
std::atomic<bool> s_cancel_seq{false};
std::atomic<unsigned> s_requested_harp{0};
constexpr int kHarpChannel = 0;

constexpr std::array<uint8_t, 32> pulse_wave(size_t high_samples) {
    std::array<uint8_t, 32> wave{};
    constexpr unsigned amplitude = 96;
    const unsigned low = 128 - amplitude * high_samples / wave.size();
    for (size_t i = 0; i < wave.size(); ++i) {
        wave[i] = low + (i < high_samples ? amplitude : 0);
    }
    return wave;
}

constexpr std::array kHarpWaves = {pulse_wave(4), pulse_wave(8), pulse_wave(12), pulse_wave(16)};
static_assert(kHarpWaves.size() == sizeof(kAirHarpDuties) / sizeof(kAirHarpDuties[0]));

uint8_t calc_vol_255() {
    if (g_settings.sound_muted) return 0;
    int v = g_settings.sound_volume;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return (uint8_t)((v * 255) / 100);
}

bool interruptible_sleep_ms(uint32_t ms) {
    uint32_t elapsed = 0;
    while (elapsed < ms) {
        if (s_cancel_seq.load(std::memory_order_acquire)) {
            return false;
        }
        uint32_t step = (ms - elapsed > 15) ? 15 : (ms - elapsed);
        vTaskDelay(pdMS_TO_TICKS(step));
        elapsed += step;
    }
    return !s_cancel_seq.load(std::memory_order_acquire);
}

void update_harp(unsigned &playing) {
    const unsigned requested = s_requested_harp.load();
    const unsigned voice = requested != 0 && !g_settings.sound_muted && !ems_timer_has_active_alarm()
        ? requested : 0;
    if (voice == playing) return;
    if (voice == 0) {
        M5.Speaker.stop(kHarpChannel);
        playing = 0;
        return;
    }

    const unsigned note = (voice >> 8) - 1;
    const unsigned duty = voice & 0xFF;
    const auto &wave = kHarpWaves[duty];
    M5.Speaker.setVolume(calc_vol_255());
    if (M5.Speaker.tone(kAirHarpNotes[note].hz, UINT32_MAX, kHarpChannel, true,
                        wave.data(), wave.size())) {
        playing = voice;
    }
}

void sound_task(void *arg) {
    (void)arg;
    SoundCue cue;
    unsigned harp_playing = 0;
    while (true) {
        const TickType_t idle_timeout = M5.Speaker.isRunning()
            ? pdMS_TO_TICKS(300) : portMAX_DELAY;
        if (xQueueReceive(s_queue, &cue, idle_timeout) != pdTRUE) {
            // end() also turns off the StickS3's external audio power rail.
            if (M5.Speaker.isRunning() && !M5.Speaker.isPlaying()) M5.Speaker.end();
            continue;
        }

        if (!M5.Speaker.isEnabled()) continue;

        if (cue == SoundCue::Stop) {
            s_cancel_seq.store(false, std::memory_order_release);
            M5.Speaker.stop();
            harp_playing = 0;
            continue;
        }
        update_harp(harp_playing);
        if (cue == SoundCue::HarpUpdate) continue;
        if (cue == SoundCue::ApplyVolume) {
            M5.Speaker.setVolume(calc_vol_255());
            continue;
        }

        if (g_settings.sound_muted) continue;

        s_cancel_seq.store(false, std::memory_order_release);
        uint8_t normal_vol = calc_vol_255();
        M5.Speaker.setVolume(normal_vol);

        switch (cue) {
            case SoundCue::Click:
                M5.Speaker.tone(1400.0f, 25);
                break;
            case SoundCue::Confirm:
                M5.Speaker.tone(950.0f, 55);
                break;
            case SoundCue::EpiLogged:
                M5.Speaker.tone(1046.5f, 70);
                if (interruptible_sleep_ms(85)) {
                    M5.Speaker.tone(1318.5f, 90);
                }
                break;
            case SoundCue::EpiDue:
                M5.Speaker.stop(kHarpChannel);
                harp_playing = 0;
                M5.Speaker.setVolume(235);
                for (int i = 0; i < 3; i++) {
                    if (s_cancel_seq.load(std::memory_order_acquire)) break;
                    M5.Speaker.tone(2400.0f, 90);
                    if (!interruptible_sleep_ms(130)) break;
                }
                M5.Speaker.setVolume(calc_vol_255());
                break;
            case SoundCue::PresetDue:
                M5.Speaker.stop(kHarpChannel);
                harp_playing = 0;
                M5.Speaker.tone(1568.0f, 100);
                if (interruptible_sleep_ms(130)) {
                    M5.Speaker.tone(1975.5f, 140);
                }
                break;
            default:
                break;
        }
    }
}

void post_cue(SoundCue cue) {
    if (!s_queue) return;
    if (cue == SoundCue::Stop) {
        s_cancel_seq.store(true, std::memory_order_release);
        xQueueReset(s_queue);
    } else if (cue == SoundCue::EpiDue || cue == SoundCue::PresetDue) {
        // Urgent alarm cancels lower-priority pending click sequences
        s_cancel_seq.store(true, std::memory_order_release);
        xQueueSendToFront(s_queue, &cue, 0);
        return;
    }
    xQueueSend(s_queue, &cue, 0);
}

} // namespace

void sound_fx_init() {
    if (s_queue) return;
    s_queue = xQueueCreate(8, sizeof(SoundCue));
    xTaskCreate(sound_task, "sound_fx", 2560, nullptr, 4, nullptr);
}

void sound_fx_apply_volume() { post_cue(SoundCue::ApplyVolume); }
void sound_fx_click() { post_cue(SoundCue::Click); }
void sound_fx_confirm() { post_cue(SoundCue::Confirm); }
void sound_fx_epi_logged() { post_cue(SoundCue::EpiLogged); }
void sound_fx_epi_due() { post_cue(SoundCue::EpiDue); }
void sound_fx_preset_due() { post_cue(SoundCue::PresetDue); }
void sound_fx_stop() { post_cue(SoundCue::Stop); }

void sound_fx_harp_play(uint8_t note, uint8_t duty) {
    if (note >= kAirHarpNoteCount || duty >= kHarpWaves.size()) return;
    const unsigned voice = ((static_cast<unsigned>(note) + 1) << 8) | duty;
    if (s_requested_harp.exchange(voice) != voice) post_cue(SoundCue::HarpUpdate);
}

void sound_fx_harp_stop() {
    if (s_requested_harp.exchange(0) != 0) post_cue(SoundCue::HarpUpdate);
}
