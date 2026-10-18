/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_RULES_CLOCK_H
#define HC_IMAGE_RULES_CLOCK_H

#include "hc/rules/engine.h"
#include "hc/time_service.h"

namespace hc {
namespace image {

/// The rule engine's view of the time service (spec/rules.md 7.7, 4.7): the local wall time while
/// the clock is synchronized, today's sun events at the device's location, and the conversions
/// between local civil time and the instant that date-time arithmetic runs on (spec 4.6), all
/// through the time zone the time service applies.
class RulesClock : public rules::TimeSource {
public:
	explicit RulesClock(const hc::TimeSource& time) : time_(time) {}

	bool local_now(DateTime& out) const override;
	bool sun_time(rules::SunEvent event, TimeOfDay& out) const override;
	int64_t local_to_utc(const DateTime& local) const override;
	bool utc_to_local(int64_t utc_seconds, DateTime& out) const override;

private:
	const hc::TimeSource& time_;
};

} // namespace image
} // namespace hc

#endif // HC_IMAGE_RULES_CLOCK_H
