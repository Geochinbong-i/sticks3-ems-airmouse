#include "ems_timer_core.h"

#include <cstdio>
#include <algorithm>

static constexpr int kPresetSeconds[EmsTimerCore::kPresetCount] = {
    0, 60, 120, 180, 300, 600, 900
};

EmsTimerCore::EmsTimerCore() {
    reset();
}

void EmsTimerCore::reset() {
    state_ = EmsState::Idle;
    total_accum_ms_ = 0;
    run_started_ms_ = 0;
    case_started_ms_ = 0;
    case_start_utc_ms_ = 0;

    epi_dose_count_ = 0;
    shock_count_ = 0;
    epi_accum_ms_ = 0;
    epi_anchor_ms_ = 0;
    epi_due_fired_ = false;
    epi_alarm_active_ = false;
    last_epi_remind_ms_ = 0;

    preset_accum_ms_ = 0;
    preset_anchor_ms_ = 0;
    preset_due_fired_ = false;
    preset_alarm_active_ = false;

    event_count_ = 0;
    next_event_ = 0;
}

void EmsTimerCore::set_epi_interval_sec(int sec) {
    if (sec == 180 || sec == 240 || sec == 300) {
        epi_interval_sec_ = sec;
    }
}

void EmsTimerCore::cycle_epi_interval() {
    if (epi_interval_sec_ == 180) epi_interval_sec_ = 240;
    else if (epi_interval_sec_ == 240) epi_interval_sec_ = 300;
    else epi_interval_sec_ = 180;
}

void EmsTimerCore::set_preset_index(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kPresetCount) idx = kPresetCount - 1;
    preset_idx_ = idx;
    preset_accum_ms_ = 0;
    preset_due_fired_ = false;
    preset_alarm_active_ = false;
}

int EmsTimerCore::preset_sec() const {
    return kPresetSeconds[preset_idx_];
}

void EmsTimerCore::cycle_preset() {
    preset_idx_ = (preset_idx_ + 1) % kPresetCount;
    preset_accum_ms_ = 0;
    preset_due_fired_ = false;
    preset_alarm_active_ = false;
}

void EmsTimerCore::start(int64_t now_ms, int64_t utc_ms) {
    if (state_ == EmsState::Running) return;
    if (state_ == EmsState::Idle) {
        total_accum_ms_ = 0;
        run_started_ms_ = now_ms;
        case_started_ms_ = now_ms;
        case_start_utc_ms_ = utc_ms;
        epi_accum_ms_ = 0;
        epi_anchor_ms_ = now_ms;
        preset_accum_ms_ = 0;
        preset_anchor_ms_ = now_ms;
        state_ = EmsState::Running;
        add_event(EmsEventType::CaseStart, 0, now_ms);
    } else {
        resume(now_ms);
    }
}

void EmsTimerCore::sync_clock(int64_t now_ms, int64_t utc_ms) {
    if (state_ == EmsState::Idle || utc_ms <= 0) return;
    case_start_utc_ms_ = utc_ms - std::max<int64_t>(0, now_ms - case_started_ms_);
}

void EmsTimerCore::pause(int64_t now_ms) {
    if (state_ != EmsState::Running) return;
    int64_t delta = now_ms - run_started_ms_;
    if (delta < 0) delta = 0;
    total_accum_ms_ += delta;
    if (epi_dose_count_ > 0) {
        int64_t ed = now_ms - epi_anchor_ms_;
        if (ed > 0) epi_accum_ms_ += ed;
    }
    if (preset_enabled()) {
        int64_t pd = now_ms - preset_anchor_ms_;
        if (pd > 0) preset_accum_ms_ += pd;
    }
    state_ = EmsState::Paused;
    add_event(EmsEventType::TimerPause, 0, now_ms);
}

void EmsTimerCore::resume(int64_t now_ms) {
    if (state_ != EmsState::Paused) return;
    run_started_ms_ = now_ms;
    epi_anchor_ms_ = now_ms;
    preset_anchor_ms_ = now_ms;
    state_ = EmsState::Running;
    add_event(EmsEventType::TimerResume, 0, now_ms);
}

