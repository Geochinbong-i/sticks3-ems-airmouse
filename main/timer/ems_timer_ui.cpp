#include "ems_timer_ui.h"
#include "ems_timer_core.h"
#include "app_settings.h"
#include "input_util.h"
#include "sound_fx.h"
#include "ui_common.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static EmsTimerCore s_timer;
static bool s_timer_cfg_applied = false;
static SemaphoreHandle_t s_timer_mutex = nullptr;

class TimerLock {
public:
    TimerLock() {
        if (s_timer_mutex) xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
    }
    ~TimerLock() {
        if (s_timer_mutex) xSemaphoreGive(s_timer_mutex);
    }
    TimerLock(const TimerLock &) = delete;
    TimerLock &operator=(const TimerLock &) = delete;
};

static void ems_timer_bg_task(void *arg) {
    (void)arg;
    while (true) {
        int64_t now_ms = input_now_ms();
        EmsTickResult tres{false, false, false};
        {
            TimerLock lock;
            tres = s_timer.tick(now_ms);
        }
        if (tres.epi_just_due || tres.epi_reminder_beep) {
            sound_fx_epi_due();
        } else if (tres.preset_just_due) {
            sound_fx_preset_due();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void ems_timer_init() {
    if (s_timer_mutex) return;
    s_timer_mutex = xSemaphoreCreateMutex();
    {
        TimerLock lock;
        s_timer.set_epi_interval_sec(g_settings.epi_interval_sec);
        s_timer.set_preset_index(g_settings.preset_idx);
        s_timer_cfg_applied = true;
    }
    xTaskCreate(ems_timer_bg_task, "ems_bg", 2560, nullptr, 3, nullptr);
}

bool ems_timer_is_running() {
    TimerLock lock;
    return s_timer.state() == EmsState::Running;
}

bool ems_timer_has_active_alarm() {
    TimerLock lock;
    return s_timer.epi_alarm_active() || s_timer.preset_alarm_active();
}

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
        ui_draw_header(canvas, "RESET EMS", 0x2B1212, 0xFF8A80);

        canvas.drawRoundRect(6, 32, w - 12, 168, 7, 0xFF5252);
        canvas.fillRoundRect(7, 33, w - 14, 166, 6, 0x1B0D0D);

        canvas.setFont(&fonts::Font2);
        canvas.setTextSize(1);
        canvas.setTextWrap(false);

        canvas.setTextColor(0xFF8A80, 0x1B0D0D);
        canvas.setCursor(14, 42);
        canvas.print("RESET EMS?");

        canvas.setTextColor(0xE2E8F0, 0x1B0D0D);
        canvas.setCursor(14, 68);
        canvas.print("Clears timer and");
        canvas.setCursor(14, 84);
        canvas.print("all event logs.");

        canvas.fillRoundRect(12, 114, w - 24, 30, 4, 0x183834);
        canvas.setTextColor(0x80CBC4, 0x183834);
        canvas.setCursor(18, 121);
        canvas.print("A : Keep Data");

        canvas.fillRoundRect(12, 152, w - 24, 30, 4, 0x6A1616);
        canvas.setTextColor(0xFFCDD2, 0x6A1616);
        canvas.setCursor(18, 159);
        canvas.print("B : Reset All");

        ui_draw_footer2(canvas, "A: Cancel  B: Clear", "Hold A+B : Go Back", 0x140A0A, 0xC09E9E);
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

        size_t total = 0;
        {
            TimerLock lock;
            total = s_timer.event_count();
        }
        if (input_a_pressed() && total > 8) {
            offset = (offset + 4 < total) ? (offset + 4) : 0;
            sound_fx_click();
        }

        int w = canvas.width();
        int h = canvas.height();
        canvas.fillScreen(0x0B0F17);

        char lhdr[24];
        std::snprintf(lhdr, sizeof(lhdr), "LOGS (%u)", (unsigned)total);
        ui_draw_header(canvas, lhdr, 0x1C2838, 0xFFFFFF);

        canvas.setFont(&fonts::Font2);
        canvas.setTextSize(1);
        canvas.setTextWrap(false);

        if (total == 0) {
            canvas.setTextColor(0x8899AA, 0x0B0F17);
            canvas.setCursor(14, 96);
            canvas.print("No events logged");
        } else {
            TimerLock lock;
            total = s_timer.event_count();
            int y = 26;
            for (size_t i = offset; i < total && y < h - 46; i++) {
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
                canvas.setCursor(4, y);
                canvas.printf("%02d:%02d", mm, ss);
                canvas.setTextColor(col, 0x0B0F17);
                canvas.setCursor(48, y);
                canvas.print(ev->label);
                y += 18;
            }
        }

        ui_draw_footer2(canvas, "A: NextPg  B: Back", "Hold A+B : Close");
        canvas.pushSprite(0, 0);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void draw_ems_frame(M5Canvas &canvas, int64_t now_ms, EmsAction sel_action) {
    int w = canvas.width();
    int h = canvas.height();
    bool flash_on = ((now_ms / 450) % 2) == 0;

    EmsState st;
    int shock_cnt;
    int64_t total_ms;
    int epi_int_sec;
    int epi_doses;
    bool epi_act;
    int64_t epi_rem;
    bool epi_overdue;
    bool pre_on;
    bool pre_done;
    int pre_sec;
    int64_t pre_rem;
    size_t ev_cnt;
    EmsEvent recent[2]{};
    {
        TimerLock lock;
        st = s_timer.state();
        shock_cnt = s_timer.shock_count();
        total_ms = s_timer.total_elapsed_ms(now_ms);
        epi_int_sec = s_timer.epi_interval_sec();
        epi_doses = s_timer.epi_dose_count();
        epi_act = s_timer.epi_active();
        epi_rem = s_timer.epi_remaining_ms(now_ms);
        epi_overdue = s_timer.epi_is_overdue(now_ms);
        pre_on = s_timer.preset_enabled();
        pre_done = s_timer.preset_is_done(now_ms);
        pre_sec = s_timer.preset_sec();
        pre_rem = s_timer.preset_remaining_ms(now_ms);
        ev_cnt = s_timer.event_count();
        for (size_t i = 0; i < 2 && i < ev_cnt; i++) {
            recent[i] = *s_timer.event_at(i);
        }
    }

    canvas.fillScreen(0x090D14);

    // 1. Top Bar with vertical battery / plug badge
    uint32_t hdr_col = (st == EmsState::Running) ? 0x143326
                     : (st == EmsState::Paused ? 0x4A360D : 0x1A2433);
    const char *st_title = (st == EmsState::Running) ? "EMS RUN"
                         : (st == EmsState::Paused ? "EMS PAUSE" : "EMS READY");
    ui_draw_header(canvas, st_title, hdr_col, 0xFFFFFF,
                   g_settings.sound_muted ? "M" : nullptr);

    // 2. Total Incident Stopwatch
    char tot_buf[16];
    EmsTimerCore::format_hh_mm_ss(total_ms, tot_buf, sizeof(tot_buf));
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextColor(0x90A4AE, 0x090D14);
    canvas.setCursor(4, 24);
    canvas.print("TOTAL");
    canvas.setCursor(w - 50, 24);
    canvas.printf("SHK:%d", shock_cnt);

    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(2);
    canvas.setTextColor(0xFFFFFF, 0x090D14);
    canvas.setCursor((w - (int)std::strlen(tot_buf) * 12) / 2, 39);
    canvas.print(tot_buf);

    // 3. Epinephrine Interval Card
    uint32_t epi_bg = 0x121B26;
    uint32_t epi_border = 0x2A3D54;
    uint32_t epi_fg = 0x69F0AE;
    if (epi_act) {
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

    canvas.fillRoundRect(4, 58, w - 8, 56, 5, epi_bg);
    canvas.drawRoundRect(4, 58, w - 8, 56, 5, epi_border);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(0xCFD8DC, epi_bg);
    canvas.setCursor(9, 60);
    canvas.printf("EPI %dm", epi_int_sec / 60);

    char dose_str[24];
    std::snprintf(dose_str, sizeof(dose_str), "#%d", epi_doses);
    canvas.setCursor(w - canvas.textWidth(dose_str) - 9, 60);
    canvas.print(dose_str);

    char epi_buf[24];
    if (!epi_act) {
        EmsTimerCore::format_mm_ss((int64_t)epi_int_sec * 1000, epi_buf, sizeof(epi_buf));
    } else if (epi_overdue) {
        char mmss[16];
        EmsTimerCore::format_mm_ss(-epi_rem, mmss, sizeof(mmss));
        std::snprintf(epi_buf, sizeof(epi_buf), "+%s", mmss);
    } else {
        EmsTimerCore::format_mm_ss(epi_rem, epi_buf, sizeof(epi_buf));
    }

    canvas.setFont(&fonts::Font4);
    canvas.setTextSize(1);
    canvas.setTextColor(epi_fg, epi_bg);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString(epi_buf, w / 2, 90);
    canvas.setTextDatum(TL_DATUM);

    int bar_x = 9, bar_y = 107, bar_w = w - 18;
    canvas.fillRect(bar_x, bar_y, bar_w, 4, 0x0B0F17);
    if (epi_act) {
        int64_t tot_epi = (int64_t)epi_int_sec * 1000;
        int fill = epi_overdue ? bar_w : (int)((epi_rem * bar_w) / tot_epi);
        if (fill < 0) fill = 0;
        if (fill > bar_w) fill = bar_w;
        canvas.fillRect(bar_x, bar_y, fill, 4, epi_overdue ? 0xFF5252 : epi_fg);
    }

    // 4. Countdown Preset Card (Vital / Reassess)
    uint32_t pre_bg = (pre_done && flash_on) ? 0x0D47A1 : 0x101822;
    canvas.fillRoundRect(4, 117, w - 8, 28, 5, pre_bg);
    canvas.drawRoundRect(4, 117, w - 8, 28, 5, pre_done ? 0x40C4FF : 0x263547);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(0x90CAF9, pre_bg);
    canvas.setCursor(9, 121);
    if (!pre_on) {
        canvas.print("PRESET: OFF");
    } else {
        canvas.printf("VITAL %dm", pre_sec / 60);
    }

    if (pre_on) {
        char pbuf[16];
        if (pre_done) {
            std::snprintf(pbuf, sizeof(pbuf), "DUE!");
        } else {
            EmsTimerCore::format_mm_ss(pre_rem, pbuf, sizeof(pbuf));
        }
        canvas.setFont(&fonts::Font2);
        canvas.setTextSize(1);
        canvas.setTextColor(pre_done ? 0xFFFFFF : 0x80D8FF, pre_bg);
        canvas.setCursor(w - canvas.textWidth(pbuf) - 9, 121);
        canvas.print(pbuf);

        int64_t p_tot = (int64_t)pre_sec * 1000;
        int p_fill = (int)((pre_rem * (w - 18)) / p_tot);
        canvas.fillRect(9, 139, w - 18, 3, 0x090D14);
        canvas.fillRect(9, 139, p_fill, 3, pre_done ? 0xFFFFFF : 0x40C4FF);
    }

    // 5. Recent Events Mini-Strip (up to 2 newest in Font2)
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    int ey = 148;
    if (ev_cnt == 0) {
        canvas.setTextColor(0x78909C, 0x090D14);
        canvas.setCursor(8, ey + 6);
        canvas.print("Press B to START");
    } else {
        for (size_t i = 0; i < 2 && i < ev_cnt; i++) {
            const EmsEvent &ev = recent[i];
            canvas.setTextColor(0x78909C, 0x090D14);
            canvas.setCursor(6, ey);
            canvas.printf("%02u:%02u", (unsigned)(ev.at_sec / 60), (unsigned)(ev.at_sec % 60));
            canvas.setTextColor(i == 0 ? 0xECEFF1 : 0x90A4AE, 0x090D14);
            canvas.setCursor(48, ey);
            canvas.print(ev.label);
            ey += 15;
        }
    }

    // 6. Action Bar + Enlarged 2-line Footer
    if (st == EmsState::Idle) {
        canvas.fillRoundRect(4, h - 56, w - 8, 22, 4, 0x1B5E20);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(0xFFFFFF, 0x1B5E20);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString("B: START TIMER", w / 2, h - 45);
        canvas.setTextDatum(TL_DATUM);
        ui_draw_footer2(canvas, "A:Preset  HoldB:EPI", "Hold A+B : Go Home");
    } else {
        uint32_t acol = action_color(sel_action);
        const char *aname = action_name(sel_action, st);
        canvas.fillRoundRect(4, h - 56, w - 8, 22, 4, acol);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(0xFFFFFF, acol);
        char act_line[32];
        std::snprintf(act_line, sizeof(act_line), "[ %s ]", aname);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString(act_line, w / 2, h - 45);
        canvas.setTextDatum(TL_DATUM);
        ui_draw_footer2(canvas, "A:Act  B:OK  HB:EPI", "Hold A+B : Go Home");
    }

    canvas.pushSprite(0, 0);
}

void ems_timer_ui_run() {
    ems_timer_init();
    {
        TimerLock lock;
        if (!s_timer_cfg_applied || s_timer.state() == EmsState::Idle) {
            s_timer.set_epi_interval_sec(g_settings.epi_interval_sec);
            s_timer.set_preset_index(g_settings.preset_idx);
            s_timer_cfg_applied = true;
        }
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
            sound_fx_click();
            input_wait_ab_release();
            canvas.deleteSprite();
            return;
        }

        bool a_down = input_a_down();
        bool b_down = input_b_down();

        if (a_down && b_down) {
            a_was_down = false;
            b_was_down = false;
            a_long_fired = false;
            b_long_fired = false;
        } else {
            // Button A handling
            if (a_down && !a_was_down) {
                a_down_ms = now_ms;
                a_long_fired = false;
            } else if (a_down && a_was_down && !a_long_fired && (now_ms - a_down_ms) >= 650) {
                a_long_fired = true;
                bool had_alarm = false;
                {
                    TimerLock lock;
                    had_alarm = s_timer.epi_alarm_active() || s_timer.preset_alarm_active();
                    if (had_alarm) {
                        s_timer.acknowledge_alarms();
                    }
                }
                if (had_alarm) {
                    sound_fx_stop();
                } else {
                    g_settings.sound_muted = !g_settings.sound_muted;
                    sound_fx_apply_volume();
                    app_settings_save();
                }
                sound_fx_click();
            } else if (!a_down && a_was_down && !a_long_fired) {
                bool is_idle = false;
                {
                    TimerLock lock;
                    is_idle = (s_timer.state() == EmsState::Idle);
                    if (is_idle) {
                        s_timer.cycle_preset();
                        g_settings.preset_idx = s_timer.preset_index();
                    }
                }
                if (is_idle) {
                    app_settings_save();
                } else {
                    sel_action = (EmsAction)(((int)sel_action + 1) % (int)EmsAction::Count);
                }
                sound_fx_click();
            }
            a_was_down = a_down;

            // Button B handling
            if (b_down && !b_was_down) {
                b_down_ms = now_ms;
                b_long_fired = false;
            } else if (b_down && b_was_down && !b_long_fired && (now_ms - b_down_ms) >= 650) {
                b_long_fired = true;
                bool is_idle = false;
                {
                    TimerLock lock;
                    is_idle = (s_timer.state() == EmsState::Idle);
                    if (is_idle) {
                        s_timer.cycle_epi_interval();
                        g_settings.epi_interval_sec = s_timer.epi_interval_sec();
                    } else {
                        s_timer.log_epi(now_ms);
                    }
                }
                if (is_idle) {
                    app_settings_save();
                    sound_fx_confirm();
                } else {
                    sel_action = EmsAction::Epi;
                    sound_fx_epi_logged();
                }
            } else if (!b_down && b_was_down && !b_long_fired) {
                EmsState cur_st;
                {
                    TimerLock lock;
                    cur_st = s_timer.state();
                    if (cur_st == EmsState::Idle) {
                        s_timer.start(now_ms);
                    }
                }
                if (cur_st == EmsState::Idle) {
                    sel_action = EmsAction::Epi;
                    sound_fx_confirm();
                } else {
                    switch (sel_action) {
                        case EmsAction::Epi:
                            {
                                TimerLock lock;
                                s_timer.log_epi(now_ms);
                            }
                            sound_fx_epi_logged();
                            break;
                        case EmsAction::Shock:
                            {
                                TimerLock lock;
                                s_timer.log_event(EmsEventType::Shock, now_ms);
                            }
                            sound_fx_confirm();
                            sel_action = EmsAction::Epi;
                            break;
                        case EmsAction::Rosc:
                            {
                                TimerLock lock;
                                s_timer.log_event(EmsEventType::Rosc, now_ms);
                            }
                            sound_fx_confirm();
                            sel_action = EmsAction::Epi;
                            break;
                        case EmsAction::Vital:
                            {
                                TimerLock lock;
                                s_timer.log_event(EmsEventType::Vital, now_ms);
                            }
                            sound_fx_confirm();
                            sel_action = EmsAction::Epi;
                            break;
                        case EmsAction::Airway:
                            {
                                TimerLock lock;
                                s_timer.log_event(EmsEventType::Airway, now_ms);
                            }
                            sound_fx_confirm();
                            sel_action = EmsAction::Epi;
                            break;
                        case EmsAction::PauseResume:
                            {
                                TimerLock lock;
                                s_timer.toggle_pause(now_ms);
                            }
                            sound_fx_confirm();
                            break;
                        case EmsAction::Logs:
                            show_logs_modal(canvas);
                            break;
                        case EmsAction::Reset:
                            if (confirm_reset_modal(canvas)) {
                                TimerLock lock;
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

        if ((now_ms - last_draw_ms) >= 100) {
            last_draw_ms = now_ms;
            draw_ems_frame(canvas, now_ms, sel_action);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
