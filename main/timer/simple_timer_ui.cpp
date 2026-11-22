#include "simple_timer_ui.h"
#include "simple_timer_core.h"

#include "audio/voice_ui.h"
#include "ems_timer_ui.h"
#include "input_util.h"
#include "sound_fx.h"
#include "time_sync.h"
#include "ui_common.h"
#include "ui_theme.h"

#include "esp_random.h"
#include "nvs.h"

#include <array>
#include <cstdio>
#include <ctime>

namespace {
constexpr char kNvsNamespace[] = "simple_timer";
constexpr char kNvsKey[] = "time_log";
constexpr uint32_t kStampMagic = 0x31504D54;
constexpr int64_t kMemoHoldMs = 650;

struct StagePalette {
    uint32_t bg;
    uint32_t fg;
};

constexpr StagePalette kColors[SimpleTimerCore::kMaxStamps] = {
    {0xC62828, 0xFFFFFF}, {0xEF6C00, 0x111111}, {0xFBC02D, 0x111111},
    {0x2E7D32, 0xFFFFFF}, {0x1565C0, 0xFFFFFF}, {0x6A1B9A, 0xFFFFFF},
    {0xAD1457, 0xFFFFFF},
};

struct StoredLog {
    uint32_t magic;
    uint32_t generation;
    uint8_t count;
    uint8_t times[SimpleTimerCore::kMaxStamps][3];
};

SimpleTimerCore s_timer;
std::array<bool, SimpleTimerCore::kMaxStamps> s_has_memo{};
uint32_t s_generation = 0;

uint32_t new_generation() {
    const uint32_t value = esp_random();
    return value && value != s_generation ? value : (s_generation == UINT32_MAX ? 1 : s_generation + 1);
}

void load() {
    if (s_generation) return;
    nvs_handle_t nvs;
    s_generation = new_generation();
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return;
    StoredLog log{};
    size_t size = sizeof(log);
    const auto result = nvs_get_blob(nvs, kNvsKey, &log, &size);
    nvs_close(nvs);
    if (result != ESP_OK || size != sizeof(log) || log.magic != kStampMagic || !log.generation ||
        log.count > SimpleTimerCore::kMaxStamps) return;

    SimpleTimerCore timer;
    for (unsigned i = 0; i < log.count; ++i) {
        const auto &v = log.times[i];
        if (v[0] > 23 || v[1] > 59 || v[2] > 59) return;
        timer.stamp(v[0], v[1], v[2]);
    }
    s_generation = log.generation;
    s_timer = timer;
    for (unsigned i = 0; i < s_timer.count(); ++i) {
        s_has_memo[i] = voice_memo_exists(s_generation, i);
    }
}

bool save(const SimpleTimerCore &timer, uint32_t generation) {
    StoredLog log{kStampMagic, generation, static_cast<uint8_t>(timer.count()), {}};
    for (size_t i = 0; i < timer.count(); ++i) {
        const auto &s = *timer.at(i);
        log.times[i][0] = s.hour;
        log.times[i][1] = s.min;
        log.times[i][2] = s.sec;
    }
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) return false;
    const bool success = nvs_set_blob(nvs, kNvsKey, &log, sizeof(log)) == ESP_OK &&
                         nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    if (success) {
        s_timer = timer;
        s_generation = generation;
    }
    return success;
}

