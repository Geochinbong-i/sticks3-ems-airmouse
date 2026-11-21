#pragma once

struct EmsAlarmState {
    bool epi;
    bool preset;

    bool any() const { return epi || preset; }
    const char *title() const {
        if (epi && preset) return "EPI/VITAL DUE";
        if (epi) return "EPI DUE!";
        if (preset) return "VITAL DUE!";
        return nullptr;
    }
};

void ems_timer_init();
void ems_timer_sync_clock();
void ems_timer_ui_run();
bool ems_timer_is_running();
EmsAlarmState ems_timer_active_alarms();
unsigned ems_timer_alarm_sequence();
