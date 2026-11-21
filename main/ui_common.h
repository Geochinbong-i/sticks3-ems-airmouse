#pragma once

#include "M5Unified.h"

#include <cstdint>

void ui_apply_brightness();
void ui_init_backlight();
void ui_note_activity(int64_t now_ms);
bool ui_update_screen_power(int64_t now_ms, bool button_down);
void ui_present(M5Canvas &canvas);
bool ui_screen_is_on();

// Draws a compact vertical battery icon (or charging plug icon when charging)
// immediately to the left of the battery percentage using Font2.
void ui_draw_battery_badge(M5Canvas &canvas, int right_x, int hdr_h, uint32_t bg_col);

// Draws standard 22px header with Font2 title + optional badge text + battery/plug icon.
void ui_draw_header(M5Canvas &canvas, const char *title, uint32_t hdr_col,
                    uint32_t fg_col = 0xFFFFFF, const char *badge_txt = nullptr,
                    uint32_t badge_bg = 0x4A3208, uint32_t badge_fg = 0xFFAB40, int title_x = 4);

// Draws the 29px, two-line footer using the selected theme.
void ui_draw_themed_footer(M5Canvas &canvas, const char *line1, const char *line2);
