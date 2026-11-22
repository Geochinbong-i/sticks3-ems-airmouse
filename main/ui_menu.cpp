#include "ui_menu.h"
#include "input_util.h"
#include "sound_fx.h"
#include "ui_theme.h"

#include <algorithm>

void ui_draw_menu(M5Canvas &canvas, const MenuRow *rows, int count, const MenuCursor &cursor) {
    const int center = canvas.width() / 2;
    const int footer = canvas.height() - 29;
    constexpr int kTop = 32;
    constexpr int kX = 2;
    const int width = canvas.width() - 2 * kX;
    const int height = (footer - 11 - kTop) / kMenuVisibleRows;
    const auto &theme = ui_theme();
    const uint32_t arrow = theme.accent;
    if (cursor.top > 0) canvas.fillTriangle(center, 25, center - 4, 29, center + 4, 29, arrow);
    if (cursor.top + kMenuVisibleRows < count) {
        canvas.fillTriangle(center - 4, footer - 8, center + 4, footer - 8, center, footer - 4, arrow);
    }

    canvas.setTextWrap(false);
    canvas.setTextDatum(TL_DATUM);
    for (int visible = 0; visible < kMenuVisibleRows && cursor.top + visible < count; ++visible) {
        const int index = cursor.top + visible;
        const auto &row = rows[index];
        const int y = kTop + visible * height;
        const uint32_t bg = index % 2 == 0 ? theme.surface : theme.alternate;
        canvas.fillRect(kX, y, width, height, bg);
        if (index == cursor.selected) {
            const uint32_t accent = g_settings.light_theme ? theme.accent : row.accent;
            canvas.drawRect(kX, y, width, height, accent);
            canvas.fillTriangle(kX + 1, y + 1, kX + 12, y + 1, kX + 1, y + 12, accent);
        }

        constexpr int text_x = kX + 9;
        canvas.setFont(&fonts::FreeSansBold9pt7b);
        canvas.setTextSize(1);
        const int text_w = canvas.textWidth(row.title);
        const float scale = text_w > 0
            ? std::min(1.0f, static_cast<float>(canvas.width() - text_x - 5) / text_w) : 1.0f;
        canvas.setTextSize(scale, 1.0f);
        canvas.setTextColor(theme.text);
        canvas.drawString(row.title, text_x, y + 7);

        canvas.setFont(&fonts::Font2);
        canvas.setTextSize(1);
        canvas.setTextColor(theme.muted);
        ui_draw_text_fit(canvas, row.subtitle, text_x, y + 23, canvas.width() - text_x - 5);
    }
}

int ui_choose_menu(const char *title, const MenuRow *rows, int count) {
    M5Canvas canvas(&M5.Display);
    if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) return -1;
    input_wait_all_release();
    MenuCursor cursor;
    MenuButtons buttons;
    int64_t next_draw = 0;
    for (;;) {
        input_update();
        const auto now = input_now_ms();
        const auto action = buttons.update(input_a_down(), input_b_down(), now, false);
        if (action == MenuAction::Select || action == MenuAction::Back) {
            if (action == MenuAction::Back) sound_fx_back();
            else sound_fx_confirm();
            input_wait_all_release();
            return action == MenuAction::Select ? cursor.selected : -1;
        }
        if (action == MenuAction::Next || action == MenuAction::Previous) {
            cursor.move(action == MenuAction::Next ? 1 : -1, count);
            sound_fx_click();
            next_draw = 0;
        }
        if (ui_screen_is_on() && now >= next_draw) {
            const auto &theme = ui_theme();
            canvas.fillScreen(theme.background);
            ui_draw_header(canvas, title, theme.surface, theme.text);
            ui_draw_menu(canvas, rows, count, cursor);
            ui_draw_themed_footer(canvas, "A Next   B Open", "Hold A+B Back");
            ui_present(canvas);
            next_draw = now + 2000;
        }
        input_idle_delay(20);
    }
}
