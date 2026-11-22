#include "ui_common.h"
#include "app_settings.h"
#include "ems_timer_ui.h"
#include "esp_timer.h"
#include "power_save.h"
#include "ui_theme.h"

#include "driver/ledc.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>

static bool s_screen_off = false;
static int64_t s_last_activity_ms = 0;

void ui_note_activity(int64_t now_ms) {
    s_last_activity_ms = now_ms;
}

bool ui_update_screen_power(int64_t now_ms, bool button_down) {
    static unsigned last_alarm = 0;
    const unsigned alarm = ems_timer_alarm_sequence();
    const bool new_alarm = alarm != last_alarm;
    last_alarm = alarm;

    if (s_last_activity_ms == 0 || button_down || new_alarm) ui_note_activity(now_ms);
    if (s_screen_off && (button_down || new_alarm || g_settings.screen_off_sec == 0)) {
        s_screen_off = false;
        power_save_allow_screen_idle(false);
        M5.Display.wakeup();
        ui_apply_brightness();
        return button_down;
    }
    if (!s_screen_off && g_settings.screen_off_sec > 0 &&
        now_ms - s_last_activity_ms >= g_settings.screen_off_sec * 1000LL) {
        s_screen_off = true;
        M5.Display.sleep();
        power_save_allow_screen_idle(true);
    }
    return false;
}

bool ui_screen_is_on() {
    return !s_screen_off;
}

void ui_present(M5Canvas &canvas) {
    if (!s_screen_off) canvas.pushSprite(0, 0);
}

void ui_init_backlight() {
    if (M5.getBoard() != m5::board_t::board_M5StickS3) return;

    // M5GFX configures Stick S3's GPIO38 on LEDC channel 7 / timer 3 at 9 bits.
    // Keep its brightness and panel sleep handling, and use a DFS-independent clock.
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = LEDC_TIMER_3;
    timer.duty_resolution = LEDC_TIMER_9_BIT;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_USE_XTAL_CLK;
    const auto result = ledc_timer_config(&timer);
    ESP_LOGI("display", "Stick S3 backlight PWM: %u Hz (XTAL setup: %s)",
             (unsigned)ledc_get_freq(timer.speed_mode, timer.timer_num), esp_err_to_name(result));
}

void ui_apply_brightness() {
    int pct = g_settings.brightness_pct;
    if (pct < 20) pct = 20;
    if (pct > 100) pct = 100;
    uint8_t val = (uint8_t)((pct * 255) / 100);
    if (val < 36) val = 36;
    M5.Display.setBrightness(val);
}

void ui_draw_battery_badge(M5Canvas &canvas, int right_x, int hdr_h, uint32_t bg_col) {
    static int batt = -1;
    static bool charging = false;
    static int64_t next_sample_us = 0;
    const int64_t now_us = esp_timer_get_time();
    if (now_us >= next_sample_us) {
        batt = M5.Power.getBatteryLevel();
        if (batt > 100) batt = 100;
        charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
        next_sample_us = now_us + 2000000;
    }
    if (batt < 0) return;

    char bstr[16];
    std::snprintf(bstr, sizeof(bstr), "%d%%", batt);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    int txt_w = canvas.textWidth(bstr);
    int txt_x = right_x - txt_w;
    int txt_y = (hdr_h - 14) / 2;

    const auto &theme = ui_theme();
    const bool themed = bg_col == theme.surface || bg_col == theme.alternate;
    const uint32_t txt_col = themed
        ? (charging ? theme.accent : (batt <= 20 ? theme.danger : theme.text))
        : (charging ? 0x80D8FF : (batt <= 20 ? 0xFF8A80 : 0xFFFFFF));
    const uint32_t icon_col = txt_col;

    // Icon placed directly to the left of the battery number
    int icon_x = txt_x - 11;
    int icon_y = (hdr_h - 12) / 2;

    if (charging) {
        // Vertical 9x12 charging plug silhouette
        canvas.fillRect(icon_x + 2, icon_y, 2, 3, icon_col);      // left prong
        canvas.fillRect(icon_x + 5, icon_y, 2, 3, icon_col);      // right prong
        canvas.fillRect(icon_x + 1, icon_y + 3, 7, 5, icon_col);  // plug body
        canvas.fillRect(icon_x + 2, icon_y + 8, 5, 2, icon_col);  // strain relief
        canvas.fillRect(icon_x + 4, icon_y + 10, 1, 2, icon_col); // cord tail
        // Tiny lightning notch in plug center
        canvas.drawPixel(icon_x + 4, icon_y + 4, bg_col);
        canvas.drawPixel(icon_x + 3, icon_y + 5, bg_col);
        canvas.drawPixel(icon_x + 4, icon_y + 5, bg_col);
        canvas.drawPixel(icon_x + 5, icon_y + 5, bg_col);
        canvas.drawPixel(icon_x + 4, icon_y + 6, bg_col);
    } else {
        // Vertical 7x12 battery outline + charge fill
        canvas.fillRect(icon_x + 2, icon_y, 3, 2, icon_col);          // positive cap
        canvas.drawRect(icon_x, icon_y + 2, 7, 10, icon_col);         // vertical body
        canvas.fillRect(icon_x + 1, icon_y + 3, 5, 8, bg_col);        // inner clear
        int fill_h = (batt * 8 + 50) / 100;
        if (fill_h < 1 && batt > 0) fill_h = 1;
        if (fill_h > 8) fill_h = 8;
        if (fill_h > 0) {
            canvas.fillRect(icon_x + 1, icon_y + 3 + (8 - fill_h), 5, fill_h, icon_col);
        }
    }

    canvas.setTextColor(txt_col, bg_col);
    canvas.setCursor(txt_x, txt_y);
    canvas.print(bstr);
}

