/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the review of #58 against #57: an override of an astro entry's offset moves its firings
// as the bounded astro search (sun_window(), #57) computes them; the search with the override equals
// the search over the whole window, at places from Munich to the polar circle.
#include "doctest.h"
#include "scheduler_test_support.h"

using hc::ScheduleBound;
using hc::ScheduleChange;
using hc::ScheduleMember;

TEST_CASE("ScheduleAdjustProbe.AstroOffsetOverrideKeepsTheBoundedSearchExact") {
	struct Place {
		const char* name;
		double latitude;
		double longitude;
	};
	const Place places[] = {{"Munich", 48.137, 11.575}, {"Tromsø", 69.65, 18.96}, {"date line west", 62.0, -179.9}};
	for (int32_t offset : {11 * 3600, -11 * 3600 - 1800, 0}) {
		for (const Place& place : places) {
			ScheduleBound bound;
			bound.member = ScheduleMember::kOffset;
			bound.min = -12 * 3600;
			bound.max = 12 * 3600;
			bound.step = 60;
			ScheduleSpec dusk = astro("dusk", SunEvent::dusk, 1800);
			dusk.adjustable = ScheduleMember::kOffset;
			dusk.bounds = &bound;
			dusk.bound_count = 1;
			Rig bounded;
			Rig reference;
			for (Rig* rig : {&bounded, &reference}) {
				rig->time.has_location = true;
				rig->time.location.latitude = place.latitude;
				rig->time.location.longitude = place.longitude;
				rig->time.set_utc(cet(2026, 1, 1, 0, 0));
				REQUIRE(rig->load({dusk}) == ScheduleStatus::ok);
				ScheduleSpec values;
				values.offset_s = offset;
				INFO(place.name << " offset " << offset);
				REQUIRE(rig->scheduler->set_members("dusk", ScheduleMember::kOffset, values) == ScheduleChange::ok);
			}
			reference.time.wide_windows = true;
			CHECK(bounded.scheduler->spec(0).offset_s == offset);
			for (int64_t t = cet(2026, 1, 1, 0, 0); t < cet(2027, 1, 1, 0, 0); t += 23 * 3600 + 41 * 60 + 7) {
				INFO(place.name << " offset " << offset << " after " << t);
				CHECK(bounded.scheduler->next_after(0, t) == reference.scheduler->next_after(0, t));
			}
		}
	}
}
