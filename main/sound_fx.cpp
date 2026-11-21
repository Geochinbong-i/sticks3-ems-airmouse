#include "sound_fx.h"
#include "app_settings.h"
#include "air_harp/air_harp_core.h"
#include "timer/ems_timer_ui.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>

namespace {

enum class SoundCue : uint8_t { Wake, Click, Confirm, EpiLogged };

QueueHandle_t s_queue = nullptr;
std::atomic<bool> s_stop_requested{false};
std::atomic<unsigned> s_pending_alarms{0};
std::atomic<unsigned> s_requested_harp{0};
std::atomic<uint8_t> s_volume{0};
constexpr unsigned kEpiAlarm = 1;
constexpr unsigned kPresetAlarm = 2;
constexpr int kHarpChannel = 0;
constexpr int kCueChannel = 1;
constexpr unsigned kSampleRate = 24000;
constexpr size_t kBlockSamples = kSampleRate / 100; // 10 ms per block

struct HarpAudio {
    // The speaker can hold two blocks. Rotate three buffers as each slot opens.
    std::array<std::array<int16_t, kBlockSamples>, 3> blocks{};
    size_t next_block = 0;
    unsigned target_voice = 0;
    float phase = 0;
    float rate = 0;
    float duty = 0.25f;
    float target_rate = 0;
    float target_duty = 0.25f;
    float gain = 0;
    bool active = false;

    void stop() {
        if (active) M5.Speaker.stop(kHarpChannel);
        reset();
    }

    void reset() {
        active = false;
        gain = 0;
    }

    void update(unsigned voice) {
        if (voice == 0) {
            stop();
            return;
        }
        if (M5.Speaker.isPlaying(kHarpChannel) >= 2) return;
        if (target_voice != voice) {
            target_voice = voice;
            target_rate = air_harp_note_hz((voice >> 8) - 1) / kSampleRate;
            target_duty = air_harp_duty_fraction(voice & 0xFF);
        }
        if (!active) {
            rate = target_rate;
            duty = target_duty;
        }

        auto &block = blocks[next_block];
        for (auto &sample : block) {
            rate += 0.006f * (target_rate - rate);
            duty += 0.004f * (target_duty - duty);
            gain += 0.02f * (1.0f - gain);

            const float end = phase + rate;
            // Average the pulse over this sample interval so fractional edges
            // remain audible even when the duty step is smaller than one sample.
            const float high = std::clamp(duty - phase, 0.0f, rate)
                             + std::clamp(end - 1.0f, 0.0f, duty);
            sample = static_cast<int16_t>(24000.0f * gain * (high / rate - duty));
            phase = end >= 1.0f ? end - 1.0f : end;
        }
        if (M5.Speaker.playRaw(block.data(), block.size(), kSampleRate, false, 1, kHarpChannel)) {
            next_block = (next_block + 1) % blocks.size();
            active = true;
        }
    }
};

HarpAudio s_harp;

uint8_t configured_volume() {
    return g_settings.sound_muted ? 0 : std::clamp(g_settings.sound_volume, 0, 100) * 255 / 100;
}

bool interruptible_sleep_ms(uint32_t ms, bool interrupted_by_alarm = false) {
    for (uint32_t elapsed = 0; elapsed < ms;) {
        if (s_stop_requested.load() || (interrupted_by_alarm && s_pending_alarms.load())) return false;
        const uint32_t step = std::min<uint32_t>(ms - elapsed, 10);
        vTaskDelay(pdMS_TO_TICKS(step));
        elapsed += step;
    }
    return !s_stop_requested.load() && (!interrupted_by_alarm || !s_pending_alarms.load());
}

unsigned take_alarm() {
    const unsigned pending = s_pending_alarms.load();
    const unsigned next = (pending & kEpiAlarm) ? kEpiAlarm : pending & kPresetAlarm;
    if (next == 0) return 0;
    return s_pending_alarms.fetch_and(~next) & next;
}

void play_alarm(unsigned alarm, uint8_t volume) {
    const auto active = ems_timer_active_alarms();
    if (!(alarm == kEpiAlarm ? active.epi : active.preset)) return;
    s_harp.stop();
    if (volume == 0 || s_stop_requested.load() || !M5.Speaker.isEnabled()) return;

    if (alarm == kEpiAlarm) {
        M5.Speaker.setVolume(235);
        for (int i = 0; i < 3 && !s_stop_requested.load(); ++i) {
            M5.Speaker.tone(2400.0f, 90, kCueChannel);
            if (!interruptible_sleep_ms(130)) break;
        }
    } else {
        M5.Speaker.setVolume(volume);
        M5.Speaker.tone(1568.0f, 100, kCueChannel);
        if (interruptible_sleep_ms(130)) {
            M5.Speaker.tone(1975.5f, 140, kCueChannel);
            interruptible_sleep_ms(140);
        }
    }
    M5.Speaker.setVolume(s_volume.load());
}

void sound_task(void *) {
    uint8_t applied_volume = s_volume.load();
    while (true) {
        const uint8_t volume = s_volume.load();
        if (volume != applied_volume) {
            M5.Speaker.setVolume(volume);
            applied_volume = volume;
        }
        if (s_stop_requested.exchange(false)) {
            M5.Speaker.stop();
            s_harp.reset();
        }

        const unsigned alarm = take_alarm();
        if (alarm != 0) {
            play_alarm(alarm, volume);
            continue;
        }

        const unsigned requested = s_requested_harp.load();
        const unsigned voice = requested != 0 && volume != 0 && !ems_timer_active_alarms().any()
                             ? requested : 0;
        s_harp.update(voice);

        SoundCue cue;
        const TickType_t timeout = voice != 0 ? pdMS_TO_TICKS(3) :
                                   M5.Speaker.isRunning() ? pdMS_TO_TICKS(300) : portMAX_DELAY;
        if (xQueueReceive(s_queue, &cue, timeout) != pdTRUE) {
            // The StickS3's external audio rail turns off with the speaker.
            if (voice == 0 && M5.Speaker.isRunning() && !M5.Speaker.isPlaying()) M5.Speaker.end();
            continue;
        }
        if (cue == SoundCue::Wake || !M5.Speaker.isEnabled()) continue;
        if (volume == 0 || s_stop_requested.load() || s_pending_alarms.load()) continue;

        switch (cue) {
            case SoundCue::Click:
                M5.Speaker.tone(1400.0f, 25, kCueChannel);
                break;
            case SoundCue::Confirm:
                M5.Speaker.tone(950.0f, 55, kCueChannel);
                break;
            case SoundCue::EpiLogged:
                M5.Speaker.tone(1046.5f, 70, kCueChannel);
                if (interruptible_sleep_ms(85, true)) M5.Speaker.tone(1318.5f, 90, kCueChannel);
                break;
            default:
                break;
        }
    }
}

void post_cue(SoundCue cue) {
    if (s_queue) xQueueSend(s_queue, &cue, 0);
}

void wake_sound_task() {
    // If the queue is full the worker already has work and will see the atomics.
    const SoundCue cue = SoundCue::Wake;
    if (s_queue) xQueueSendToFront(s_queue, &cue, 0);
}

} // namespace

