/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/image/rules_clock.h"

#include "hc/rules/value.h"

namespace hc {
namespace image {

namespace {

void date_time_of(const LocalTime& local, DateTime& out) {
	out = DateTime();
	out.year = static_cast<uint16_t>(local.year);
	out.month = local.month;
	out.day = local.day;
	out.hour = local.hour;
	out.minute = local.minute;
	out.second = local.second > 59 ? 59 : local.second;
	out.day_of_week = rules::weekday_of(local.year, local.month, local.day);
	out.flags = local.dst ? DateTime::kSummerTime : 0;
}

bool same_fields(const LocalTime& local, const DateTime& d) {
	return local.year == d.year && local.month == d.month && local.day == d.day && local.hour == d.hour &&
		local.minute == d.minute && local.second == d.second;
}

} // namespace

bool RulesClock::local_now(DateTime& out) const {
	LocalTime local;
	if (!time_.local_time(local)) {
		return false;
	}
	date_time_of(local, out);
	return true;
}

int64_t RulesClock::local_to_utc(const DateTime& local) const {
	Date date;
	date.year = local.year;
	date.month = local.month;
	date.day = local.day;
	// The first occurrence (or the end of a spring-forward gap), by the scheduler's rule.
	int64_t first = time_.local_to_utc(date, local.hour, local.minute, local.second);
	LocalTime at;
	bool summer = (local.flags & DateTime::kSummerTime) != 0;
	if (!time_.to_local(first, at) || !same_fields(at, local) || at.dst == summer) {
		return first;
	}
	// A time that occurs twice: the other occurrence has the same fields under the offset of the
	// day before or after (at most one offset change within a day either side, as local_to_utc()).
	int32_t offset = time_.utc_offset(first);
	for (int64_t probe : {first - 86400, first + 86400}) {
		int32_t other_offset = time_.utc_offset(probe);
		if (other_offset == offset) {
			continue;
		}
		int64_t other = first + offset - other_offset;
		LocalTime there;
		if (time_.to_local(other, there) && same_fields(there, local) && there.dst == summer) {
			return other;
		}
	}
	return first;
}

bool RulesClock::utc_to_local(int64_t utc_seconds, DateTime& out) const {
	LocalTime local;
	if (!time_.to_local(utc_seconds, local) || local.year < 1900 || local.year > 2155) {
		return false;
	}
	date_time_of(local, out);
	return true;
}

bool RulesClock::sun_time(rules::SunEvent event, TimeOfDay& out) const {
	LocalTime local;
	if (!time_.local_time(local)) {
		return false;
	}
	Date today;
	today.year = static_cast<uint16_t>(local.year);
	today.month = local.month;
	today.day = local.day;
	hc::SunEvent which = hc::SunEvent::sunrise;
	switch (event) {
		case rules::SunEvent::sunrise: which = hc::SunEvent::sunrise; break;
		case rules::SunEvent::sunset: which = hc::SunEvent::sunset; break;
		case rules::SunEvent::dawn: which = hc::SunEvent::dawn; break;
		case rules::SunEvent::dusk: which = hc::SunEvent::dusk; break;
	}
	LocalTime at;
	if (!time_.sun_event_local(today, which, at)) {
		return false;
	}
	out = TimeOfDay();
	out.hour = at.hour;
	out.minute = at.minute;
	out.second = at.second > 59 ? 59 : at.second;
	return true;
}

int64_t RulesClock::civil_to_utc(int32_t year, unsigned month, unsigned day, unsigned hour, unsigned minute,
	unsigned second) const {
	Date date;
	date.year = static_cast<uint16_t>(year);
	date.month = static_cast<uint8_t>(month);
	date.day = static_cast<uint8_t>(day);
	return time_.local_to_utc(date, hour, minute, second); // S1: the end of a gap, the first occurrence
}

rules::TimeSource::SunLookup RulesClock::sun_event_utc(int32_t year, unsigned month, unsigned day, hc::SunEvent event,
	int64_t& out) const {
	Date date;
	date.year = static_cast<uint16_t>(year);
	date.month = static_cast<uint8_t>(month);
	date.day = static_cast<uint8_t>(day);
	SunTimes times;
	if (!time_.sun_times(date, times)) {
		return SunLookup::no_location;
	}
	if (!times.occurs(event)) {
		return SunLookup::absent;
	}
	out = times.at(event);
	return SunLookup::ok;
}

bool RulesClock::sun_position(int64_t utc_seconds, SunPosition& out) const {
	GeoLocation where;
	if (!time_.location(where)) {
		return false;
	}
	out = hc::sun_position(utc_seconds, where);
	return true;
}

} // namespace image
} // namespace hc
