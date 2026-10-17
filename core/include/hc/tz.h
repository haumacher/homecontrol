/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_TZ_H
#define HC_TZ_H

#include <cstddef>
#include <cstdint>

#include "hc/platform/time_zone.h"

namespace hc {

/// Checks a POSIX TZ string ("CET-1CEST,M3.5.0,M10.5.0/3", "UTC0", "<+0330>-3:30"): a standard
/// zone name and offset, optionally a DST name with an optional offset and then *both* transition
/// rules (Jn, n or Mm.w.d, each with an optional /time). A DST zone without rules is refused: the C
/// libraries silently fall back to US rules for it. On refusal, `error` (if given) points to a
/// static text naming the problem.
bool posix_tz_valid(const char* tz, const char** error = nullptr);

/// Days since 1970-01-01 of a proleptic Gregorian date.
int64_t days_from_civil(int32_t year, unsigned month, unsigned day);

/// The local time's offset from UTC in seconds (local - utc), derived from the fields.
int32_t utc_offset_seconds(const LocalTime& local, int64_t utc_seconds);

/// Writes "YYYY-MM-DD HH:MM:SS ZONE (UTC+HH:MM)" into `buffer`.
void format_local_time(const LocalTime& local, int64_t utc_seconds, char* buffer, size_t size);

} // namespace hc

#endif // HC_TZ_H
