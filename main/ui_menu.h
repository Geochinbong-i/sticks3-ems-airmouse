#pragma once

#include "menu_nav.h"
#include "ui_common.h"

struct MenuRow {
    const char *title;
    const char *subtitle;
    uint32_t accent;
};

void ui_draw_menu(M5Canvas &canvas, const MenuRow *rows, int count, const MenuCursor &cursor);
