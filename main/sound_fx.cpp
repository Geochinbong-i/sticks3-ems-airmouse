#include "sound_fx.h"
#include "app_settings.h"
#include "air_harp/air_harp_core.h"
#include "timer/ems_timer_ui.h"

#include "M5Unified.h"
#include "esp_timer.h"
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
std::atomic<bool> s_back_pending{false};
std::atomic<unsigned> s_pending_alarms{0};
std::atomic<bool> s_alarm_playing{false};
std::atomic<unsigned> s_requested_harp{0};
std::atomic<uint8_t> s_volume{0};
std::atomic<bool> s_ir_listening{false};
std::atomic<bool> s_ir_ready{false};
std::atomic<bool> s_voice_requested{false};
std::atomic<bool> s_voice_ready{false};
constexpr unsigned kEpiAlarm = 1;
constexpr unsigned kPresetAlarm = 2;
constexpr int kHarpChannel = 0;
constexpr int kCueChannel = 1;
constexpr int kHarpFadeStepMs = 4;
constexpr int kHarpMaxVolumeStep = 64;
constexpr unsigned kCueRate = 16000;
constexpr unsigned kLeadMs = 120;
constexpr unsigned kLeadSamples = kCueRate * kLeadMs / 1000;
constexpr unsigned kClickMs = 25;
constexpr unsigned kConfirmMs = 55;
constexpr unsigned kBackGapMs = 28;

template<unsigned FirstMs, unsigned GapMs = 0, unsigned SecondMs = 0>
constexpr auto cue_samples(unsigned first_hz, unsigned second_hz = 0) {
    std::array<uint8_t, kCueRate * (kLeadMs + FirstMs + GapMs + SecondMs) / 1000> samples{};
    for (auto &sample : samples) sample = 128;
    constexpr int sine[] = {0, 49, 90, 117, 127, 117, 90, 49, 0, -49, -90, -117, -127, -117, -90, -49};
    auto add = [&](unsigned offset_ms, unsigned length_ms, unsigned hz) {
        const unsigned length = kCueRate * length_ms / 1000;
        const unsigned offset = kCueRate * offset_ms / 1000;
        constexpr unsigned ramp = kCueRate / 1000;
        for (unsigned i = 0; i < length; ++i) {
            const unsigned phase = (i * hz * std::size(sine) / kCueRate) % std::size(sine);
            const int amount = std::min({i + 1, length - i, ramp});
            samples[offset + i] = 128 + sine[phase] * amount / static_cast<int>(ramp);
        }
    };
    add(kLeadMs, FirstMs, first_hz);
    if (SecondMs) add(kLeadMs + FirstMs + GapMs, SecondMs, second_hz);
    return samples;
}

constexpr auto kClick = cue_samples<kClickMs>(1400);
constexpr auto kConfirm = cue_samples<kConfirmMs>(950);
constexpr auto kBack = cue_samples<kClickMs, kBackGapMs, kConfirmMs>(1400, 950);
constexpr auto kEpi = cue_samples<70, 15, 90>(1047, 1319);

template<size_t Samples>
void play_cue(const std::array<uint8_t, Samples> &wave, bool interrupt = false) {
    // Feed silence on cold starts so the codec and amplifier are ready before
    // the first audible sample. Queue subsequent cues behind that first one.
    const size_t start = M5.Speaker.isRunning() && !interrupt ? kLeadSamples : 0;
    M5.Speaker.playRaw(wave.data() + start, wave.size() - start, kCueRate, false, 1, kCueChannel, interrupt);
}

constexpr auto kHarpWave = [] {
    std::array<uint8_t, 64> wave{};
    // A 25% pulse centered at 128, with the same peak level as the PCM voice.
    for (size_t i = 0; i < wave.size(); ++i) wave[i] = i < wave.size() / 4 ? 197 : 105;
    return wave;
}();

struct HarpAudio {
    int note = -1;
    int volume = 0;
    int64_t next_volume_step_ms = 0;

    bool playing() const { return note >= 0; }

    void stop() {
        if (playing()) M5.Speaker.stop(kHarpChannel);
        reset();
    }

    void reset() {
        note = -1;
        volume = 0;
        next_volume_step_ms = 0;
    }

    // Returns true while the channel still needs volume steps or a retry.
    bool update(unsigned voice, int64_t now_ms) {
        if (voice == 0 || !M5.Speaker.isEnabled()) {
            stop();
            return false;
        }

        const int target_volume = air_harp_channel_volume(voice & 0xFF);
        const int target_note = (voice >> 8) - 1;
        if (target_volume > 0 && target_note != note) {
            if (!playing()) M5.Speaker.setChannelVolume(kHarpChannel, 0);
            // The mixer loops this waveform on its own and keeps the waveform
            // index when the same data is submitted at a different frequency.
            if (!M5.Speaker.tone(air_harp_note_hz(target_note), UINT32_MAX, kHarpChannel,
                                 true, kHarpWave.data(), kHarpWave.size())) {
                return true;
            }
            note = target_note;
        }
        if (volume != target_volume && now_ms >= next_volume_step_ms) {
            volume += std::clamp(target_volume - volume, -kHarpMaxVolumeStep, kHarpMaxVolumeStep);
            // The mixer squares the channel level, preserving the prior gain curve.
            M5.Speaker.setChannelVolume(kHarpChannel, volume);
            next_volume_step_ms = now_ms + kHarpFadeStepMs;
        }
        if (volume == 0 && target_volume == 0 && playing()) stop();
        return volume != target_volume;
    }
};

