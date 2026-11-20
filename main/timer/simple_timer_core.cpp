#include "simple_timer_core.h"

#include <cstdio>

void SimpleTimerCore::reset() {
    for (size_t i = 0; i < kMaxStamps; i++) {
        stamps_[i] = SimpleStamp{};
    }
    count_ = 0;
}

bool SimpleTimerCore::stamp(int hour, int min, int sec) {
    if (count_ >= kMaxStamps) return false;
    if (hour < 0) hour = 0;
    if (hour > 23) hour %= 24;
    if (min < 0) min = 0;
    if (min > 59) min %= 60;
    if (sec < 0) sec = 0;
    if (sec > 59) sec %= 60;

    stamps_[count_].hour = (uint8_t)hour;
    stamps_[count_].min = (uint8_t)min;
    stamps_[count_].sec = (uint8_t)sec;
    stamps_[count_].valid = true;
    count_++;
    return true;
}

const SimpleStamp *SimpleTimerCore::at(size_t idx) const {
    if (idx >= count_) return nullptr;
    return &stamps_[idx];
}

void SimpleTimerCore::format_time(const SimpleStamp &s, char *buf, size_t len) {
    if (!buf || len == 0) return;
    if (!s.valid) {
        std::snprintf(buf, len, "--:--:--");
        return;
    }
    std::snprintf(buf, len, "%02u:%02u:%02u",
                  (unsigned)s.hour, (unsigned)s.min, (unsigned)s.sec);
}
