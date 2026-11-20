#include "ems_timer_ui.h"
#include "ems_timer_core.h"
#include "app_settings.h"
#include "input_util.h"
#include "sound_fx.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static EmsTimerCore s_timer;
static bool s_timer_cfg_applied = false;

enum class EmsAction : int {
    Epi = 0,
    Shock = 1,
    Rosc = 2,
    Vital = 3,
    Airway = 4,
    PauseResume = 5,
    Logs = 6,
    Reset = 7,
    Count = 8,
};

static const char *action_name(EmsAction act, EmsState st) {
    switch (act) {
        case EmsAction::Epi: return "LOG EPI";
        case EmsAction::Shock: return "LOG SHOCK";
        case EmsAction::Rosc: return "LOG ROSC";
        case EmsAction::Vital: return "LOG VITAL";
        case EmsAction::Airway: return "AIRWAY";
        case EmsAction::PauseResume: return (st == EmsState::Paused) ? "RESUME" : "PAUSE";
        case EmsAction::Logs: return "VIEW LOGS";
        case EmsAction::Reset: return "RESET";
        default: return "";
    }
}

static uint32_t action_color(EmsAction act) {
    switch (act) {
        case EmsAction::Epi: return 0xC62828;
        case EmsAction::Shock: return 0xE65100;
        case EmsAction::Rosc: return 0x2E7D32;
        case EmsAction::Vital: return 0x0277BD;
        case EmsAction::Airway: return 0x4527A0;
        case EmsAction::PauseResume: return 0x546E7A;
        case EmsAction::Logs: return 0x37474F;
        case EmsAction::Reset: return 0x6D1B1B;
        default: return 0x333333;
    }
}