void draw_timer(M5Canvas &canvas, int64_t now_ms, const char *message) {
    const auto &theme = ui_theme();
    const bool synced = time_sync_is_valid();
    const auto alarms = ems_timer_active_alarms();
    const bool alert = alarms.any() && (now_ms / 400 % 2 == 0);
    char clock[16] = "--:--:--";
    if (synced) {
        const auto utc = std::time(nullptr);
        struct tm local{};
        localtime_r(&utc, &local);
        std::strftime(clock, sizeof(clock), "%H:%M:%S", &local);
    }
    canvas.fillScreen(theme.background);
    ui_draw_header(canvas, alert ? alarms.title() : clock,
                   alert ? 0xB71C1Cu : theme.surface, alert ? 0xFFFFFFu : theme.text);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    const int w = canvas.width();
    for (size_t i = 0; i < s_timer.count(); ++i) {
        const int y = 29 + i * 24;
        const auto colors = kColors[i];
        canvas.fillRect(4, y, w - 8, 22, colors.bg);
        canvas.setTextColor(colors.fg, colors.bg);
        char time[12];
        SimpleTimerCore::format_time(*s_timer.at(i), time, sizeof(time));
        canvas.drawString(time, 11, y + 3);
        if (s_has_memo[i]) ui_draw_mic_icon(canvas, w - 24, y + 4, colors.fg);
    }

    if (synced && !s_timer.is_full()) {
        const int y = 29 + s_timer.count() * 24;
        canvas.drawRect(4, y, w - 8, 22, theme.border);
        canvas.setTextColor(theme.muted);
        canvas.drawString("--:--:--", 11, y + 3);
    }
    if (s_timer.count() == 0) {
        canvas.setTextColor(theme.muted);
        canvas.drawCentreString(synced ? "No timestamps yet" : "Clock not set", w / 2, 103);
        if (!synced) canvas.drawCentreString("Open Time Sync", w / 2, 126);
    }

    if (message) ui_draw_themed_footer(canvas, message, "B Open list");
    else if (!synced) ui_draw_themed_footer(canvas, "B Open list", "Hold A+B Back");
    else if (s_timer.is_full()) ui_draw_themed_footer(canvas, "All 7 slots used", "B Open list");
    else ui_draw_themed_footer(canvas, "A Stamp   B List", "Hold A: Voice memo");
    ui_present(canvas);
}

bool confirm_clear(M5Canvas &canvas) {
    input_wait_all_release();
    bool redraw = true;
    for (;;) {
        input_update();
        if (input_a_pressed() || input_ab_held(500)) {
            input_wait_all_release();
            return false;
        }
        if (input_b_pressed()) {
            input_wait_all_release();
            return true;
        }
        if (!ui_screen_is_on()) redraw = true;
        if (redraw && ui_screen_is_on()) {
            const auto &theme = ui_theme();
            canvas.fillScreen(theme.background);
            ui_draw_header(canvas, "CLEAR LOG", theme.surface, theme.text);
            canvas.fillRoundRect(4, 42, canvas.width() - 8, 143, 4, theme.surface);
            canvas.drawRoundRect(4, 42, canvas.width() - 8, 143, 4, theme.border);
            canvas.setTextColor(theme.danger);
            canvas.drawCentreString("Clear time log?", canvas.width() / 2, 56);
            canvas.setTextColor(theme.text);
            canvas.drawCentreString("Deletes all times", canvas.width() / 2, 85);
            canvas.drawCentreString("and voice memos.", canvas.width() / 2, 105);
            ui_draw_themed_footer(canvas, "A Cancel  B OK", "Hold A+B Back");
            ui_present(canvas);
            redraw = false;
        }
        input_idle_delay(20);
    }
}

