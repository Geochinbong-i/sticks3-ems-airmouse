#include "sound_fx.h"
#include "app_settings.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void sound_fx_apply_volume() {
    if (!M5.Speaker.isEnabled()) return;
    if (g_settings.sound_muted) {
        M5.Speaker.setVolume(0);
        return;
    }
    int v = g_settings.sound_volume;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    M5.Speaker.setVolume((uint8_t)((v * 255) / 100));
}

static void tone_if_unmuted(float hz, uint32_t ms) {
    if (g_settings.sound_muted || !M5.Speaker.isEnabled()) return;
    sound_fx_apply_volume();
    M5.Speaker.tone(hz, ms);
}

void sound_fx_click() {
    tone_if_unmuted(1400.0f, 25);
}

void sound_fx_confirm() {
    tone_if_unmuted(950.0f, 55);
}

void sound_fx_epi_logged() {
    if (g_settings.sound_muted || !M5.Speaker.isEnabled()) return;
    sound_fx_apply_volume();
    M5.Speaker.tone(1046.5f, 70);
    vTaskDelay(pdMS_TO_TICKS(85));
    M5.Speaker.tone(1318.5f, 90);
}

void sound_fx_epi_due() {
    if (!M5.Speaker.isEnabled() || g_settings.sound_muted) return;
    // Boost slightly for emergency overdue alert while respecting mute
    uint8_t old_vol = M5.Speaker.getVolume();
    M5.Speaker.setVolume(235);
    for (int i = 0; i < 3; i++) {
        M5.Speaker.tone(2400.0f, 90);
        vTaskDelay(pdMS_TO_TICKS(130));
    }
    M5.Speaker.setVolume(old_vol);
}

void sound_fx_preset_due() {
    if (!M5.Speaker.isEnabled() || g_settings.sound_muted) return;
    sound_fx_apply_volume();
    M5.Speaker.tone(1568.0f, 100);
    vTaskDelay(pdMS_TO_TICKS(130));
    M5.Speaker.tone(1975.5f, 140);
}

void sound_fx_stop() {
    if (M5.Speaker.isEnabled()) {
        M5.Speaker.stop();
    }
}
