#pragma once

#include <cstdint>
#include <cstddef>

enum class EmsState : uint8_t {
    Idle = 0,
    Running = 1,
    Paused = 2,
};

enum class EmsEventType : uint8_t {
    Start, Epi, Shock, Rosc, Vital, Airway, Pause, Resume, Iv,
};

struct EmsEvent {
    int64_t elapsed_ms;
    EmsEventType type;
    uint16_t seq;
    char label[16];
};

struct EmsTickResult {
    bool epi_just_due;
    bool epi_reminder_beep;
    bool preset_just_due;
};

class EmsTimerCore {
public:
    static constexpr size_t kMaxEvents = 64;
    static constexpr int kPresetCount = 7;

    EmsTimerCore();

    void reset();
    void start(int64_t now_ms, int64_t utc_ms = 0);
    void pause(int64_t now_ms);
    void resume(int64_t now_ms);
    void toggle_pause(int64_t now_ms);

    void set_epi_interval_sec(int sec);
    int epi_interval_sec() const { return epi_interval_sec_; }
    void cycle_epi_interval();

    void set_preset_index(int idx);
    int preset_index() const { return preset_idx_; }
    int preset_sec() const;
    void cycle_preset();

    void log_epi(int64_t now_ms);
    void log_event(EmsEventType type, int64_t now_ms);
    void restart_preset(int64_t now_ms);
    void acknowledge_alarms();

    EmsTickResult tick(int64_t now_ms);

    EmsState state() const { return state_; }
    int64_t total_elapsed_ms(int64_t now_ms) const;
    void sync_clock(int64_t now_ms, int64_t utc_ms);
    int64_t case_start_utc_ms() const { return case_start_utc_ms_; }

    bool epi_active() const { return epi_dose_count_ > 0; }
    int epi_dose_count() const { return epi_dose_count_; }
    int shock_count() const { return shock_count_; }
    int64_t epi_remaining_ms(int64_t now_ms) const; // negative when overdue
    bool epi_is_overdue(int64_t now_ms) const;
    bool epi_alarm_active() const { return epi_alarm_active_; }

    bool preset_enabled() const { return preset_sec() > 0; }
    int64_t preset_remaining_ms(int64_t now_ms) const; // 0 when done
    bool preset_is_done(int64_t now_ms) const;
    bool preset_alarm_active() const { return preset_alarm_active_; }

    size_t event_count() const { return event_count_; }
    const EmsEvent *event_at(size_t newest_idx) const; // 0 = most recent

    static void format_mm_ss(int64_t ms, char *buf, size_t len);
    static void format_hh_mm_ss(int64_t ms, char *buf, size_t len);

private:
    void add_event(EmsEventType type, uint16_t seq, int64_t now_ms);
    int64_t active_elapsed_since(int64_t anchor_ms, int64_t now_ms) const;

    EmsState state_ = EmsState::Idle;
    int64_t total_accum_ms_ = 0;
    int64_t run_started_ms_ = 0;
    int64_t case_started_ms_ = 0;
    int64_t case_start_utc_ms_ = 0;

    int epi_interval_sec_ = 180; // default 3 min
    int epi_dose_count_ = 0;
    int shock_count_ = 0;
    int64_t epi_accum_ms_ = 0;
    int64_t epi_anchor_ms_ = 0;
    bool epi_due_fired_ = false;
    bool epi_alarm_active_ = false;
    int64_t last_epi_remind_ms_ = 0;

    int preset_idx_ = 4; // default 5 min (300s)
    int64_t preset_accum_ms_ = 0;
    int64_t preset_anchor_ms_ = 0;
    bool preset_due_fired_ = false;
    bool preset_alarm_active_ = false;

    EmsEvent events_[kMaxEvents]{};
    size_t event_count_ = 0;
    size_t next_event_ = 0;
};
