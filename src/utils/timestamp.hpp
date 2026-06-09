#pragma once
#include <chrono>
#include <cstdint>
#include <QString>
#include <QDateTime>

namespace mosaic {

/// @brief Alias for @c std::chrono::steady_clock — a monotonic clock that is
///        immune to wall-clock adjustments (NTP, DST, user changes).
using SteadyClock = std::chrono::steady_clock;

/// @brief A point in time on the SteadyClock.
using TimePoint   = SteadyClock::time_point;

/// @brief Nanoseconds elapsed since the first call to any elapsed_* function
///        (effectively since application start).
///
/// Use this for **durations and inter-event intervals** — in particular for
/// stamping video frames and trigger events, which must be aligned to each
/// other.  This clock never jumps backwards.
///
/// @returns Nanoseconds elapsed (always non-negative, starts near 0).
[[nodiscard]] inline int64_t elapsed_ns() noexcept {
    static const TimePoint start = SteadyClock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        SteadyClock::now() - start).count();
}

/// @returns elapsed_ns() converted to milliseconds (truncated, not rounded).
[[nodiscard]] inline int64_t elapsed_ms() noexcept { return elapsed_ns() / 1'000'000; }

/// @returns elapsed_ns() as seconds (floating point).
[[nodiscard]] inline double  elapsed_s()  noexcept { return elapsed_ns() / 1e9; }

/// @brief Nanoseconds since the Unix epoch (@c system_clock).
///
/// Use this for **absolute timestamps** in file names and in
/// @c session_meta.json — where you need to know *when* the event happened
/// in calendar time, not just how long after session start.
///
/// @warning Unlike elapsed_ns(), this value can jump backwards if the system
///          clock is adjusted.  Never use it for duration measurements.
///
/// @returns Nanoseconds since 1970-01-01T00:00:00Z.
[[nodiscard]] inline int64_t wall_clock_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count();
}

/// @brief Formatted wall-clock string for log lines.
///
/// @param fmt  @c QDateTime format string.  Default: @c "hh:mm:ss.zzz".
/// @returns    A string like @c "14:32:05.123".
[[nodiscard]] inline QString wall_clock_string(const char* fmt = "hh:mm:ss.zzz") {
    return QDateTime::currentDateTime().toString(fmt);
}

/// @brief Formatted wall-clock string suitable for file/folder names.
///
/// @returns A string like @c "2026-06-04_14-32-05" (colons replaced with dashes
///          for filesystem compatibility on all platforms).
[[nodiscard]] inline QString wall_clock_filename() {
    return QDateTime::currentDateTime().toString("yyyy-MM-dd_hh-mm-ss");
}

} // namespace mosaic
