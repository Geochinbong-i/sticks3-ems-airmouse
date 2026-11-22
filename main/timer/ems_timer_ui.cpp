#include "ems_timer_ui.h"
#include "ems_timer_core.h"
#include "app_settings.h"
#include "input_util.h"
#include "sound_fx.h"
#include "time_sync.h"
#include "ui_common.h"
#include "ui_theme.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>

static EmsTimerCore s_timer;
static std::atomic<unsigned> s_alarm_sequence{0};
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

static int64_t system_utc_ms() {
    struct timeval tv{};
    if (!time_sync_is_valid() || gettimeofday(&tv, nullptr) != 0) return 0;
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void ems_timer_sync_clock() {
    const int64_t now_ms = input_now_ms();
    const int64_t utc_ms = system_utc_ms();
    TimerLock lock;
    s_timer.sync_clock(now_ms, utc_ms);
}

static void ems_timer_bg_task(void *arg) {
    (void)arg;
    while (true) {
        int64_t now_ms = input_now_ms();
        EmsTickResult tres{false, false, false};
        {
            TimerLock lock;
            tres = s_timer.tick(now_ms);
        }
        if (tres.epi_just_due || tres.preset_just_due) {
            s_alarm_sequence.fetch_add(1, std::memory_order_relaxed);
        }
        sound_fx_alarms(tres.epi_just_due || tres.epi_reminder_beep, tres.preset_just_due);
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

unsigned ems_timer_alarm_sequence() {
    return s_alarm_sequence.load(std::memory_order_relaxed);
}

EmsAlarmState ems_timer_active_alarms() {
    TimerLock lock;
    return {s_timer.epi_alarm_active(), s_timer.preset_alarm_active()};
}

enum class EmsAction : int {
    Epi, Shock, Rosc, Vital, Airway, Iv, PauseResume, Logs, Reset, Count,
};

static const char *action_name(EmsAction act, EmsState st) {
    switch (act) {
        case EmsAction::Epi: return "LOG EPI";
        case EmsAction::Shock: return "LOG SHOCK";
        case EmsAction::Rosc: return "LOG ROSC";
        case EmsAction::Vital: return "LOG VITAL";
        case EmsAction::Airway: return "LOG AIRWAY";
        case EmsAction::Iv: return "LOG IV";
        case EmsAction::PauseResume: return (st == EmsState::Paused) ? "RESUME" : "PAUSE";
        case EmsAction::Logs: return "VIEW LOGS";
        case EmsAction::Reset: return "RESET";
        default: return "";
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

        if (!ui_screen_is_on()) {
            input_idle_delay(25);
            continue;
        }

        const auto &theme = ui_theme();
        int w = canvas.width();
        canvas.fillScreen(theme.background);
        ui_draw_header(canvas, "RESET EMS", theme.surface, theme.danger);

        canvas.drawRoundRect(6, 32, w - 12, 168, 7, theme.danger);
        canvas.fillRoundRect(7, 33, w - 14, 166, 6, theme.surface);

        canvas.setFont(&fonts::Font2);
        canvas.setTextSize(1);
        canvas.setTextWrap(false);

        canvas.setTextColor(theme.danger, theme.surface);
        canvas.setCursor(14, 42);
        canvas.print("RESET EMS?");

        canvas.setTextColor(theme.text, theme.surface);
        canvas.setCursor(14, 68);
        canvas.print("Clears timer and");
        canvas.setCursor(14, 84);
        canvas.print("all event logs.");

        canvas.fillRoundRect(12, 114, w - 24, 30, 4, theme.alternate);
        canvas.setTextColor(theme.text, theme.alternate);
        canvas.setCursor(18, 121);
        canvas.print("A : Keep Data");

        canvas.fillRoundRect(12, 152, w - 24, 30, 4, theme.alternate);
        canvas.setTextColor(theme.danger, theme.alternate);
        canvas.setCursor(18, 159);
        canvas.print("B : Reset All");

        ui_draw_themed_footer(canvas, "A: Cancel  B: Clear", "Hold A+B : Go Back");
        ui_present(canvas);
        input_idle_delay(30);
    }
}

static constexpr size_t kLogRows = 6;

static void format_elapsed(uint32_t seconds, char *buf, size_t len) {
    if (seconds < 3600) {
        std::snprintf(buf, len, "+%02u:%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60));
    } else {
        std::snprintf(buf, len, "+%u:%02u:%02u", (unsigned)(seconds / 3600),
                      (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60));
    }
}

static void draw_logs(M5Canvas &canvas, size_t page) {
    EmsEvent events[kLogRows]{};
    size_t total;
    int64_t start_utc_ms;
    const size_t offset = page * kLogRows;
    {
        TimerLock lock;
        start_utc_ms = s_timer.case_start_utc_ms();
        total = s_timer.event_count();
        for (size_t i = 0; i < kLogRows && offset + i < total; ++i) {
            events[i] = *s_timer.event_at(offset + i);
        }
    }

    const auto &theme = ui_theme();
    const int w = canvas.width();
    const size_t pages = std::max<size_t>(1, (total + kLogRows - 1) / kLogRows);
    canvas.fillScreen(theme.background);
    char title[24];
    std::snprintf(title, sizeof(title), "LOGS %u/%u", (unsigned)(page + 1), (unsigned)pages);
    ui_draw_header(canvas, title, theme.surface, theme.text);

    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(1);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(theme.muted, theme.background);
    char started[24] = "Clock not set";
    if (start_utc_ms > 0) {
        const time_t time = (time_t)(start_utc_ms / 1000);
        struct tm local{};
        localtime_r(&time, &local);
        std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", &local);
    }
    if (total > 0) canvas.drawString(started, 5, 28);

    if (total == 0) {
        canvas.setFont(&fonts::Font2);
        canvas.drawString("No events logged", 12, 96);
    } else {
        for (size_t i = 0; i < kLogRows && offset + i < total; ++i) {
            const auto &event = events[i];
            const int y = 42 + i * 27;
            const uint32_t bg = i % 2 == 0 ? theme.surface : theme.alternate;
            canvas.fillRect(3, y, w - 6, 26, bg);

            const uint32_t fg = event.type == EmsEventType::Shock ? theme.warning
                              : event.type == EmsEventType::Epi ? theme.danger : theme.text;
            canvas.setFont(&fonts::Font2);
            canvas.setTextColor(fg, bg);
            canvas.drawString(event.label, 6, y + 1);

            char elapsed[20];
            format_elapsed(event.elapsed_ms / 1000, elapsed, sizeof(elapsed));
            char when[16]{};
            if (start_utc_ms > 0) {
                const time_t time = (time_t)((start_utc_ms + event.elapsed_ms) / 1000);
                struct tm local{};
                localtime_r(&time, &local);
                std::strftime(when, sizeof(when), "%m/%d %H:%M:%S", &local);
            }
            canvas.setFont(&fonts::Font0);
            canvas.setTextColor(theme.muted, bg);
            const int when_width = canvas.textWidth(when);
            // For long cases, show the absolute time without repeating the date in each row.
            const char *wall_time = when_width + canvas.textWidth(elapsed) + 4 > w - 10 ? when + 6 : when;
            canvas.drawString(elapsed, 6, y + 17);
            canvas.drawRightString(wall_time, w - 5, y + 17);
        }
    }

    ui_draw_themed_footer(canvas, pages > 1 ? "A Next   B Back" : "B: Back",
                          start_utc_ms > 0 ? "Elapsed / Local time" : "+ Time since start");
    ui_present(canvas);
}

static void show_logs_modal(M5Canvas &canvas) {
    input_wait_all_release();
    size_t page = 0;
    bool redraw = true;
    int64_t last_start_utc_ms = 0;
    while (true) {
        input_update();
        if (input_ab_held(400) || input_b_pressed()) {
            sound_fx_click();
            input_wait_all_release();
            return;
        }
        const bool next_page = input_a_pressed();
        size_t pages = 0;
        {
            TimerLock lock;
            if (next_page) pages = (s_timer.event_count() + kLogRows - 1) / kLogRows;
            if (last_start_utc_ms != s_timer.case_start_utc_ms()) {
                last_start_utc_ms = s_timer.case_start_utc_ms();
                redraw = true;
            }
        }
        if (next_page) {
            if (pages > 1) {
                page = (page + 1) % pages;
                sound_fx_click();
            }
            redraw = true;
        }
        if (!ui_screen_is_on()) redraw = true;
        if (redraw && ui_screen_is_on()) {
            draw_logs(canvas, page);
            redraw = false;
        }
        input_idle_delay(25);
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

    const auto &theme = ui_theme();
    canvas.fillScreen(theme.background);

    // 1. Top Bar with vertical battery / plug badge
    const char *st_title = (st == EmsState::Running) ? "EMS RUN"
                         : (st == EmsState::Paused ? "EMS PAUSE" : "EMS READY");
    ui_draw_header(canvas, st_title, theme.surface,
                   st == EmsState::Paused ? theme.warning : theme.text,
                   g_settings.sound_muted ? "M" : nullptr);

    // 2. Total Incident Stopwatch
    char tot_buf[16];
    EmsTimerCore::format_hh_mm_ss(total_ms, tot_buf, sizeof(tot_buf));
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextColor(theme.muted, theme.background);
    canvas.setCursor(4, 24);
    canvas.print("TIMER");
    canvas.setCursor(w - 50, 24);
    canvas.printf("SHK:%d", shock_cnt);

    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(2);
    canvas.setTextColor(theme.text, theme.background);
    canvas.setCursor((w - (int)std::strlen(tot_buf) * 12) / 2, 39);
    canvas.print(tot_buf);

    // 3. Epinephrine Interval Card
    uint32_t epi_bg = theme.surface;
    uint32_t epi_border = theme.border;
    uint32_t epi_fg = theme.text;
    if (epi_act) {
        if (epi_overdue) {
            epi_bg = flash_on ? 0x9E1414 : theme.surface;
            epi_border = flash_on ? 0xFFFFFF : theme.danger;
            epi_fg = flash_on ? 0xFFFFFF : theme.danger;
        } else if (epi_rem <= 30000) {
            epi_border = theme.warning;
            epi_fg = theme.warning;
        }
    } else {
        epi_fg = theme.accent;
    }

    canvas.fillRoundRect(4, 58, w - 8, 56, 5, epi_bg);
    canvas.drawRoundRect(4, 58, w - 8, 56, 5, epi_border);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(epi_overdue && flash_on ? 0xFFFFFF : theme.muted, epi_bg);
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
    canvas.fillRect(bar_x, bar_y, bar_w, 4, theme.alternate);
    if (epi_act) {
        int64_t tot_epi = (int64_t)epi_int_sec * 1000;
        int fill = epi_overdue ? bar_w : (int)((epi_rem * bar_w) / tot_epi);
        if (fill < 0) fill = 0;
        if (fill > bar_w) fill = bar_w;
        canvas.fillRect(bar_x, bar_y, fill, 4, epi_fg);
    }

    // 4. Countdown Preset Card (Vital / Reassess)
    const bool pre_highlight = pre_done && flash_on;
    const uint32_t pre_bg = pre_highlight ? theme.selected : theme.surface;
    const uint32_t pre_fg = pre_highlight ? theme.on_selected : theme.accent;
    canvas.fillRoundRect(4, 117, w - 8, 28, 5, pre_bg);
    canvas.drawRoundRect(4, 117, w - 8, 28, 5, pre_done ? pre_fg : theme.border);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(pre_fg, pre_bg);
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
        canvas.setTextColor(pre_fg, pre_bg);
        canvas.setCursor(w - canvas.textWidth(pbuf) - 9, 121);
        canvas.print(pbuf);

        int64_t p_tot = (int64_t)pre_sec * 1000;
        int p_fill = (int)((pre_rem * (w - 18)) / p_tot);
        canvas.fillRect(9, 139, w - 18, 3, theme.alternate);
        canvas.fillRect(9, 139, p_fill, 3, pre_fg);
    }

    // 5. Recent Events Mini-Strip (up to 2 newest in Font2)
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    int ey = 148;
    if (ev_cnt == 0) {
        canvas.setTextColor(theme.muted, theme.background);
        canvas.setCursor(8, ey + 6);
        canvas.print("Press B to START");
    } else {
        for (size_t i = 0; i < 2 && i < ev_cnt; i++) {
            const EmsEvent &ev = recent[i];
            char elapsed[20];
            format_elapsed(ev.elapsed_ms / 1000, elapsed, sizeof(elapsed));
            canvas.setFont(&fonts::Font0);
            canvas.setTextColor(theme.muted, theme.background);
            canvas.drawString(elapsed, 6, ey + 5);
            canvas.setFont(&fonts::Font2);
            canvas.setTextColor(theme.text, theme.background);
            const int x = std::max(48, 9 + 6 * (int)std::strlen(elapsed));
            const int width = canvas.textWidth(ev.label);
            if (width > w - x - 4) canvas.setTextSize(float(w - x - 4) / width, 1.0f);
            canvas.drawString(ev.label, x, ey);
            canvas.setTextSize(1);
            ey += 15;
        }
    }

    // 6. Action Bar + Enlarged 2-line Footer
    if (st == EmsState::Idle) {
        canvas.fillRect(4, h - 56, w - 8, 22, theme.selected);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(theme.on_selected, theme.selected);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString("Start timer", w / 2, h - 45);
        canvas.setTextDatum(TL_DATUM);
        ui_draw_themed_footer(canvas, "A Preset   B Start", "Hold B: EPI interval");
    } else {
        const char *aname = action_name(sel_action, st);
        canvas.fillRect(4, h - 56, w - 8, 22, theme.selected);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(theme.on_selected, theme.selected);
        char act_line[32];
        std::snprintf(act_line, sizeof(act_line), "[ %s ]", aname);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString(act_line, w / 2, h - 45);
        canvas.setTextDatum(TL_DATUM);
        ui_draw_themed_footer(canvas, "A Next   B Confirm", "Hold B: Log EPI");
    }

    ui_present(canvas);
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
                        s_timer.start(now_ms, system_utc_ms());
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
                        case EmsAction::Rosc:
                        case EmsAction::Vital:
                        case EmsAction::Airway:
                        case EmsAction::Iv: {
                            constexpr EmsEventType events[] = {
                                EmsEventType::Shock, EmsEventType::Rosc, EmsEventType::Vital,
                                EmsEventType::Airway, EmsEventType::Iv,
                            };
                            {
                                TimerLock lock;
                                s_timer.log_event(events[(int)sel_action - (int)EmsAction::Shock], now_ms);
                            }
                            sound_fx_confirm();
                            sel_action = EmsAction::Epi;
                            break;
                        }
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

        if (ui_screen_is_on() && (now_ms - last_draw_ms) >= 100) {
            last_draw_ms = now_ms;
            draw_ems_frame(canvas, now_ms, sel_action);
        }

        input_idle_delay(20);
    }
}
