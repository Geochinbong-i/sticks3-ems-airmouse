#include "ui_common.h"
#include "app_settings.h"

#include <cstdio>
#include <cstring>

void ui_apply_brightness() {
    int pct = g_settings.brightness_pct;
    if (pct < 20) pct = 20;
    if (pct > 100) pct = 100;
    uint8_t val = (uint8_t)((pct * 255) / 100);
    if (val < 36) val = 36;
    M5.Display.setBrightness(val);
}

void ui_draw_battery_badge(M5Canvas &canvas, int right_x, int hdr_h, uint32_t bg_col) {
    int batt = M5.Power.getBatteryLevel();
    if (batt < 0) return;
    if (batt > 100) batt = 100;

    bool charging = (M5.Power.isCharging() == m5::Power_Class::is_charging);

    char bstr[8];
    std::snprintf(bstr, sizeof(bstr), "%d%%", batt);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    int txt_w = canvas.textWidth(bstr);
    int txt_x = right_x - txt_w;
    int txt_y = (hdr_h - 14) / 2;

    uint32_t icon_col = charging ? 0x40C4FF : (batt <= 20 ? 0xFF5252 : 0x69F0AE);
    uint32_t txt_col = charging ? 0x80D8FF : (batt <= 20 ? 0xFF8A80 : 0xFFFFFF);

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
                    uint32_t badge_bg, uint32_t badge_fg) {
    int w = canvas.width();
    constexpr int kHdrH = 22;

    canvas.fillRect(0, 0, w, kHdrH, hdr_col);
    canvas.drawFastHLine(0, kHdrH - 1, w, 0x22324A);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextColor(fg_col, hdr_col);
    canvas.setCursor(4, 3);
    canvas.print(title);

    if (badge_txt && badge_txt[0] != '\0') {
        int bx = 4 + canvas.textWidth(title) + 4;
        int bw = canvas.textWidth(badge_txt) + 6;
        if (bx + bw < w - 42) {
            canvas.fillRoundRect(bx, 3, bw, 15, 2, badge_bg);
            canvas.setTextColor(badge_fg, badge_bg);
            canvas.setCursor(bx + 3, 3);
            canvas.print(badge_txt);
        }
    }

    ui_draw_battery_badge(canvas, w - 4, kHdrH, hdr_col);
}

void ui_draw_footer2(M5Canvas &canvas, const char *line1, const char *line2,
                     uint32_t bg_col, uint32_t fg_col) {
    int w = canvas.width();
    int h = canvas.height();
    constexpr int kFooterH = 29;
    int top = h - kFooterH;

    canvas.fillRect(0, top, w, kFooterH, bg_col);
    canvas.drawFastHLine(0, top, w, 0x1E2B3D);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextColor(fg_col, bg_col);
    canvas.setCursor(3, top + 1);
    canvas.print(line1);
    canvas.setCursor(3, top + 14);
    canvas.print(line2);
}