void EmsTimerCore::toggle_pause(int64_t now_ms) {
    if (state_ == EmsState::Running) pause(now_ms);
    else if (state_ == EmsState::Paused) resume(now_ms);
    else start(now_ms);
}

void EmsTimerCore::log_epi(int64_t now_ms) {
    if (state_ == EmsState::Idle) {
        start(now_ms);
    }
    epi_dose_count_++;
    epi_accum_ms_ = 0;
    epi_anchor_ms_ = now_ms;
    epi_due_fired_ = false;
    epi_alarm_active_ = false;
    last_epi_remind_ms_ = now_ms;
    add_event(EmsEventType::Epi, (uint16_t)epi_dose_count_, now_ms);
}

void EmsTimerCore::log_event(EmsEventType type, int64_t now_ms) {
    if (type == EmsEventType::Epi) {
        log_epi(now_ms);
        return;
    }
    if (state_ == EmsState::Idle) {
        start(now_ms);
    }
    uint16_t seq = 0;
    if (type == EmsEventType::Shock) {
        shock_count_++;
        seq = (uint16_t)shock_count_;
    } else if (type == EmsEventType::Vital) {
        restart_preset(now_ms);
    }
    add_event(type, seq, now_ms);
}

void EmsTimerCore::restart_preset(int64_t now_ms) {
    preset_accum_ms_ = 0;
    preset_anchor_ms_ = now_ms;
    preset_due_fired_ = false;
    preset_alarm_active_ = false;
}

void EmsTimerCore::acknowledge_alarms() {
    epi_alarm_active_ = false;
    preset_alarm_active_ = false;
}

int64_t EmsTimerCore::total_elapsed_ms(int64_t now_ms) const {
    if (state_ == EmsState::Idle) return 0;
    if (state_ == EmsState::Paused) return total_accum_ms_;
    int64_t delta = now_ms - run_started_ms_;
    return total_accum_ms_ + (delta > 0 ? delta : 0);
}

int64_t EmsTimerCore::active_elapsed_since(int64_t anchor_ms, int64_t now_ms) const {
    if (state_ != EmsState::Running) return 0;
    int64_t d = now_ms - anchor_ms;
    return d > 0 ? d : 0;
}

int64_t EmsTimerCore::epi_remaining_ms(int64_t now_ms) const {
    int64_t interval_ms = (int64_t)epi_interval_sec_ * 1000;
    if (epi_dose_count_ <= 0) return interval_ms;
    int64_t elapsed = epi_accum_ms_ + active_elapsed_since(epi_anchor_ms_, now_ms);
    return interval_ms - elapsed;
}

bool EmsTimerCore::epi_is_overdue(int64_t now_ms) const {
    return epi_dose_count_ > 0 && epi_remaining_ms(now_ms) <= 0;
}

int64_t EmsTimerCore::preset_remaining_ms(int64_t now_ms) const {
    if (!preset_enabled()) return 0;
    int64_t target_ms = (int64_t)preset_sec() * 1000;
    if (state_ == EmsState::Idle) return target_ms;
    int64_t elapsed = preset_accum_ms_ + active_elapsed_since(preset_anchor_ms_, now_ms);
    int64_t rem = target_ms - elapsed;
    return rem > 0 ? rem : 0;
}

bool EmsTimerCore::preset_is_done(int64_t now_ms) const {
    return preset_enabled() && state_ != EmsState::Idle && preset_remaining_ms(now_ms) == 0;
}

EmsTickResult EmsTimerCore::tick(int64_t now_ms) {
    EmsTickResult res{false, false, false};
    if (state_ != EmsState::Running) return res;

    if (epi_dose_count_ > 0 && epi_remaining_ms(now_ms) <= 0) {
        if (!epi_due_fired_) {
            epi_due_fired_ = true;
            epi_alarm_active_ = true;
            last_epi_remind_ms_ = now_ms;
            res.epi_just_due = true;
        } else if (epi_alarm_active_ && (now_ms - last_epi_remind_ms_) >= 15000) {
            last_epi_remind_ms_ = now_ms;
            res.epi_reminder_beep = true;
        }
    }

    if (preset_enabled() && preset_remaining_ms(now_ms) == 0) {
        if (!preset_due_fired_) {
            preset_due_fired_ = true;
            preset_alarm_active_ = true;
            res.preset_just_due = true;
        }
    }

    return res;
}

