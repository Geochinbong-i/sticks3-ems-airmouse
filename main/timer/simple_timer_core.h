#pragma once

#include <cstddef>
#include <cstdint>

struct SimpleStamp {
    uint8_t hour = 0;
    uint8_t min = 0;
    uint8_t sec = 0;
    bool valid = false;
};

class SimpleTimerCore {
public:
    static constexpr size_t kMaxStamps = 7;

    void reset();
    bool stamp(int hour, int min, int sec);

    size_t count() const { return count_; }
    bool is_full() const { return count_ >= kMaxStamps; }
    const SimpleStamp *at(size_t idx) const;

    static void format_time(const SimpleStamp &s, char *buf, size_t len);

private:
    SimpleStamp stamps_[kMaxStamps]{};
    size_t count_ = 0;
};
