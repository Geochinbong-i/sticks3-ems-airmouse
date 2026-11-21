#pragma once

#include "app_settings.h"

struct UiTheme {
    uint32_t background;
    uint32_t surface;
    uint32_t alternate;
    uint32_t text;
    uint32_t muted;
    uint32_t border;
    uint32_t accent;
    uint32_t selected;
    uint32_t on_selected;
    uint32_t danger;
    uint32_t warning;
};

inline const UiTheme &ui_theme() {
    static constexpr UiTheme dark{
        0x0E1420, 0x172234, 0x1E2B40, 0xF3F6FC, 0xC1CEE1, 0x677C96,
        0x9AD8FF, 0x2D4D78, 0xF7FAFF, 0xFFB7B1, 0xFFDA99,
    };
    static constexpr UiTheme light{
        0xF2F5FA, 0xFFFFFF, 0xE5ECF7, 0x17243A, 0x465770, 0x6B7C95,
        0x184C86, 0x194779, 0xFFFFFF, 0x931F25, 0x774400,
    };
    return g_settings.light_theme ? light : dark;
}