void sound_fx_init() {
    if (s_queue) return;
    s_volume.store(configured_volume());
    M5.Speaker.setVolume(s_volume.load());
    s_queue = xQueueCreate(8, sizeof(SoundCue));
    if (!s_queue) return;
    if (xTaskCreate(sound_task, "sound_fx", 3072, nullptr, 4, nullptr) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = nullptr;
    }
}

void sound_fx_apply_volume() {
    s_volume.store(configured_volume());
    wake_sound_task();
}
void sound_fx_click() { post_cue(SoundCue::Click); }
void sound_fx_confirm() { post_cue(SoundCue::Confirm); }
void sound_fx_epi_logged() { post_cue(SoundCue::EpiLogged); }

void sound_fx_alarms(bool epi, bool preset) {
    const unsigned alarms = (epi ? kEpiAlarm : 0u) | (preset ? kPresetAlarm : 0u);
    if (alarms != 0) {
        s_pending_alarms.fetch_or(alarms);
        wake_sound_task();
    }
}

void sound_fx_stop() {
    if (!s_queue) return;
    s_stop_requested.store(true);
    s_pending_alarms.store(0);
    xQueueReset(s_queue);
    wake_sound_task();
}

void sound_fx_harp_play(uint8_t note, uint8_t duty) {
    if (note >= kAirHarpNoteCount || duty >= kAirHarpDutyCount) return;
    const unsigned voice = ((static_cast<unsigned>(note) + 1) << 8) | duty;
    if (s_requested_harp.exchange(voice) == 0) wake_sound_task();
}

void sound_fx_harp_stop() {
    if (s_requested_harp.exchange(0) != 0) wake_sound_task();
}
