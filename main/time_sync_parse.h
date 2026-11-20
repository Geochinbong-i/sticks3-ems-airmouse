#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

enum class TimePayloadKind {
    Invalid = 0,
    LocalFields,
    UnixEpochUtc,
};

struct ParsedTimePayload {
    TimePayloadKind kind = TimePayloadKind::Invalid;
    int year = 0;   // full year, e.g. 2026
    int month = 0;  // 1..12
    int day = 0;    // 1..31
    int hour = 0;   // 0..23
    int min = 0;    // 0..59
    int sec = 0;    // 0..59
    int64_t epoch_sec = 0;
};

// Parses standard Bluetooth CTS 0x2A2B (7..10 bytes), 4-byte LE epoch,
// ASCII "YYYY-MM-DD HH:MM:SS", or ASCII 10-digit Unix epoch seconds.
ParsedTimePayload parse_ble_time_payload(const uint8_t *data, size_t len);

// Formats POSIX TZ string for fixed integer UTC offset (note POSIX sign inversion).
void format_posix_tz(int tz_offset_hours, char *buf, size_t len);

// In-place URL decoder for HTTP query parameters (+ -> space, %XX -> byte).
void url_decode_inplace(char *s);
