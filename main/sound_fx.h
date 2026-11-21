#pragma once

#include <cstdint>

void sound_fx_init();
void sound_fx_apply_volume();
void sound_fx_click();
void sound_fx_confirm();
void sound_fx_epi_logged();
void sound_fx_epi_due();
void sound_fx_preset_due();
void sound_fx_stop();
void sound_fx_harp_play(uint8_t note, uint8_t duty);
void sound_fx_harp_stop();