void EmsTimerCore::add_event(EmsEventType type, uint16_t seq, int64_t now_ms) {
    EmsEvent &ev = events_[next_event_];
    next_event_ = (next_event_ + 1) % kMaxEvents;
    event_count_ = std::min(event_count_ + 1, kMaxEvents);
    ev.elapsed_ms = std::max<int64_t>(0, now_ms - case_started_ms_);
    ev.type = type;
    ev.seq = seq;

    switch (type) {
        case EmsEventType::CaseStart: std::snprintf(ev.label, sizeof(ev.label), "CASE START"); break;
        case EmsEventType::Epi:    std::snprintf(ev.label, sizeof(ev.label), "EPI #%u", (unsigned)seq); break;
        case EmsEventType::Shock:  std::snprintf(ev.label, sizeof(ev.label), "SHOCK #%u", (unsigned)seq); break;
        case EmsEventType::Rosc:   std::snprintf(ev.label, sizeof(ev.label), "ROSC"); break;
        case EmsEventType::Vital:  std::snprintf(ev.label, sizeof(ev.label), "VITAL CHECK"); break;
        case EmsEventType::Airway: std::snprintf(ev.label, sizeof(ev.label), "AIRWAY PLACED"); break;
        case EmsEventType::TimerPause: std::snprintf(ev.label, sizeof(ev.label), "TIMER PAUSED"); break;
        case EmsEventType::TimerResume: std::snprintf(ev.label, sizeof(ev.label), "TIMER RESUMED"); break;
        case EmsEventType::CprStart: std::snprintf(ev.label, sizeof(ev.label), "CPR START"); break;
        case EmsEventType::RhythmVf: std::snprintf(ev.label, sizeof(ev.label), "RHYTHM VF"); break;
        case EmsEventType::RhythmPvt: std::snprintf(ev.label, sizeof(ev.label), "RHYTHM pVT"); break;
        case EmsEventType::RhythmPea: std::snprintf(ev.label, sizeof(ev.label), "RHYTHM PEA"); break;
        case EmsEventType::RhythmAsystole: std::snprintf(ev.label, sizeof(ev.label), "ASYSTOLE"); break;
        case EmsEventType::Amiodarone: std::snprintf(ev.label, sizeof(ev.label), "AMIODARONE"); break;
        case EmsEventType::Lidocaine: std::snprintf(ev.label, sizeof(ev.label), "LIDOCAINE"); break;
        case EmsEventType::IvAccess: std::snprintf(ev.label, sizeof(ev.label), "IV ACCESS"); break;
        case EmsEventType::IoAccess: std::snprintf(ev.label, sizeof(ev.label), "IO ACCESS"); break;
        case EmsEventType::Capnography: std::snprintf(ev.label, sizeof(ev.label), "CAPNO START"); break;
        case EmsEventType::Handoff: std::snprintf(ev.label, sizeof(ev.label), "HANDOFF"); break;
        case EmsEventType::Rearrest: std::snprintf(ev.label, sizeof(ev.label), "RE-ARREST"); break;
    }
}

const EmsEvent *EmsTimerCore::event_at(size_t newest_idx) const {
    if (newest_idx >= event_count_) return nullptr;
    return &events_[(next_event_ + kMaxEvents - 1 - newest_idx) % kMaxEvents];
}

void EmsTimerCore::format_mm_ss(int64_t ms, char *buf, size_t len) {
    if (ms < 0) ms = -ms;
    int64_t total_sec = ms / 1000;
    int mins = (int)(total_sec / 60);
    int secs = (int)(total_sec % 60);
    std::snprintf(buf, len, "%02d:%02d", mins, secs);
}

void EmsTimerCore::format_hh_mm_ss(int64_t ms, char *buf, size_t len) {
    if (ms < 0) ms = 0;
    int64_t total_sec = ms / 1000;
    int hrs = (int)(total_sec / 3600);
    int mins = (int)((total_sec % 3600) / 60);
    int secs = (int)(total_sec % 60);
    std::snprintf(buf, len, "%02d:%02d:%02d", hrs, mins, secs);
}
