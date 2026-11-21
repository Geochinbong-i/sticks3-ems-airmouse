#include "simple_timer_ui.h"
#include "simple_timer_core.h"
#include "ems_timer_ui.h"
#include "input_util.h"
#include "sound_fx.h"
#include "time_sync.h"
#include "ui_common.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <ctime>

namespace {

struct StagePalette {
    uint32_t bg;
    uint32_t fg;
};

// Red -> Orange -> Yellow -> Green -> Blue -> Purple -> Pink
constexpr StagePalette kStageColors[SimpleTimerCore::kMaxStamps] = {
    {0xC62828, 0xFFFFFF},
    {0xEF6C00, 0xFFFFFF},
    {0xFBC02D, 0x111111},
    {0x2E7D32, 0xFFFFFF},
    {0x1565C0, 0xFFFFFF},
    {0x6A1B9A, 0xFFFFFF},
    {0xAD1457, 0xFFFFFF},
};

SimpleTimerCore s_simple_timer;

void get_wall_hms(int &h, int &m, int &s) {
    time_t now = std::time(nullptr);
    struct tm t{};
    localtime_r(&now, &t);
    h = t.tm_hour;
    m = t.tm_min;
    s = t.tm_sec;
}

bool confirm_simple_reset(M5Canvas &canvas) {
    input_wait_all_release();
    while (true) {
        input_update();
        if (input_ab_held(500) || input_a_pressed()) {
            sound_fx_click();
            input_wait_all_release();
            return false;
        }
        if (input_b_pressed()) {
            sound_fx_confirm();
            input_wait_all_release();
            return true;
        }

        if (!ui_screen_is_on()) {
            input_idle_delay(25);
            continue;
        }

        int w = canvas.width();
        canvas.fillScreen(0x140A0A);
        ui_draw_header(canvas, "RESET TIME", 0x2B1212, 0xFF8A80);

        canvas.drawRoundRect(6, 32, w - 12, 168, 7, 0xFF5252);
        canvas.fillRoundRect(7, 33, w - 14, 166, 6, 0x1B0D0D);

        canvas.setFont(&fonts::Font2);
        canvas.setTextSize(1);
        canvas.setTextWrap(false);

        canvas.setTextColor(0xFF8A80, 0x1B0D0D);
        canvas.setCursor(14, 42);
        canvas.print("RESET ALL?");

        canvas.setTextColor(0xE2E8F0, 0x1B0D0D);
        canvas.setCursor(14, 68);
        canvas.print("Clear all stamped");
        canvas.setCursor(14, 84);
        canvas.print("timestamps now?");

        canvas.fillRoundRect(12, 114, w - 24, 30, 4, 0x183834);
        canvas.setTextColor(0x80CBC4, 0x183834);
        canvas.setCursor(18, 121);
        canvas.print("A : Keep Data");

        canvas.fillRoundRect(12, 152, w - 24, 30, 4, 0x6A1616);
        canvas.setTextColor(0xFFCDD2, 0x6A1616);
        canvas.setCursor(18, 159);
        canvas.print("B : Clear All");

        ui_draw_footer2(canvas, "A: Cancel  B: Clear", "Hold A+B : Go Back", 0x140A0A, 0xC09E9E);
        ui_present(canvas);
        input_idle_delay(25);
    }
}

void draw_simple_frame(M5Canvas &canvas, int64_t now_ms) {
    int w = canvas.width();
    int h = canvas.height();
    bool ems_alarm = ems_timer_has_active_alarm();
    bool flash_on = ((now_ms / 400) % 2) == 0;

    canvas.fillScreen(0x070A0F);

    int ch = 0, cm = 0, cs = 0;
    const bool clock_valid = time_sync_is_valid();
    char clk_buf[16] = "--:--:--";
    if (clock_valid) {
        get_wall_hms(ch, cm, cs);
        std::snprintf(clk_buf, sizeof(clk_buf), "%02d:%02d:%02d", ch, cm, cs);
    }

    uint32_t hdr_col = (ems_alarm && flash_on) ? 0xB71C1C : 0x142033;
    ui_draw_header(canvas, (ems_alarm && flash_on) ? "EPI DUE!" : clk_buf, hdr_col, 0xFFFFFF);

    constexpr int kTopY = 22;
    constexpr int kFooterH = 29;
    constexpr int kBoxH = 27; // 7 * 27 = 189px -> fits 22..211 exact with 0px gap!

    size_t cnt = s_simple_timer.count();
    for (size_t i = 0; i < cnt && i < SimpleTimerCore::kMaxStamps; i++) {
        int y = kTopY + (int)i * kBoxH;
        const StagePalette &pal = kStageColors[i];

        // Strictly square corners (0px radius) and 0px gap stacked top-to-bottom
        canvas.fillRect(0, y, w, kBoxH, pal.bg);

        const SimpleStamp *st = s_simple_timer.at(i);
        char tbuf[16];
        SimpleTimerCore::format_time(*st, tbuf, sizeof(tbuf));

        // Time-only display (no stage label, no number) centered inside each bar
        canvas.setFont(&fonts::Font4);
        canvas.setTextSize(1);
        canvas.setTextWrap(false);
        canvas.setTextColor(pal.fg, pal.bg);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString(tbuf, w / 2, y + kBoxH / 2 + 1);
    }
    canvas.setTextDatum(TL_DATUM);

    if (cnt == 0) {
        int cy = kTopY + (h - kTopY - kFooterH - 64) / 2;
        canvas.drawRoundRect(8, cy, w - 16, 64, 6, 0x24344C);
        canvas.fillRoundRect(9, cy + 1, w - 18, 62, 5, 0x0F1621);
        canvas.setFont(&fonts::Font2);
        canvas.setTextSize(1);
        canvas.setTextWrap(false);
        canvas.setTextColor(0x80D8FF, 0x0F1621);
        canvas.setCursor(16, cy + 8);
        canvas.print("READY (0/7)");
        canvas.setTextColor(0xCFD8DC, 0x0F1621);
        canvas.setCursor(16, cy + 28);
        canvas.print("Press A to stamp");
        canvas.setCursor(16, cy + 44);
        canvas.print("current time.");
    } else if (cnt < SimpleTimerCore::kMaxStamps) {
        // Subtle next-slot hairline preview so user sees where next press stacks
        int ny = kTopY + (int)cnt * kBoxH;
        canvas.fillRect(0, ny, w, kBoxH, 0x0F1622);
        canvas.drawFastHLine(0, ny + kBoxH - 1, w, kStageColors[cnt].bg);
        canvas.setFont(&fonts::Font4);
        canvas.setTextSize(1);
        canvas.setTextColor(0x455A64, 0x0F1622);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString("--:--:--", w / 2, ny + kBoxH / 2 + 1);
        canvas.setTextDatum(TL_DATUM);
    }

    ui_draw_footer2(canvas, clock_valid ? "A: Stamp   B: Reset" : "Unset     B: Reset",
                    "Hold A+B : Go Home");
    ui_present(canvas);
}

} // namespace

void simple_timer_ui_run() {
    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    input_wait_all_release();

    int64_t last_draw_ms = 0;

    while (true) {
        input_update();
        int64_t now_ms = input_now_ms();

        if (input_ab_held(600)) {
            sound_fx_click();
            input_wait_ab_release();
            canvas.deleteSprite();
            return;
        }

        if (input_a_pressed()) {
            int h = 0, m = 0, s = 0;
            const bool clock_valid = time_sync_is_valid();
            if (clock_valid) get_wall_hms(h, m, s);
            if (clock_valid && s_simple_timer.stamp(h, m, s)) {
                sound_fx_confirm();
            } else {
                sound_fx_click();
            }
            last_draw_ms = 0;
        }

        if (input_b_pressed()) {
            if (s_simple_timer.count() > 0) {
                sound_fx_click();
                if (confirm_simple_reset(canvas)) {
                    s_simple_timer.reset();
                }
            } else {
                sound_fx_click();
            }
            last_draw_ms = 0;
        }

        if (ui_screen_is_on() && (now_ms - last_draw_ms) >= 120) {
            last_draw_ms = now_ms;
            draw_simple_frame(canvas, now_ms);
        }

        input_idle_delay(20);
    }
}
