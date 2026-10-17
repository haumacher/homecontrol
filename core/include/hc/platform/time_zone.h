/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_TIME_ZONE_H
#define HC_PLATFORM_TIME_ZONE_H

#include <cstdint>

namespace hc {

/// A broken-down local time as the platform's C library computes it.
struct LocalTime {
	int32_t year;   ///< e.g. 2026
	uint8_t month;  ///< 1..12
	uint8_t day;    ///< 1..31
	uint8_t hour;   ///< 0..23
	uint8_t minute; ///< 0..59
	uint8_t second; ///< 0..60
	bool dst;       ///< Daylight saving time in effect.
	char zone[8];   ///< Zone abbreviation ("CET", "CEST"), NUL-terminated, may be empty.
};

/// The local time zone of the platform. The core validates a POSIX TZ string (hc/tz.h) and hands
/// it over; the platforms apply it with setenv("TZ") and tzset(), which the core may not call.
class TimeZone {
public:
	/// Makes `posix_tz` the zone of to_local() (and of the platform's own local-time output).
	/// The string has been validated by hc::posix_tz_valid().
	virtual void apply(const char* posix_tz) = 0;

	/// Converts seconds since the epoch (UTC) into local time. Returns false if the platform
	/// cannot (the value is out of its range).
	virtual bool to_local(int64_t utc_seconds, LocalTime& out) const = 0;

protected:
	~TimeZone() = default;
};

} // namespace hc

#endif // HC_PLATFORM_TIME_ZONE_H