void ui_draw_header(M5Canvas &canvas, const char *title, uint32_t hdr_col,
                    uint32_t fg_col, const char *badge_txt,
                    uint32_t badge_bg, uint32_t badge_fg, int title_x) {
    int w = canvas.width();
    constexpr int kHdrH = 22;

    canvas.fillRect(0, 0, w, kHdrH, hdr_col);
    const auto &theme = ui_theme();
    canvas.drawFastHLine(0, kHdrH - 1, w,
                         hdr_col == theme.surface || hdr_col == theme.alternate ? theme.border : 0x22324A);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextColor(fg_col, hdr_col);
    const int battery_x = w - 4 - canvas.textWidth("100%") - 11;
    ui_draw_text_fit(canvas, title, title_x, 3, battery_x - title_x - 3);

    if (badge_txt && badge_txt[0] != '\0') {
        int bx = title_x + canvas.textWidth(title) + 4;
        int bw = canvas.textWidth(badge_txt) + 6;
        if (bx + bw < battery_x) {
            canvas.fillRoundRect(bx, 3, bw, 15, 2, badge_bg);
            canvas.setTextColor(badge_fg, badge_bg);
            canvas.setCursor(bx + 3, 3);
            canvas.print(badge_txt);
        }
    }

    ui_draw_battery_badge(canvas, w - 4, kHdrH, hdr_col);
}

void ui_draw_text_fit(M5Canvas &canvas, const char *text, int x, int y, int available) {
    const int pixels = canvas.textWidth(text);
    if (pixels > available && available > 0) {
        canvas.setTextSize(static_cast<float>(available) / pixels, 1.0f);
    }
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(text, x, y);
    canvas.setTextSize(1);
}

void ui_draw_themed_footer(M5Canvas &canvas, const char *line1, const char *line2) {
    const auto &theme = ui_theme();
    int w = canvas.width();
    int h = canvas.height();
    constexpr int kFooterH = 29;
    int top = h - kFooterH;

    canvas.fillRect(0, top, w, kFooterH, theme.background);
    canvas.drawFastHLine(0, top, w, theme.border);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextColor(theme.muted, theme.background);
    ui_draw_text_fit(canvas, line1, 3, top + 1, w - 6);
    ui_draw_text_fit(canvas, line2, 3, top + 14, w - 6);
}

void ui_draw_mic_icon(M5Canvas &canvas, int x, int y, uint32_t color) {
    canvas.fillRoundRect(x + 3, y, 5, 9, 2, color);
    canvas.drawFastVLine(x + 1, y + 4, 5, color);
    canvas.drawFastVLine(x + 9, y + 4, 5, color);
    canvas.drawFastHLine(x + 2, y + 9, 7, color);
    canvas.drawFastVLine(x + 5, y + 9, 4, color);
    canvas.drawFastHLine(x + 3, y + 13, 5, color);
}
