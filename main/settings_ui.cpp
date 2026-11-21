#include "settings_ui.h"

#include "app_settings.h"
#include "imu_cal.h"
#include "input_util.h"
#include "sound_fx.h"
#include "time_sync.h"
#include "ui_common.h"
#include "ui_theme.h"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <iterator>

namespace {

enum class Setting {
    Brightness, Theme, ScreenOff, Rotation, Sync, Timezone, TimerMode, EpiInterval,
    Sensitivity, InvertX, InvertY, Swap, NaturalScroll, ImuCal,
    HarpGate, Volume, Mute, IrTxPower, Reboot,
};

struct Row {
    const char *label;
    Setting setting;
};

constexpr Row kDisplay[] = {
    {"Brightness", Setting::Brightness}, {"Theme", Setting::Theme}, {"Screen Off", Setting::ScreenOff},
    {"Flip Screen", Setting::Rotation},
};
constexpr Row kTime[] = {{"Time Sync", Setting::Sync}, {"Timezone", Setting::Timezone}};
constexpr Row kTimers[] = {{"Timer Mode", Setting::TimerMode}, {"EPI Interval", Setting::EpiInterval}};
constexpr Row kAirMouse[] = {
    {"Sensitivity", Setting::Sensitivity}, {"Invert X", Setting::InvertX},
    {"Invert Y", Setting::InvertY}, {"Swap X/Y", Setting::Swap},
    {"Nat Scroll", Setting::NaturalScroll}, {"IMU Abs Cal", Setting::ImuCal},
};
constexpr Row kAirHarp[] = {{"Play Button", Setting::HarpGate}};
constexpr Row kSound[] = {{"Volume", Setting::Volume}, {"Mute", Setting::Mute}};
constexpr Row kIr[] = {{"TX Power", Setting::IrTxPower}};
constexpr Row kDevice[] = {{"Reboot Device", Setting::Reboot}};

struct Group {
    const char *label;
    const char *title;
    const Row *rows;
    int count;
};

constexpr Group kGroups[] = {
    {"Display", "DISPLAY", kDisplay, std::size(kDisplay)},
    {"Time", "TIME", kTime, std::size(kTime)},
    {"Timers", "TIMERS", kTimers, std::size(kTimers)},
    {"Air Mouse", "AIR MOUSE", kAirMouse, std::size(kAirMouse)},
    {"Air Harp", "AIR HARP", kAirHarp, std::size(kAirHarp)},
    {"Sound", "SOUND", kSound, std::size(kSound)},
    {"IR Remote", "IR REMOTE", kIr, std::size(kIr)},
    {"Device", "DEVICE", kDevice, std::size(kDevice)},
};
constexpr int kGroupCount = std::size(kGroups);

constexpr int kScreenOffSeconds[] = {0, 30, 60, 180};
constexpr const char *kScreenOffLabels[] = {"OFF", "30s", "1m", "3m"};
constexpr int kScreenOffCount = std::size(kScreenOffSeconds);

int screen_off_index() {
    for (int i = 0; i < kScreenOffCount; ++i) {
        if (g_settings.screen_off_sec == kScreenOffSeconds[i]) return i;
    }
    return 0;
}

const char *on_off(bool on) { return on ? "ON" : "OFF"; }

const char *setting_value(Setting setting, char *buf, size_t size) {
    switch (setting) {
        case Setting::Brightness: std::snprintf(buf, size, "%d%%", g_settings.brightness_pct); break;
        case Setting::Theme: return g_settings.light_theme ? "LIGHT" : "DARK";
        case Setting::IrTxPower: return g_settings.ir_tx_high ? "HIGH" : "NORMAL";
        case Setting::ScreenOff: return kScreenOffLabels[screen_off_index()];
        case Setting::Rotation: return g_settings.rotation == 2 ? "180" : "0";
        case Setting::Timezone: std::snprintf(buf, size, "UTC%+d", g_settings.tz_offset_hours); break;
        case Setting::TimerMode: return g_settings.timer_mode == 0 ? "SIMPLE" : "EMS";
        case Setting::EpiInterval: std::snprintf(buf, size, "%dm", g_settings.epi_interval_sec / 60); break;
        case Setting::Sensitivity: std::snprintf(buf, size, "%d", g_settings.air_sensitivity); break;
        case Setting::InvertX: return on_off(g_settings.air_invert_x);
        case Setting::InvertY: return on_off(g_settings.air_invert_y);
        case Setting::Swap: return on_off(g_settings.air_swap_xy);
        case Setting::NaturalScroll: return on_off(g_settings.air_natural_scroll);
        case Setting::HarpGate: return g_settings.harp_toggle ? "TOGGLE" : "HOLD";
        case Setting::Volume: std::snprintf(buf, size, "%d%%", g_settings.sound_volume); break;
        case Setting::Mute: return on_off(g_settings.sound_muted);
        case Setting::Sync:
        case Setting::ImuCal:
        case Setting::Reboot: return ">";
    }
    return buf;
}

void change_setting(Setting setting) {
    switch (setting) {
        case Setting::Brightness:
            g_settings.brightness_pct += 20;
            if (g_settings.brightness_pct > 100) g_settings.brightness_pct = 20;
            ui_apply_brightness();
            break;
        case Setting::Theme: g_settings.light_theme = !g_settings.light_theme; break;
        case Setting::IrTxPower: g_settings.ir_tx_high = !g_settings.ir_tx_high; break;
        case Setting::ScreenOff:
            g_settings.screen_off_sec =
                kScreenOffSeconds[(screen_off_index() + 1) % kScreenOffCount];
            break;
        case Setting::Rotation:
            g_settings.rotation = g_settings.rotation == 0 ? 2 : 0;
            M5.Display.setRotation(g_settings.rotation);
            break;
        case Setting::Sync:
            time_sync_ui_run();
            return;
        case Setting::Timezone:
            if (++g_settings.tz_offset_hours > 14) g_settings.tz_offset_hours = -12;
            time_sync_apply_tz();
            time_sync_save_rtc();
            break;
        case Setting::TimerMode: g_settings.timer_mode = g_settings.timer_mode == 0 ? 1 : 0; break;
        case Setting::EpiInterval:
            g_settings.epi_interval_sec += 60;
            if (g_settings.epi_interval_sec > 300) g_settings.epi_interval_sec = 180;
            break;
        case Setting::Sensitivity: g_settings.air_sensitivity = g_settings.air_sensitivity % 5 + 1; break;
        case Setting::InvertX: g_settings.air_invert_x = !g_settings.air_invert_x; break;
        case Setting::InvertY: g_settings.air_invert_y = !g_settings.air_invert_y; break;
        case Setting::Swap: g_settings.air_swap_xy = !g_settings.air_swap_xy; break;
        case Setting::NaturalScroll: g_settings.air_natural_scroll = !g_settings.air_natural_scroll; break;
        case Setting::ImuCal:
            imu_abs_cal_ui_run();
            return;
        case Setting::HarpGate: g_settings.harp_toggle = !g_settings.harp_toggle; break;
        case Setting::Volume:
            g_settings.sound_volume += 20;
            if (g_settings.sound_volume > 100) g_settings.sound_volume = 20;
            sound_fx_apply_volume();
            break;
        case Setting::Mute:
            g_settings.sound_muted = !g_settings.sound_muted;
            sound_fx_apply_volume();
            break;
        case Setting::Reboot:
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
            return;
    }
    app_settings_save();
}

void draw_settings(M5Canvas &canvas, const Group *group, int selected) {
    const int count = group ? group->count : kGroupCount;
    const int w = canvas.width();
    const int h = canvas.height();
    const auto &theme = ui_theme();
    canvas.fillScreen(theme.background);
    ui_draw_header(canvas, group ? group->title : "SETTINGS", theme.surface, theme.text);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);