HarpAudio s_harp;

uint8_t configured_volume() {
    return g_settings.sound_muted ? 0 : std::clamp(g_settings.sound_volume, 0, 100) * 255 / 100;
}

bool interruptible_sleep_ms(uint32_t ms) {
    for (uint32_t elapsed = 0; elapsed < ms;) {
        if (s_stop_requested.load()) return false;
        const uint32_t step = std::min<uint32_t>(ms - elapsed, 10);
        vTaskDelay(pdMS_TO_TICKS(step));
        elapsed += step;
    }
    return !s_stop_requested.load();
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
        if (s_voice_requested.load()) {
            if (!s_voice_ready.load()) {
                s_harp.stop();
                if (M5.Speaker.isRunning()) M5.Speaker.end();
                s_voice_ready.store(true);
            }
            // The recorder owns the shared I2S port. Leave alarms queued
            // until it returns the port and discard key clicks while recording.
            SoundCue discarded;
            xQueueReceive(s_queue, &discarded, pdMS_TO_TICKS(10));
            continue;
        }
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
            s_ir_ready.store(false);
            s_alarm_playing.store(true);
            play_alarm(alarm, volume);
            s_alarm_playing.store(false);
            continue;
        }

        const bool ir_quiet = s_ir_listening.load();
        if (ir_quiet) {
            s_harp.stop();
            if (M5.Speaker.isRunning()) M5.Speaker.end();
            s_ir_ready.store(true);
        }
        const unsigned requested = s_requested_harp.load();
        const unsigned voice = !ir_quiet && requested != 0 && volume != 0 && !ems_timer_active_alarms().any()
                             ? requested : 0;
        const bool changing_harp = s_harp.update(voice, esp_timer_get_time() / 1000);
        if (volume == 0 || ir_quiet || !M5.Speaker.isEnabled()) {
            s_back_pending.store(false);
        } else if (M5.Speaker.isPlaying(kCueChannel) < 2 && s_back_pending.exchange(false)) {
            play_cue(kBack);
        }
        const bool check_idle_speaker = !s_harp.playing() && M5.Speaker.isRunning();

        SoundCue cue;
        const bool waiting_back = s_back_pending.load();
        const TickType_t timeout = changing_harp ? pdMS_TO_TICKS(kHarpFadeStepMs) :
                                   waiting_back ? pdMS_TO_TICKS(10) :
                                   check_idle_speaker ? pdMS_TO_TICKS(1200) : portMAX_DELAY;
        if (xQueueReceive(s_queue, &cue, timeout) != pdTRUE) {
            // The StickS3's external audio rail turns off with the speaker.
            if (!changing_harp && !waiting_back && check_idle_speaker && !M5.Speaker.isPlaying()) M5.Speaker.end();
            continue;
        }
        if (cue == SoundCue::Wake || s_voice_requested.load() ||
            s_ir_listening.load() || !M5.Speaker.isEnabled()) continue;
        if (volume == 0 || s_stop_requested.load() || s_pending_alarms.load()) continue;
        // Never make the worker wait for a mixer slot while it services alarms
        // and I2S handoffs. The dedicated Back request above waits separately.
        if (cue != SoundCue::EpiLogged && M5.Speaker.isPlaying(kCueChannel) >= 2) continue;

        switch (cue) {
            case SoundCue::Click: play_cue(kClick); break;
            case SoundCue::Confirm: play_cue(kConfirm); break;
            case SoundCue::EpiLogged: play_cue(kEpi, true); break;
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
void sound_fx_back() {
    s_back_pending.store(true);
    wake_sound_task();
}
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
    s_back_pending.store(false);
    xQueueReset(s_queue);
    wake_sound_task();
}

void sound_fx_harp_play(uint8_t note, uint8_t volume_pct) {
    if (note >= kAirHarpAvailableNotes || volume_pct > kAirHarpMaxVolumePct) return;
    const unsigned voice = ((static_cast<unsigned>(note) + 1) << 8) | volume_pct;
    if (s_requested_harp.exchange(voice) != voice) wake_sound_task();
}

void sound_fx_harp_stop() {
    if (s_requested_harp.exchange(0) != 0) wake_sound_task();
}

void sound_fx_set_ir_listening(bool enabled) {
    s_ir_ready.store(false);
    s_ir_listening.store(enabled);
    wake_sound_task();
}

bool sound_fx_ir_ready() {
    return !s_queue || s_ir_ready.load();
}

bool sound_fx_acquire_voice() {
    if (!s_queue) return false;
    s_voice_ready.store(false);
    s_voice_requested.store(true);
    wake_sound_task();
    const int64_t deadline = esp_timer_get_time() + 750000;
    while (!s_voice_ready.load() && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (s_voice_ready.load()) return true;
    sound_fx_release_voice();
    return false;
}

void sound_fx_release_voice() {
    s_voice_requested.store(false);
    wake_sound_task();
}

bool sound_fx_alarm_busy() {
    return s_pending_alarms.load() || s_alarm_playing.load();
}