void show_log(M5Canvas &canvas) {
    input_wait_all_release();
    const size_t count = s_timer.count();
    size_t selected = 0;
    const char *status = nullptr;
    bool redraw = true;
    for (;;) {
        input_update();
        if (input_ab_held(500)) break;
        if (input_a_pressed() && count) {
            selected = (selected + 1) % (count + 1);
            sound_fx_click();
            status = nullptr;
            redraw = true;
        } else if (input_b_pressed()) {
            if (!count) break;
            if (selected == count) {
                if (confirm_clear(canvas)) {
                    const SimpleTimerCore cleared;
                    if (save(cleared, new_generation())) {
                        s_has_memo.fill(false);
                        voice_prune_memos(s_generation);
                        sound_fx_confirm();
                        break;
                    }
                    status = "Clear failed";
                }
            } else if (s_has_memo[selected]) {
                char stamp[12];
                SimpleTimerCore::format_time(*s_timer.at(selected), stamp, sizeof(stamp));
                voice_play_memo(s_generation, selected, stamp);
            } else {
                status = "No voice memo";
                sound_fx_click();
            }
            redraw = true;
        }

        if (!ui_screen_is_on()) redraw = true;
        if (redraw && ui_screen_is_on()) {
            const auto &theme = ui_theme();
            canvas.fillScreen(theme.background);
            ui_draw_header(canvas, "TIME LOG", theme.surface, theme.text);
            if (!count) {
                canvas.setTextColor(theme.muted);
                canvas.drawCentreString("No time records", canvas.width() / 2, 103);
            } else {
                for (size_t i = 0; i <= count; ++i) {
                    const int y = 29 + i * 21;
                    const bool active = i == selected;
                    const auto bg = active ? theme.selected : (i % 2 ? theme.alternate : theme.surface);
                    canvas.fillRect(3, y, canvas.width() - 6, 20, bg);
                    if (active) canvas.fillRect(3, y, 3, 20, theme.on_selected);
                    if (i < count) canvas.fillRect(6, y + 3, 2, 14, kColors[i].bg);
                    char label[24] = "Clear records";
                    if (i < count) SimpleTimerCore::format_time(*s_timer.at(i), label, sizeof(label));
                    canvas.setTextColor(active ? theme.on_selected : theme.text, bg);
                    canvas.drawString(label, 11, y + 2);
                    canvas.setTextColor(active ? theme.on_selected : theme.accent, bg);
                    canvas.drawRightString(i == count ? ">" : s_has_memo[i] ? "MEMO" : "--",
                                           canvas.width() - 8, y + 2);
                }
            }
            const char *controls = !count ? "B Back" : selected == count ? "A Next   B Open"
                                  : s_has_memo[selected] ? "A Next   B Play" : "A Next";
            ui_draw_themed_footer(canvas, controls, status ? status : "Hold A+B Back");
            ui_present(canvas);
            redraw = false;
        }
        input_idle_delay(20);
    }
    input_wait_all_release();
}
} // namespace

void simple_timer_ui_run() {
    load();
    M5Canvas canvas(&M5.Display);
    if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) return;
    input_wait_all_release();

    bool tracking = false;
    bool was_a_down = false;
    int64_t press_ms = 0;
    bool stamp_valid = false;
    struct tm pressed_time{};
    int64_t next_draw = 0;
    const char *message = nullptr;

    auto stamp = [&](bool memo) {
        if (!stamp_valid || s_timer.is_full()) {
            sound_fx_click();
            return;
        }
        const auto index = s_timer.count();
        SimpleTimerCore updated = s_timer;
        updated.stamp(pressed_time.tm_hour, pressed_time.tm_min, pressed_time.tm_sec);
        if (!save(updated, s_generation)) {
            message = "Stamp save failed";
            sound_fx_click();
            return;
        }
        if (memo) {
            char label[12];
            SimpleTimerCore::format_time(*s_timer.at(index), label, sizeof(label));
            const auto result = voice_record_memo(s_generation, index, label);
            s_has_memo[index] = result == VoiceResult::Saved || result == VoiceResult::SavedForAlarm;
        } else {
            sound_fx_confirm();
        }
    };

    for (;;) {
        input_update();
        const auto now = input_now_ms();
        if (input_ab_held(600)) break;
        const bool a_down = input_a_down();
        if (a_down && input_b_down()) tracking = false;
        else if (a_down && !was_a_down) {
            tracking = true;
            message = nullptr;
            press_ms = now;
            const auto utc = std::time(nullptr);
            stamp_valid = time_sync_is_valid() && localtime_r(&utc, &pressed_time) != nullptr;
        } else if (a_down && tracking && now - press_ms >= kMemoHoldMs) {
            tracking = false;
            stamp(true);
            next_draw = 0;
        } else if (!a_down && was_a_down && tracking) {
            tracking = false;
            stamp(false);
            next_draw = 0;
        }
        was_a_down = a_down;

        if (input_b_pressed()) {
            tracking = false;
            message = nullptr;
            sound_fx_click();
            show_log(canvas);
            next_draw = 0;
        }
        if (ui_screen_is_on() && now >= next_draw) {
            draw_timer(canvas, now, message);
            next_draw = now + 120;
        }
        input_idle_delay(20);
    }
    input_wait_ab_release();
}