static bool confirm_reset_modal(M5Canvas &canvas) {
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

        int w = canvas.width();
        canvas.fillScreen(0x140A0A);
        canvas.drawRoundRect(8, 56, w - 16, 120, 8, 0xFF5252);
        canvas.setTextSize(1);
        canvas.setTextColor(0xFF8A80, 0x140A0A);
        canvas.setCursor(20, 72);
        canvas.print("RESET INCIDENT?");
        canvas.setTextColor(0xDDDDDD, 0x140A0A);
        canvas.setCursor(16, 94);
        canvas.print("Clears timer & logs");
        canvas.setTextColor(0x80CBC4, 0x140A0A);
        canvas.setCursor(22, 128);
        canvas.print("A : CANCEL");
        canvas.setTextColor(0xFF5252, 0x140A0A);
        canvas.setCursor(22, 146);
        canvas.print("B : CONFIRM RESET");
        canvas.pushSprite(0, 0);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void show_logs_modal(M5Canvas &canvas) {
    input_wait_all_release();
    size_t offset = 0;
    while (true) {
        input_update();
        if (input_ab_held(400) || input_b_pressed()) {
            sound_fx_click();
            input_wait_all_release();
            return;
        }
        size_t total = s_timer.event_count();
        if (input_a_pressed() && total > 8) {
            offset = (offset + 4 < total) ? (offset + 4) : 0;
            sound_fx_click();
        }

        int w = canvas.width();
        int h = canvas.height();
        canvas.fillScreen(0x0B0F17);
        canvas.fillRect(0, 0, w, 22, 0x1C2838);
        canvas.setTextSize(1);
        canvas.setTextColor(0xFFFFFF, 0x1C2838);
        canvas.setCursor(6, 7);
        canvas.printf("EVENT LOG (%u)", (unsigned)total);

        if (total == 0) {
            canvas.setTextColor(0x8899AA, 0x0B0F17);
            canvas.setCursor(18, 90);
            canvas.print("No events logged");
        } else {
            int y = 30;
            for (size_t i = offset; i < total && y < h - 24; i++) {
                const EmsEvent *ev = s_timer.event_at(i);
                if (!ev) break;
                int mm = (int)(ev->at_sec / 60);
                int ss = (int)(ev->at_sec % 60);
                uint32_t col = 0xE0E6ED;
                if (ev->type == EmsEventType::Epi) col = 0xFF8A80;
                else if (ev->type == EmsEventType::Shock) col = 0xFFD180;
                else if (ev->type == EmsEventType::Rosc) col = 0x69F0AE;
                else if (ev->type == EmsEventType::Vital) col = 0x80D8FF;

                canvas.setTextColor(0x78909C, 0x0B0F17);
                canvas.setCursor(6, y);
                canvas.printf("%02d:%02d", mm, ss);
                canvas.setTextColor(col, 0x0B0F17);
                canvas.setCursor(44, y);
                canvas.print(ev->label);
                y += 18;
            }
        }

        canvas.setTextColor(0x8899AA, 0x0B0F17);
        canvas.setCursor(6, h - 14);
        canvas.print("A:Page  B:Close");
        canvas.pushSprite(0, 0);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void draw_ems_frame(M5Canvas &canvas, int64_t now_ms, EmsAction sel_action) {
    int w = canvas.width();
    int h = canvas.height();
    EmsState st = s_timer.state();
    bool flash_on = ((now_ms / 450) % 2) == 0;

    canvas.fillScreen(0x090D14);

    // 1. Top Bar
    uint32_t hdr_col = (st == EmsState::Running) ? 0x143326
                     : (st == EmsState::Paused ? 0x4A360D : 0x1A2433);
    canvas.fillRect(0, 0, w, 20, hdr_col);
    canvas.setTextSize(1);
    canvas.setTextColor(0xFFFFFF, hdr_col);
    canvas.setCursor(5, 6);
    canvas.print(st == EmsState::Running ? "EMS RUN" : (st == EmsState::Paused ? "EMS PAUSE" : "EMS READY"));

    if (g_settings.sound_muted) {
        canvas.setTextColor(0xFFAB40, hdr_col);
        canvas.setCursor(66, 6);
        canvas.print("MUTE");
    }
    int batt = M5.Power.getBatteryLevel();
    if (batt >= 0) {
        if (batt > 100) batt = 100;
        char bstr[16];
        std::snprintf(bstr, sizeof(bstr), "%d%%", batt);
        canvas.setTextColor(0xCFD8DC, hdr_col);
        canvas.setCursor(w - (int)std::strlen(bstr) * 6 - 4, 6);
        canvas.print(bstr);
    }

    // 2. Total Incident Stopwatch
    char tot_buf[16];
    EmsTimerCore::format_hh_mm_ss(s_timer.total_elapsed_ms(now_ms), tot_buf, sizeof(tot_buf));
    canvas.setTextColor(0x8FA3B8, 0x090D14);
    canvas.setCursor(6, 24);
    canvas.print("TOTAL");
    canvas.setCursor(64, 24);
    canvas.printf("SHK:%d", s_timer.shock_count());

    canvas.setTextSize(2);
    canvas.setTextColor(0xFFFFFF, 0x090D14);
    canvas.setCursor((w - (int)std::strlen(tot_buf) * 12) / 2, 35);
    canvas.print(tot_buf);

    // 3. Epinephrine Interval Card
    int64_t epi_rem = s_timer.epi_remaining_ms(now_ms);
    bool epi_overdue = s_timer.epi_is_overdue(now_ms);
    uint32_t epi_bg = 0x121B26;
    uint32_t epi_border = 0x2A3D54;
    uint32_t epi_fg = 0x69F0AE;
    if (s_timer.epi_active()) {
        if (epi_overdue) {
            epi_bg = flash_on ? 0x9E1414 : 0x3D0B0B;
            epi_border = 0xFF5252;
            epi_fg = 0xFFFFFF;
        } else if (epi_rem <= 30000) {
            epi_bg = 0x2E2208;
            epi_border = 0xFFB300;
            epi_fg = 0xFFD54F;
        }
    } else {
        epi_fg = 0x80CBC4;
    }

    canvas.fillRoundRect(4, 57, w - 8, 60, 6, epi_bg);
    canvas.drawRoundRect(4, 57, w - 8, 60, 6, epi_border);

    canvas.setTextSize(1);
    canvas.setTextColor(0xCFD8DC, epi_bg);
    canvas.setCursor(10, 62);
    canvas.printf("EPI %dm", s_timer.epi_interval_sec() / 60);

    char dose_str[24];
    std::snprintf(dose_str, sizeof(dose_str), "DOSE #%d", s_timer.epi_dose_count());
    canvas.setCursor(w - (int)std::strlen(dose_str) * 6 - 10, 62);
    canvas.print(dose_str);

    char epi_buf[24];
    if (!s_timer.epi_active()) {
        EmsTimerCore::format_mm_ss((int64_t)s_timer.epi_interval_sec() * 1000, epi_buf, sizeof(epi_buf));
    } else if (epi_overdue) {
        char mmss[16];
        EmsTimerCore::format_mm_ss(-epi_rem, mmss, sizeof(mmss));
        std::snprintf(epi_buf, sizeof(epi_buf), "+%s", mmss);
    } else {
        EmsTimerCore::format_mm_ss(epi_rem, epi_buf, sizeof(epi_buf));
    }

    canvas.setTextSize(3);
    canvas.setTextColor(epi_fg, epi_bg);
    canvas.setCursor((w - (int)std::strlen(epi_buf) * 18) / 2, 76);
    canvas.print(epi_buf);

    // EPI Progress bar
    int bar_x = 10, bar_y = 107, bar_w = w - 20;
    canvas.fillRect(bar_x, bar_y, bar_w, 5, 0x0B0F17);
    if (s_timer.epi_active()) {
        int64_t tot_epi = (int64_t)s_timer.epi_interval_sec() * 1000;
        int fill = epi_overdue ? bar_w : (int)((epi_rem * bar_w) / tot_epi);
        if (fill < 0) fill = 0;
        if (fill > bar_w) fill = bar_w;
        canvas.fillRect(bar_x, bar_y, fill, 5, epi_overdue ? 0xFF5252 : epi_fg);
    }

    // 4. Countdown Preset Card (Vital / Reassess)
    bool pre_on = s_timer.preset_enabled();
    bool pre_done = s_timer.preset_is_done(now_ms);
    uint32_t pre_bg = (pre_done && flash_on) ? 0x0D47A1 : 0x101822;
    canvas.fillRoundRect(4, 121, w - 8, 32, 5, pre_bg);
    canvas.drawRoundRect(4, 121, w - 8, 32, 5, pre_done ? 0x40C4FF : 0x263547);

    canvas.setTextSize(1);
    canvas.setTextColor(0x90CAF9, pre_bg);
    canvas.setCursor(10, 127);
    if (!pre_on) {
        canvas.print("PRESET: OFF");
    } else {
        canvas.printf("VITAL %dm", s_timer.preset_sec() / 60);
    }

    if (pre_on) {
        char pbuf[16];
        if (pre_done) {
            std::snprintf(pbuf, sizeof(pbuf), "DUE!");
        } else {
            EmsTimerCore::format_mm_ss(s_timer.preset_remaining_ms(now_ms), pbuf, sizeof(pbuf));
        }
        canvas.setTextSize(2);
        canvas.setTextColor(pre_done ? 0xFFFFFF : 0x80D8FF, pre_bg);
        canvas.setCursor(w - (int)std::strlen(pbuf) * 12 - 10, 126);
        canvas.print(pbuf);

        int64_t p_tot = (int64_t)s_timer.preset_sec() * 1000;
        int64_t p_rem = s_timer.preset_remaining_ms(now_ms);
        int p_fill = (int)((p_rem * (w - 20)) / p_tot);
        canvas.fillRect(10, 146, w - 20, 3, 0x090D14);
        canvas.fillRect(10, 146, p_fill, 3, pre_done ? 0xFFFFFF : 0x40C4FF);
    }

    // 5. Recent Events Mini-Strip (up to 3 newest)
    canvas.setTextSize(1);
    int ey = 157;
    if (s_timer.event_count() == 0) {
        canvas.setTextColor(0x607D8B, 0x090D14);
        canvas.setCursor(8, ey + 6);
        canvas.print("Press B to START");
    } else {
        for (size_t i = 0; i < 3 && i < s_timer.event_count(); i++) {
            const EmsEvent *ev = s_timer.event_at(i);
            canvas.setTextColor(0x6E8296, 0x090D14);
            canvas.setCursor(6, ey);
            canvas.printf("%02u:%02u", (unsigned)(ev->at_sec / 60), (unsigned)(ev->at_sec % 60));
            canvas.setTextColor(i == 0 ? 0xECEFF1 : 0x90A4AE, 0x090D14);
            canvas.setCursor(42, ey);
            canvas.print(ev->label);
            ey += 11;
        }
    }

    // 6. Action / Controls Footer
    if (st == EmsState::Idle) {
        canvas.fillRoundRect(4, h - 44, w - 8, 22, 4, 0x1B5E20);
        canvas.setTextColor(0xFFFFFF, 0x1B5E20);
        canvas.setCursor(22, h - 37);
        canvas.print("B: START TIMER");
        canvas.setTextColor(0x78909C, 0x090D14);
        canvas.setCursor(4, h - 18);
        canvas.print("A:Preset HoldB:EPI A+B:Ex");
    } else {
        uint32_t acol = action_color(sel_action);
        const char *aname = action_name(sel_action, st);
        canvas.fillRoundRect(4, h - 44, w - 8, 22, 4, acol);
        canvas.setTextColor(0xFFFFFF, acol);
        char act_line[32];
        std::snprintf(act_line, sizeof(act_line), "[ %s ]", aname);
        canvas.setCursor((w - (int)std::strlen(act_line) * 6) / 2, h - 37);
        canvas.print(act_line);

        canvas.setTextColor(0x78909C, 0x090D14);
        canvas.setCursor(4, h - 18);
        canvas.print("A:Act B:OK HB:EPI A+B:Ex");
    }

    canvas.pushSprite(0, 0);
}

void ems_timer_ui_run() {
    if (!s_timer_cfg_applied || s_timer.state() == EmsState::Idle) {
        s_timer.set_epi_interval_sec(g_settings.epi_interval_sec);
        s_timer.set_preset_index(g_settings.preset_idx);
        s_timer_cfg_applied = true;
    }

    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    input_wait_all_release();

    EmsAction sel_action = EmsAction::Epi;
    bool a_was_down = false;
    bool a_long_fired = false;
    int64_t a_down_ms = 0;

    bool b_was_down = false;
    bool b_long_fired = false;
    int64_t b_down_ms = 0;

    int64_t last_draw_ms = 0;

    while (true) {
        input_update();
        int64_t now_ms = input_now_ms();

        if (input_ab_held(600)) {
            sound_fx_stop();
            sound_fx_click();
            input_wait_ab_release();
            canvas.deleteSprite();
            return;
        }

        bool a_down = input_a_down();
        bool b_down = input_b_down();

        if (a_down && b_down) {
            // Reset single-button tracking during A+B hold
            a_was_down = false;
            b_was_down = false;
            a_long_fired = false;
            b_long_fired = false;
        } else {
            // Button A handling (tap: next action / cycle preset, hold 650ms: ack alarm / mute)
            if (a_down && !a_was_down) {
                a_down_ms = now_ms;
                a_long_fired = false;
            } else if (a_down && a_was_down && !a_long_fired && (now_ms - a_down_ms) >= 650) {
                a_long_fired = true;
                if (s_timer.epi_alarm_active() || s_timer.preset_alarm_active()) {
                    s_timer.acknowledge_alarms();
                    sound_fx_stop();
                } else {
                    g_settings.sound_muted = !g_settings.sound_muted;
                    sound_fx_apply_volume();
                    app_settings_save();
                }
                sound_fx_click();
            } else if (!a_down && a_was_down && !a_long_fired) {
                if (s_timer.state() == EmsState::Idle) {
                    s_timer.cycle_preset();
                    g_settings.preset_idx = s_timer.preset_index();
                    app_settings_save();
                } else {
                    sel_action = (EmsAction)(((int)sel_action + 1) % (int)EmsAction::Count);
                }
                sound_fx_click();
            }
            a_was_down = a_down;

            // Button B handling (tap: start / exec action, hold 650ms: cycle EPI 3/4/5m if Idle, or instant LOG EPI if active)
            if (b_down && !b_was_down) {
                b_down_ms = now_ms;
                b_long_fired = false;
            } else if (b_down && b_was_down && !b_long_fired && (now_ms - b_down_ms) >= 650) {
                b_long_fired = true;
                if (s_timer.state() == EmsState::Idle) {
                    s_timer.cycle_epi_interval();
                    g_settings.epi_interval_sec = s_timer.epi_interval_sec();
                    app_settings_save();
                    sound_fx_confirm();
                } else {
                    s_timer.log_epi(now_ms);
                    sel_action = EmsAction::Epi;
                    sound_fx_epi_logged();
                }
            } else if (!b_down && b_was_down && !b_long_fired) {
                if (s_timer.state() == EmsState::Idle) {
                    s_timer.start(now_ms);
                    sel_action = EmsAction::Epi;
                    sound_fx_confirm();
                } else {
                    switch (sel_action) {
                        case EmsAction::Epi:
                            s_timer.log_epi(now_ms);
                            sound_fx_epi_logged();
                            break;
                        case EmsAction::Shock:
                            s_timer.log_event(EmsEventType::Shock, now_ms);
                            sound_fx_confirm();
                            sel_action = EmsAction::Epi;
                            break;
                        case EmsAction::Rosc:
                            s_timer.log_event(EmsEventType::Rosc, now_ms);
                            sound_fx_confirm();
                            sel_action = EmsAction::Epi;
                            break;
                        case EmsAction::Vital:
                            s_timer.log_event(EmsEventType::Vital, now_ms);
                            sound_fx_confirm();
                            sel_action = EmsAction::Epi;
                            break;
                        case EmsAction::Airway:
                            s_timer.log_event(EmsEventType::Airway, now_ms);
                            sound_fx_confirm();
                            sel_action = EmsAction::Epi;
                            break;
                        case EmsAction::PauseResume:
                            s_timer.toggle_pause(now_ms);
                            sound_fx_confirm();
                            break;
                        case EmsAction::Logs:
                            show_logs_modal(canvas);
                            break;
                        case EmsAction::Reset:
                            if (confirm_reset_modal(canvas)) {
                                s_timer.reset();
                                sel_action = EmsAction::Epi;
                            }
                            break;
                        default:
                            break;
                    }
                }
            }
            b_was_down = b_down;
        }

        EmsTickResult tres = s_timer.tick(now_ms);
        if (tres.epi_just_due || tres.epi_reminder_beep) {
            sound_fx_epi_due();
        } else if (tres.preset_just_due) {
            sound_fx_preset_due();
        }

        if ((now_ms - last_draw_ms) >= 100) {
            last_draw_ms = now_ms;
            draw_ems_frame(canvas, now_ms, sel_action);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
