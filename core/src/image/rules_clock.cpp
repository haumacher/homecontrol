/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/image/rules_clock.h"

#include "hc/rules/value.h"

namespace hc {
namespace image {

bool RulesClock::local_now(DateTime& out) const {
	LocalTime local;
	if (!time_.local_time(local)) {
		return false;
	}
	out = DateTime();
	out.year = static_cast<uint16_t>(local.year);
	out.month = local.month;
	out.day = local.day;
	out.hour = local.hour;
	out.minute = local.minute;
	out.second = local.second > 59 ? 59 : local.second;
	out.day_of_week = rules::weekday_of(local.year, local.month, local.day);
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

} // namespace image
} // namespace hc