    constexpr int kTop = 32;
    constexpr int kStep = 21;
    const int visible = (h - 39 - kTop) / kStep;
    const int start = selected >= visible ? selected - visible + 1 : 0;
    const int center = w / 2;
    if (start > 0) canvas.fillTriangle(center, 25, center - 4, 29, center + 4, 29, theme.accent);
    if (start + visible <= count) {
        canvas.fillTriangle(center, h - 32, center - 4, h - 36, center + 4, h - 36, theme.accent);
    }
    for (int row = start, y = kTop; row <= count && row - start < visible; ++row, y += kStep) {
        char buf[16]{};
        const char *label = "< BACK";
        const char *value = "";
        if (row < count) {
            label = group ? group->rows[row].label : kGroups[row].label;
            value = group ? setting_value(group->rows[row].setting, buf, sizeof(buf)) : ">";
        }

        const bool focused = row == selected;
        const uint32_t bg = focused ? theme.selected : (row % 2 == 0 ? theme.surface : theme.alternate);
        canvas.fillRect(3, y, w - 6, kStep - 1, bg);
        if (focused) canvas.fillRect(3, y, 3, kStep - 1, g_settings.light_theme ? theme.on_selected : theme.accent);
        canvas.setTextColor(focused ? theme.on_selected : theme.text, bg);
        canvas.drawString(label, 9, y + 2);
        canvas.setTextColor(focused ? theme.on_selected : theme.accent, bg);
        canvas.drawRightString(value, w - 7, y + 2);
    }
    ui_draw_themed_footer(canvas, group ? "A: Next    B: Edit" : "A: Next    B: Open",
                    group ? "Hold A+B : Back" : "Hold A+B : Home");
    ui_present(canvas);
}

} // namespace

void settings_ui_run() {
    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    input_wait_all_release();

    const Group *group = nullptr;
    int group_sel = 0;
    int row_sel = 0;
    int64_t next_draw_ms = 0;
    while (true) {
        input_update();
        int &selected = group ? row_sel : group_sel;
        const int back = group ? group->count : kGroupCount;
        const bool hold_back = input_ab_held(500);
        const bool open = input_b_pressed();
        if (hold_back || (open && selected == back)) {
            sound_fx_click();
            input_wait_all_release();
            if (!group) break;
            group = nullptr;
            next_draw_ms = 0;
        } else if (open) {
            sound_fx_confirm();
            if (group) {
                change_setting(group->rows[row_sel].setting);
            } else {
                group = &kGroups[group_sel];
                row_sel = 0;
            }
            next_draw_ms = 0;
        } else if (input_a_pressed()) {
            selected = (selected + 1) % (back + 1);
            sound_fx_click();
            next_draw_ms = 0;
        }

        const int64_t now_ms = input_now_ms();
        if (ui_screen_is_on() && now_ms >= next_draw_ms) {
            draw_settings(canvas, group, group ? row_sel : group_sel);
            next_draw_ms = now_ms + 2000;
        }
        input_idle_delay(25);
    }
    canvas.deleteSprite();
}
