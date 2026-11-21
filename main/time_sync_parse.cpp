#include "time_sync_parse.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
bool valid_ymd_hms(int y, int mo, int d, int h, int mi, int s) {
    return (y >= 2024 && y <= 2099 &&
            mo >= 1 && mo <= 12 &&
            d >= 1 && d <= 31 &&
            h >= 0 && h <= 23 &&
            mi >= 0 && mi <= 59 &&
            s >= 0 && s <= 59);
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

ParsedTimePayload parse_ble_time_payload(const uint8_t *data, size_t len) {
    ParsedTimePayload out{};
    if (!data || len == 0) return out;

    // 1. Standard Bluetooth CTS Current Time (0x2A2B): 7..10+ bytes (little-endian year)
    if (len >= 7 && len <= 12) {
        int y = (int)data[0] | ((int)data[1] << 8);
        int mo = (int)data[2];
        int d = (int)data[3];
        int h = (int)data[4];
        int mi = (int)data[5];
        int s = (int)data[6];
        if (valid_ymd_hms(y, mo, d, h, mi, s)) {
            out.kind = TimePayloadKind::LocalFields;
            out.year = y;
            out.month = mo;
            out.day = d;
            out.hour = h;
            out.min = mi;
            out.sec = s;
            return out;
        }
    }

    // 2. 4-byte unsigned little-endian Unix epoch seconds
    if (len == 4) {
        uint32_t ep = (uint32_t)data[0] |
                      ((uint32_t)data[1] << 8) |
                      ((uint32_t)data[2] << 16) |
                      ((uint32_t)data[3] << 24);
        if (ep >= 1700000000u && ep <= 4000000000u) {
            out.kind = TimePayloadKind::UnixEpochUtc;
            out.epoch_sec = (int64_t)ep;
            return out;
        }
    }

    // 3. ASCII representations ("YYYY-MM-DD HH:MM:SS" or Unix epoch digits)
    char str[40]{};
    size_t n = (len < sizeof(str) - 1) ? len : (sizeof(str) - 1);
    std::memcpy(str, data, n);
    str[n] = '\0';

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (std::sscanf(str, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6 ||
        std::sscanf(str, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6 ||
        std::sscanf(str, "%d/%d/%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
        if (valid_ymd_hms(y, mo, d, h, mi, s)) {
            out.kind = TimePayloadKind::LocalFields;
            out.year = y;
            out.month = mo;
            out.day = d;
            out.hour = h;
            out.min = mi;
            out.sec = s;
            return out;
        }
    }

    const char *p = str;
    if (*p == '@') p++;
    if (std::isdigit((unsigned char)*p)) {
        char *endp = nullptr;
        long long ep = std::strtoll(p, &endp, 10);
        if (endp != p && ep >= 1700000000LL && ep <= 4000000000LL) {
            out.kind = TimePayloadKind::UnixEpochUtc;
            out.epoch_sec = (int64_t)ep;
            return out;
        }
    }

    return out;
}

void format_posix_tz(int tz_offset_hours, char *buf, size_t len) {
    if (!buf || len == 0) return;
    if (tz_offset_hours < -12) tz_offset_hours = -12;
    if (tz_offset_hours > 14) tz_offset_hours = 14;
    if (tz_offset_hours == 0) {
        std::snprintf(buf, len, "UTC0");
    } else {
        // POSIX TZ reverses sign relative to ISO offset (+9 => "UTC-9")
        std::snprintf(buf, len, "UTC%+d", -tz_offset_hours);
    }
}

bool url_decode_inplace(char *s) {
    if (!s) return false;
    char *dst = s;
    for (const char *src = s; *src != '\0'; ++src) {
        if (*src == '+') {
            *dst++ = ' ';
        } else if (*src == '%') {
            if (src[1] == '\0' || src[2] == '\0') return false;
            const int hi = hex_val(src[1]);
            const int lo = hex_val(src[2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return false;
            *dst++ = static_cast<char>((hi << 4) | lo);
            src += 2;
        } else {
            *dst++ = *src;
        }
    }
    *dst = '\0';
    return true;
}
