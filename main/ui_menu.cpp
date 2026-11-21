#include "ui_menu.h"
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
        canvas.drawString(row.subtitle, text_x, y + 23);
    }
}
