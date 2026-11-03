/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The astro search of the scheduler is bounded by the sun windows (homecontrol #57): it computes
// the sun only for the dates that can still hold the next firing, and finds exactly what the
// search over the whole window finds.
#include "doctest.h"
#include "scheduler_test_support.h"

using hc::TimeService;
using hc_test::FakeClock;
using hc_test::FakeTimeZone;

namespace {

/// The test home's astro entries (E-Home doc/test-home.md: `dusk-dawn`) and the image fixture's
/// evening blinds (spec/fixtures/images/example.json: sunset -30 min within 17:00..22:00).
std::vector<ScheduleSpec> test_home_entries() {
	ScheduleSpec blinds = astro("blindsEvening", SunEvent::sunset, -30 * 60);
	blinds.has_not_before = true;
	blinds.not_before = at(17, 0);
	blinds.has_not_after = true;
	blinds.not_after = at(22, 0);
	return {astro("dusk", SunEvent::dusk, 0), astro("dawn", SunEvent::dawn, 0), blinds};
}

/// How many dates' sun the first evaluation after the clock synchronizes at `local` (CEST, UTC+2)
/// computes, with the controller's TimeService and its cache.
uint32_t computations_at_synchronization(int64_t utc_now) {
	FakeClock clock;
	FakeTimeZone zone;
	FakeSettings settings;
	hc::EventLoop loop;
	TimeService time{loop, clock, zone, settings};
	hc::GeoLocation munich;
	munich.latitude = 48.137;
	munich.longitude = 11.575;
	REQUIRE(time.set_location(munich));
	Scheduler scheduler(loop, time, settings, nullptr);
	time.add_listener(scheduler);
	time.start();
	std::vector<ScheduleSpec> specs = test_home_entries();
	REQUIRE(scheduler.load(specs.data(), specs.size()) == ScheduleStatus::ok);
	REQUIRE(time.sun_computations() == 0);
	clock.synchronized = true;
	clock.wall_seconds = utc_now;
	time.refresh(); // the watch's tick: kSynchronized, the scheduler evaluates
	for (size_t i = 0; i < specs.size(); ++i) {
		REQUIRE(scheduler.next_firing(i));
	}
	return time.sun_computations();
}

} // namespace

TEST_CASE("SchedulerAstro.FirstEvaluationComputesTheSunForTwoDates") {
	// Through the day and the year: morning, afternoon, after sunset, before midnight, after it.
	const int64_t moments[] = {
		cest(2026, 11, 3, 7, 30), cest(2026, 11, 3, 15, 0), cest(2026, 11, 3, 18, 30),
		cest(2026, 11, 3, 23, 50), cest(2026, 11, 4, 0, 10), cest(2026, 6, 21, 3, 0),
		cest(2026, 6, 21, 22, 0), cest(2026, 6, 21, 23, 30), cest(2026, 3, 20, 12, 0),
	};
	for (int64_t moment : moments) {
		uint32_t computed = computations_at_synchronization(moment);
		INFO("at " << moment << ": " << computed << " dates");
		CHECK(computed >= 1);
		CHECK(computed <= 2); // was 10: yesterday and the next eight days
	}
}

TEST_CASE("SchedulerAstro.BoundedSearchFindsWhatTheWholeWindowFinds") {
	// Places across the longitudes and into the polar circles (polar day and night, dusk after
	// midnight), entries with the extreme offsets, clamps and weekdays; the reference is the same
	// scheduler on windows too wide to skip any date.
	struct Place {
		const char* name;
		double latitude;
		double longitude;
	};
	const Place places[] = {
		{"Munich", 48.137, 11.575}, {"Tromsø", 69.65, 18.96}, {"Helsinki", 60.17, 24.94},
		{"Sydney", -33.87, 151.21}, {"date line east", 10.0, 179.9}, {"date line west", 62.0, -179.9},
		{"McMurdo", -77.85, 166.67},
	};
	auto entries = [] {
		std::vector<ScheduleSpec> specs = test_home_entries();
		specs.push_back(astro("rise-minus-12h", SunEvent::sunrise, -12 * 3600));
		specs.push_back(astro("set-plus-12h", SunEvent::sunset, 12 * 3600));
		ScheduleSpec night = astro("dusk-plus-3h-clamped", SunEvent::dusk, 3 * 3600);
		night.has_not_before = true;
		night.not_before = at(23, 30);
		specs.push_back(night);
		ScheduleSpec weekend = astro("weekend-dawn", SunEvent::dawn, -2 * 3600);
		weekend.days = 0x20 | 0x40;
		weekend.has_not_after = true;
		weekend.not_after = at(4, 0);
		specs.push_back(weekend);
		ScheduleSpec monday = astro("monday-rise", SunEvent::sunrise, 0);
		monday.days = 0x01;
		specs.push_back(monday);
		return specs;
	};
	std::vector<ScheduleSpec> specs = entries();
	for (const Place& place : places) {
		Rig bounded;
		Rig reference;
		for (Rig* rig : {&bounded, &reference}) {
			rig->time.has_location = true;
			rig->time.location.latitude = place.latitude;
			rig->time.location.longitude = place.longitude;
			rig->time.set_utc(cet(2026, 1, 1, 0, 0));
			REQUIRE(rig->load(specs) == ScheduleStatus::ok);
		}
		reference.time.wide_windows = true;
		size_t compared = 0;
		size_t found = 0;
		for (int64_t t = cet(2026, 1, 1, 0, 0); t < cet(2027, 1, 1, 0, 0); t += 29 * 3600 + 17 * 60 + 13) {
			for (size_t i = 0; i < specs.size(); ++i) {
				std::optional<int64_t> got = bounded.scheduler->next_after(i, t);
				std::optional<int64_t> expected = reference.scheduler->next_after(i, t);
				INFO(place.name << ", " << specs[i].id << ", after " << t);
				CHECK(got == expected);
				++compared;
				found += expected ? 1 : 0;
			}
		}
		INFO(place.name);
		CHECK(found > compared / 4); // even McMurdo has its events for part of the year
	}
}
