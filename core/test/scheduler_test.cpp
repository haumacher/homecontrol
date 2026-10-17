/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "doctest.h"
#include "scheduler_test_support.h"

TEST_CASE("Scheduler.WeeklyFiresOnItsDays") {
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 11, 12, 0)); // Sunday
	REQUIRE(rig.load({weekly("mwf", kMonWedFri, at(7, 0))}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 12, 7, 0));
	rig.run_until(cest(2026, 10, 18, 23, 0));
	Firings expected = {{"mwf", cest(2026, 10, 12, 7, 0)}, {"mwf", cest(2026, 10, 14, 7, 0)},
		{"mwf", cest(2026, 10, 16, 7, 0)}};
	Firings got = rig.take();
	INFO(describe(got));
	CHECK(got == expected);
	CHECK(rig.scheduler->last_fired(0) == cest(2026, 10, 16, 7, 0));
}

TEST_CASE("Scheduler.YearlyDatesAndOnce") {
	Rig rig;
	rig.time.set_utc(cet(2026, 12, 23, 12, 0));
	const Date holidays[] = {{2027, 1, 6}, {2026, 12, 26}, {2026, 12, 24}};
	REQUIRE(rig.load({yearly("xmas", 12, 25, at(8, 0)), dates("holiday", holidays, 3, at(9, 30)),
		once("party", Date{2026, 12, 31}, at(23, 59, 30))}) == ScheduleStatus::ok);
	rig.run_until(cet(2027, 1, 10, 0, 0));
	Firings expected = {{"holiday", cet(2026, 12, 24, 9, 30)}, {"xmas", cet(2026, 12, 25, 8, 0)},
		{"holiday", cet(2026, 12, 26, 9, 30)}, {"party", cet(2026, 12, 31, 23, 59, 30)},
		{"holiday", cet(2027, 1, 6, 9, 30)}};
	Firings got = rig.take();
	INFO(describe(got));
	CHECK(got == expected);
	CHECK(rig.scheduler->next_firing(0) == cet(2027, 12, 25, 8, 0));
	CHECK_FALSE(rig.scheduler->next_firing(1)); // the list is exhausted
	CHECK_FALSE(rig.scheduler->next_firing(2)); // a one-shot fires once
}

TEST_CASE("Scheduler.IntervalSlotsOnTheGivenDays") {
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 16, 7, 0)); // Friday
	REQUIRE(rig.load({interval("pump", kWorkdays, 20, at(8, 0), at(9, 0))}) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 19, 8, 30)); // to Monday
	Firings expected = {{"pump", cest(2026, 10, 16, 8, 0)}, {"pump", cest(2026, 10, 16, 8, 20)},
		{"pump", cest(2026, 10, 16, 8, 40)}, {"pump", cest(2026, 10, 16, 9, 0)},
		{"pump", cest(2026, 10, 19, 8, 0)}, {"pump", cest(2026, 10, 19, 8, 20)}};
	Firings got = rig.take();
	INFO(describe(got));
	CHECK(got == expected);
	// `until` that is not on the grid: the last slot is the latest one before it.
	CHECK(rig.load({interval("odd", Weekdays::kAll, 25, at(8, 0), at(9, 0))}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->next_after(0, cest(2026, 10, 19, 8, 40)) == cest(2026, 10, 19, 8, 50));
	CHECK(rig.scheduler->next_after(0, cest(2026, 10, 19, 8, 50)) == cest(2026, 10, 20, 8, 0));
}

TEST_CASE("Scheduler.AstroWithOffsetAndClampWindow") {
	Rig rig;
	rig.time.has_location = true;
	rig.time.location.latitude = 48.137;
	rig.time.location.longitude = 11.575;
	rig.time.set_utc(cest(2026, 6, 20, 12, 0));
	ScheduleSpec plain = astro("dusk-30", SunEvent::sunset, -30 * 60);
	ScheduleSpec late = astro("not-after", SunEvent::sunset, 0);
	late.has_not_after = true;
	late.not_after = at(21, 0);
	ScheduleSpec early = astro("not-before", SunEvent::sunrise, 0);
	early.has_not_before = true;
	early.not_before = at(6, 30);
	ScheduleSpec window = astro("window", SunEvent::sunrise, 0);
	window.has_not_before = true;
	window.not_before = at(4, 0);
	window.has_not_after = true;
	window.not_after = at(7, 0);
	REQUIRE(rig.load({plain, late, early, window}) == ScheduleStatus::ok);

	hc::SunTimes june21 = hc::sun_times(2026, 6, 21, rig.time.location);
	// Munich, 21 June: sunrise 05:13 CEST, sunset 21:18 CEST.
	CHECK(rig.scheduler->next_after(0, cest(2026, 6, 21, 0, 0)) == june21.at(SunEvent::sunset) - 1800);
	CHECK(rig.scheduler->next_after(1, cest(2026, 6, 21, 0, 0)) == cest(2026, 6, 21, 21, 0));
	CHECK(rig.scheduler->next_after(2, cest(2026, 6, 21, 0, 0)) == cest(2026, 6, 21, 6, 30));
	CHECK(rig.scheduler->next_after(3, cest(2026, 6, 21, 0, 0)) == june21.at(SunEvent::sunrise));

	// In December the clamps let the sun through, or hold it the other way.
	hc::SunTimes december = hc::sun_times(2026, 12, 21, rig.time.location);
	CHECK(rig.scheduler->next_after(1, cet(2026, 12, 21, 0, 0)) == december.at(SunEvent::sunset));
	CHECK(rig.scheduler->next_after(2, cet(2026, 12, 21, 0, 0)) == december.at(SunEvent::sunrise));
	CHECK(rig.scheduler->next_after(3, cet(2026, 12, 21, 0, 0)) == cet(2026, 12, 21, 7, 0));

	rig.run_until(cest(2026, 6, 21, 0, 0));
	rig.take(); // the evening of the 20th
	rig.run_until(cest(2026, 6, 21, 23, 59));
	Firings got = rig.take();
	INFO(describe(got));
	REQUIRE(got.size() == 4);
	CHECK(got[0] == std::make_pair(std::string("window"), june21.at(SunEvent::sunrise)));
	CHECK(got[1] == std::make_pair(std::string("not-before"), cest(2026, 6, 21, 6, 30)));
	CHECK(got[2] == std::make_pair(std::string("dusk-30"), june21.at(SunEvent::sunset) - 1800));
	CHECK(got[3] == std::make_pair(std::string("not-after"), cest(2026, 6, 21, 21, 0)));
}

TEST_CASE("Scheduler.AstroWithoutLocationIsRefusedUntilOneIsSet") {
	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 6, 20, 12, 0));
	REQUIRE(rig.load({astro("eve", SunEvent::sunset, 0), weekly("daily", Weekdays::kAll, at(7, 0))}) ==
		ScheduleStatus::ok);
	CHECK(capture.sink.count("E hc.sched: schedule eve (astro sunset) refused: astronomy needs a location "
		"(settings location.lat, location.lon)") == 1);
	CHECK_FALSE(rig.scheduler->next_firing(0));
	CHECK(rig.scheduler->next_firing(1)); // the others run
	rig.scheduler->evaluate();
	CHECK(capture.sink.count("refused: astronomy needs a location") == 1); // logged once

	rig.time.has_location = true;
	rig.time.location.latitude = 48.137;
	rig.time.location.longitude = 11.575;
	rig.scheduler->time_changed(TimeEvents::kLocationChanged);
	CHECK(rig.scheduler->next_firing(0));
}

TEST_CASE("Scheduler.PolarDayAndNightHaveNoAstroFirings") {
	Rig rig;
	rig.time.has_location = true;
	rig.time.location.latitude = 69.65; // Tromsø
	rig.time.location.longitude = 18.96;
	rig.time.set_utc(cest(2026, 6, 15, 12, 0));
	REQUIRE(rig.load({astro("set", SunEvent::sunset, 0), astro("dusk", SunEvent::dusk, 0)}) == ScheduleStatus::ok);
	CHECK_FALSE(rig.scheduler->next_firing(0));
	CHECK_FALSE(rig.scheduler->next_firing(1));
	rig.run_until(cest(2026, 6, 20, 12, 0));
	CHECK(rig.take().empty());
	// In autumn the sun sets again.
	CHECK(rig.scheduler->next_after(0, cest(2026, 9, 1, 0, 0)));
	// In the polar night there is civil twilight, but no sunset.
	CHECK_FALSE(rig.scheduler->next_after(0, cet(2026, 12, 18, 0, 0)));
	CHECK(rig.scheduler->next_after(1, cet(2026, 12, 18, 0, 0)));
}

TEST_CASE("Scheduler.SpringForwardFiresInTheGapOnceAtTheEndOfTheGap") {
	Rig rig;
	rig.time.set_utc(cet(2026, 3, 27, 12, 0));
	REQUIRE(rig.load({weekly("half-past-two", Weekdays::kAll, at(2, 30)), weekly("three", Weekdays::kAll, at(3, 0)),
		interval("quarter", Weekdays::kAll, 15, at(1, 30), at(3, 15))}) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 3, 30, 12, 0));
	Firings got = rig.take();
	Firings half;
	Firings three;
	Firings quarter;
	for (const auto& f : got) {
		(f.first == "half-past-two" ? half : f.first == "three" ? three : quarter).push_back(f);
	}
	CHECK(half == Firings{{"half-past-two", cet(2026, 3, 28, 2, 30)}, {"half-past-two", cest(2026, 3, 29, 3, 0)},
		{"half-past-two", cest(2026, 3, 30, 2, 30)}});
	CHECK(three == Firings{{"three", cet(2026, 3, 28, 3, 0)}, {"three", cest(2026, 3, 29, 3, 0)},
		{"three", cest(2026, 3, 30, 3, 0)}});
	// On the 29th: 01:30, 01:45, then 02:00..02:45 collapse into 03:00, then 03:15.
	Firings sunday;
	for (const auto& f : quarter) {
		if (f.second >= cet(2026, 3, 29, 0, 0) && f.second < cest(2026, 3, 30, 0, 0)) {
			sunday.push_back(f);
		}
	}
	CHECK(sunday == Firings{{"quarter", cet(2026, 3, 29, 1, 30)}, {"quarter", cet(2026, 3, 29, 1, 45)},
		{"quarter", cest(2026, 3, 29, 3, 0)}, {"quarter", cest(2026, 3, 29, 3, 15)}});
}

TEST_CASE("Scheduler.FallBackFiresOnceNotTwice") {
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 24, 12, 0));
	REQUIRE(rig.load({weekly("half-past-two", Weekdays::kAll, at(2, 30)),
		interval("half-hourly", Weekdays::kAll, 30, at(2, 0), at(3, 0))}) == ScheduleStatus::ok);
	rig.run_until(cet(2026, 10, 26, 12, 0));
	Firings got = rig.take();
	Firings expected = {{"half-hourly", cest(2026, 10, 25, 2, 0)}, {"half-past-two", cest(2026, 10, 25, 2, 30)},
		{"half-hourly", cest(2026, 10, 25, 2, 30)}, {"half-hourly", cet(2026, 10, 25, 3, 0)},
		{"half-hourly", cet(2026, 10, 26, 2, 0)}, {"half-past-two", cet(2026, 10, 26, 2, 30)},
		{"half-hourly", cet(2026, 10, 26, 2, 30)}, {"half-hourly", cet(2026, 10, 26, 3, 0)}};
	INFO(describe(got));
	CHECK(got == expected);
}

TEST_CASE("Scheduler.NextFiringAcrossMonthAndYearEnds") {
	Rig rig;
	rig.time.set_utc(cet(2026, 12, 1, 12, 0));
	const Date list[] = {{2027, 3, 1}, {2026, 12, 31}};
	REQUIRE(rig.load({weekly("monday", Weekdays::kMonday, at(8, 0)), yearly("newyear", 1, 1, at(0, 0)),
		yearly("leap", 2, 29, at(12, 0)), dates("list", list, 2, at(18, 0)),
		interval("workday", kWorkdays, 60, at(8, 0), at(17, 0)), yearly("silvester", 12, 31, at(23, 59, 59))}) ==
		ScheduleStatus::ok);
	Scheduler& s = *rig.scheduler;
	CHECK(s.next_after(0, cet(2026, 12, 30, 12, 0)) == cet(2027, 1, 4, 8, 0));
	CHECK(s.next_after(0, cest(2026, 10, 27, 12, 0)) == cet(2026, 11, 2, 8, 0)); // across a month end
	CHECK(s.next_after(1, cet(2026, 12, 31, 23, 59, 59)) == cet(2027, 1, 1, 0, 0));
	CHECK(s.next_after(1, cet(2027, 1, 1, 0, 0)) == cet(2028, 1, 1, 0, 0));
	CHECK(s.next_after(2, cet(2026, 3, 1, 0, 0)) == cet(2028, 2, 29, 12, 0));
	CHECK(s.next_after(2, cet(2028, 2, 29, 12, 0)) == cet(2032, 2, 29, 12, 0));
	CHECK(s.next_after(3, cet(2026, 12, 1, 0, 0)) == cet(2026, 12, 31, 18, 0));
	CHECK(s.next_after(3, cet(2026, 12, 31, 18, 0)) == cet(2027, 3, 1, 18, 0));
	CHECK_FALSE(s.next_after(3, cet(2027, 3, 1, 18, 0)));
	CHECK(s.next_after(4, cet(2026, 10, 30, 17, 0)) == cet(2026, 11, 2, 8, 0)); // Friday 17:00 -> Monday
	CHECK(s.next_after(4, cet(2026, 12, 31, 16, 30)) == cet(2026, 12, 31, 17, 0));
	CHECK(s.next_after(4, cet(2026, 12, 31, 17, 0)) == cet(2027, 1, 1, 8, 0));
	CHECK(s.next_after(5, cet(2026, 12, 31, 23, 59, 58)) == cet(2026, 12, 31, 23, 59, 59));
	CHECK(s.next_after(5, cet(2026, 12, 31, 23, 59, 59)) == cet(2027, 12, 31, 23, 59, 59));
	// What the scheduler has armed now.
	CHECK(s.next_firing(0) == cet(2026, 12, 7, 8, 0));
	CHECK(s.next_firing(2) == cet(2028, 2, 29, 12, 0));
}

TEST_CASE("Scheduler.CatchUpVersusSkipAcrossAReboot") {
	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 6, 0));
	ScheduleSpec skip = weekly("skip", Weekdays::kAll, at(7, 0));
	ScheduleSpec catch_up = weekly("catch", Weekdays::kAll, at(7, 0));
	catch_up.missed = MissedPolicy::catch_up;
	std::vector<ScheduleSpec> specs = {skip, catch_up};
	REQUIRE(rig.load(specs) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 13, 6, 0));
	CHECK(rig.take() == Firings{{"skip", cest(2026, 10, 12, 7, 0)}, {"catch", cest(2026, 10, 12, 7, 0)}});

	// Power off from 06:00 to 09:30 on the 13th: the 07:00 firing falls into the gap.
	rig.scheduler.reset();
	rig.time.mono_ms += 3600 * 1000; // a new boot has its own monotonic base; the wall clock moves on
	rig.time.set_utc(cest(2026, 10, 13, 9, 30));
	rig.boot();
	REQUIRE(rig.load(specs) == ScheduleStatus::ok);
	CHECK(rig.take().empty()); // nothing fires from within load()
	rig.run_until(cest(2026, 10, 13, 9, 31));
	CHECK(rig.take() == Firings{{"catch", cest(2026, 10, 13, 9, 30)}});
	CHECK(capture.sink.contains("W hc.sched: schedule skip missed its firing of 2026-10-13 07:00:00 CEST "
		"(UTC+02:00); skipped"));
	CHECK(capture.sink.contains("W hc.sched: schedule catch missed its firing of 2026-10-13 07:00:00 CEST "
		"(UTC+02:00); catching up once"));
	CHECK(capture.sink.contains("I hc.sched: schedule catch fired (catching up the firing of 2026-10-13 07:00:00 "
		"CEST (UTC+02:00))"));

	// Another reboot soon after: the gap is accounted for, nothing is repeated.
	rig.boot();
	REQUIRE(rig.load(specs) == ScheduleStatus::ok);
	size_t misses = capture.sink.count("missed its firing");
	rig.run_until(cest(2026, 10, 14, 6, 59));
	CHECK(rig.take().empty());
	CHECK(capture.sink.count("missed its firing") == misses);
	rig.run_until(cest(2026, 10, 14, 7, 1));
	CHECK(rig.take() == Firings{{"skip", cest(2026, 10, 14, 7, 0)}, {"catch", cest(2026, 10, 14, 7, 0)}});
	CHECK(rig.scheduler->last_fired(0) == cest(2026, 10, 14, 7, 0));

	// A long outage misses several firings: one catch-up, one log line naming the range.
	rig.scheduler.reset();
	rig.time.set_utc(cest(2026, 10, 17, 12, 0));
	rig.boot();
	REQUIRE(rig.load(specs) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 17, 12, 1));
	CHECK(rig.take() == Firings{{"catch", cest(2026, 10, 17, 12, 0)}});
	CHECK(capture.sink.contains("W hc.sched: schedule skip missed its firings from 2026-10-15 07:00:00 CEST "
		"(UTC+02:00) to 2026-10-17 07:00:00 CEST (UTC+02:00); skipped"));
}

TEST_CASE("Scheduler.ClockJumpsForwardAndBack") {
	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 6, 0));
	ScheduleSpec skip = weekly("skip", Weekdays::kAll, at(7, 0));
	ScheduleSpec catch_up = weekly("catch", Weekdays::kAll, at(7, 30));
	catch_up.missed = MissedPolicy::catch_up;
	REQUIRE(rig.load({skip, catch_up}) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 12, 6, 30));
	// SNTP steps the clock 2 hours ahead.
	rig.time.set_utc(cest(2026, 10, 12, 8, 30));
	rig.scheduler->time_changed(TimeEvents::kClockJumped);
	rig.run_until(cest(2026, 10, 12, 8, 31));
	CHECK(rig.take() == Firings{{"catch", cest(2026, 10, 12, 8, 30)}});
	CHECK(capture.sink.contains("schedule skip missed its firing of 2026-10-12 07:00:00 CEST (UTC+02:00); skipped"));

	// Now back by 2 hours: 07:00 and 07:30 do not fire a second time today.
	rig.time.set_utc(cest(2026, 10, 12, 6, 31));
	rig.scheduler->time_changed(TimeEvents::kClockJumped);
	rig.run_until(cest(2026, 10, 12, 23, 0));
	CHECK(rig.take().empty());
	rig.run_until(cest(2026, 10, 13, 8, 0));
	CHECK(rig.take() == Firings{{"skip", cest(2026, 10, 13, 7, 0)}, {"catch", cest(2026, 10, 13, 7, 30)}});
}

TEST_CASE("Scheduler.WaitsForASynchronizedClock") {
	ScopedCapture capture;
	Rig rig;
	rig.time.synchronized = false;
	rig.time.set_utc(1000); // 1970: the chip's idea of time before SNTP
	REQUIRE(rig.load({weekly("daily", Weekdays::kAll, at(7, 0))}) == ScheduleStatus::ok);
	CHECK(capture.sink.contains("I hc.sched: schedules wait for a synchronized wall clock"));
	CHECK_FALSE(rig.scheduler->next_firing(0));
	CHECK(rig.loop.timer_count() == 0);
	rig.run_until(200000);
	CHECK(rig.take().empty());
	// The first SNTP fix: a new entry starts from now, nothing counts as missed.
	rig.time.synchronized = true;
	rig.time.set_utc(cest(2026, 10, 12, 9, 0));
	rig.scheduler->time_changed(TimeEvents::kSynchronized);
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 13, 7, 0));
	rig.run_until(cest(2026, 10, 13, 7, 0));
	CHECK(rig.take() == Firings{{"daily", cest(2026, 10, 13, 7, 0)}});
	CHECK(capture.sink.count("missed") == 0);
	// Losing synchronization stops the timer.
	rig.time.synchronized = false;
	rig.scheduler->time_changed(TimeEvents::kSynchronizationLost);
	CHECK(rig.loop.timer_count() == 0);
}

TEST_CASE("Scheduler.FiredTodayOutputDatapoint") {
	ScopedCapture capture;
	Rig rig;
	const hc::DatapointSpec datapoints[] = {
		{"evening.fired", hc::ValueType::boolean, 0},
		{"number", hc::ValueType::real, 0},
	};
	REQUIRE(rig.model.load(datapoints, 2) == hc::DatapointStatus::ok);
	rig.time.set_utc(cest(2026, 10, 12, 12, 0));
	ScheduleSpec spec = weekly("evening", Weekdays::kAll, at(19, 0));
	spec.output = "evening.fired";
	REQUIRE(rig.load({spec}) == ScheduleStatus::ok);
	const hc::Datapoint* dp = rig.model.find("evening.fired");
	CHECK(dp->technology() == rig.scheduler.get());
	CHECK(dp->value() == hc::Value::of_bool(false));
	rig.run_until(cest(2026, 10, 12, 19, 0, 30));
	CHECK(dp->value() == hc::Value::of_bool(true));
	rig.run_until(cest(2026, 10, 12, 23, 59, 59));
	CHECK(dp->value() == hc::Value::of_bool(true));
	rig.run_until(cest(2026, 10, 13, 0, 0, 5)); // reset at local midnight
	CHECK(dp->value() == hc::Value::of_bool(false));

	// A command is refused: the scheduler owns the output.
	CHECK(rig.model.command(dp->id(), hc::Value::of_bool(true)) != hc::DatapointStatus::ok);

	// After a reboot on the day of a firing, the output says so again.
	rig.run_until(cest(2026, 10, 13, 20, 0));
	rig.boot();
	REQUIRE(rig.model.load(datapoints, 2) == hc::DatapointStatus::ok);
	REQUIRE(rig.load({spec}) == ScheduleStatus::ok);
	CHECK(rig.model.find("evening.fired")->value() == hc::Value::of_bool(true));

	// Refusals: a missing and a non-boolean output.
	spec.output = "nothing";
	CHECK(rig.load({spec}) == ScheduleStatus::unknown_reference);
	CHECK(capture.sink.contains("E hc.sched: refusing schedules: entry evening: output datapoint 'nothing' does not exist"));
	spec.output = "number";
	CHECK(rig.load({spec}) == ScheduleStatus::type_error);
	ScheduleSpec twin = weekly("twin", Weekdays::kAll, at(6, 0));
	twin.output = "evening.fired";
	spec.output = "evening.fired";
	CHECK(rig.load({spec, twin}) == ScheduleStatus::output_conflict);
	CHECK(rig.scheduler->size() == 1); // the previous entries stay
}

TEST_CASE("Scheduler.EnableSwitchIsPersistedAndRespectsTheImage") {
	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 6, 0));
	ScheduleSpec adjustable = weekly("wake", Weekdays::kAll, at(7, 0));
	ScheduleSpec fixed = weekly("fixed", Weekdays::kAll, at(7, 0));
	fixed.enabled_adjustable = false;
	std::vector<ScheduleSpec> specs = {adjustable, fixed};
	REQUIRE(rig.load(specs) == ScheduleStatus::ok);
	const char* error = nullptr;
	CHECK_FALSE(rig.scheduler->set_enabled("fixed", false, &error));
	CHECK(std::string(error) == "the image fixes this schedule's enable switch");
	CHECK_FALSE(rig.scheduler->set_enabled("nope", false, &error));
	CHECK(std::string(error) == "no such schedule");
	CHECK(rig.scheduler->set_enabled("wake", false));
	CHECK_FALSE(rig.scheduler->next_firing(0));
	rig.run_until(cest(2026, 10, 12, 8, 0));
	CHECK(rig.take() == Firings{{"fixed", cest(2026, 10, 12, 7, 0)}});

	// The override survives a reboot.
	rig.boot();
	REQUIRE(rig.load(specs) == ScheduleStatus::ok);
	CHECK_FALSE(rig.scheduler->enabled(0));
	// Switching on again: what passed while it was off is not missed.
	rig.run_until(cest(2026, 10, 14, 12, 0));
	rig.take();
	CHECK(rig.scheduler->set_enabled("wake", true));
	rig.run_until(cest(2026, 10, 14, 12, 1));
	CHECK(rig.take().empty());
	CHECK(capture.sink.count("schedule wake missed") == 0);
	CHECK(rig.scheduler->set_enabled("wake", false));

	// A new image that fixes the switch drops the override.
	specs[0].enabled_adjustable = false;
	rig.boot();
	REQUIRE(rig.load(specs) == ScheduleStatus::ok);
	CHECK(rig.scheduler->enabled(0));
	CHECK(capture.sink.contains("W hc.sched: schedule wake: enable override removed: the image fixes the switch"));
}

TEST_CASE("Scheduler.RecordKeysAndOrphans") {
	char key[Scheduler::kRecordKeySize];
	Scheduler::record_key("a-schedule-id-of-the-full-length", key);
	CHECK(std::strlen(key) == 12);
	CHECK(std::strlen(key) <= hc::Settings::kMaxKeyLength);
	CHECK(std::string(key).substr(0, 4) == "sch.");
	CHECK(hc::Settings::check_key(key) == hc::SettingsStatus::ok);
	Scheduler::record_key("", key);
	CHECK(std::string(key) == "sch.811c9dc5"); // FNV-1a offset basis

	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 6, 0));
	REQUIRE(rig.load({weekly("one", Weekdays::kAll, at(7, 0)), weekly("two", Weekdays::kAll, at(7, 0))}) ==
		ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 12, 7, 1));
	char one_key[Scheduler::kRecordKeySize];
	Scheduler::record_key("one", one_key);
	char value[128];
	REQUIRE(rig.settings.get_string(one_key, value, sizeof(value)) == hc::SettingsStatus::ok);
	CHECK(std::string(value) == "1;" + std::to_string(cest(2026, 10, 12, 7, 0)) + ";" +
		std::to_string(cest(2026, 10, 12, 7, 0)) + ";;one");
	rig.settings.set_string("wifi.ssid", "home");
	// A new image without "two": its record goes, other settings stay.
	REQUIRE(rig.load({weekly("one", Weekdays::kAll, at(7, 0))}) == ScheduleStatus::ok);
	size_t records = 0;
	for (const auto& entry : rig.settings.entries) {
		records += entry.first.compare(0, 4, "sch.") == 0;
	}
	CHECK(records == 1);
	CHECK(rig.settings.entries.count("wifi.ssid") == 1);

	// A record of another id under the key (a hash collision or garbage) is not trusted.
	rig.settings.set_string(one_key, "1;5;5;;other");
	rig.boot();
	REQUIRE(rig.load({weekly("one", Weekdays::kAll, at(7, 0))}) == ScheduleStatus::ok);
	CHECK(capture.sink.contains("schedule one: record " + std::string(one_key) + " belongs to schedule other"));
	CHECK_FALSE(rig.scheduler->last_fired(0));
	rig.settings.set_string(one_key, "9;x");
	rig.boot();
	REQUIRE(rig.load({weekly("one", Weekdays::kAll, at(7, 0))}) == ScheduleStatus::ok);
	CHECK(capture.sink.contains("schedule one: record " + std::string(one_key) + " unreadable (format)"));

	// A failing store is logged, the schedule still runs.
	rig.take();
	rig.settings.fail_writes = true;
	rig.run_until(cest(2026, 10, 13, 7, 1));
	CHECK(rig.take().size() == 1);
	CHECK(capture.sink.contains("E hc.sched: schedule one: cannot store record"));
}

TEST_CASE("Scheduler.LoadRefusalsKeepThePreviousEntries") {
	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 6, 0));
	REQUIRE(rig.load({weekly("keep", Weekdays::kAll, at(7, 0))}) == ScheduleStatus::ok);
	struct Case {
		ScheduleSpec spec;
		ScheduleStatus status;
		const char* log;
	};
	ScheduleSpec bad_id = weekly("no spaces", Weekdays::kAll, at(7, 0));
	ScheduleSpec no_label = weekly("x", Weekdays::kAll, at(7, 0));
	no_label.label = "";
	ScheduleSpec no_days = weekly("x", 0, at(7, 0));
	ScheduleSpec bad_time = weekly("x", Weekdays::kAll, at(24, 0));
	ScheduleSpec bad_day = yearly("x", 2, 30, at(7, 0));
	ScheduleSpec inverted = interval("x", Weekdays::kAll, 10, at(9, 0), at(8, 0));
	ScheduleSpec zero_every = interval("x", Weekdays::kAll, 0, at(8, 0), at(9, 0));
	ScheduleSpec clamp = astro("x", SunEvent::sunset, 0);
	clamp.has_not_before = true;
	clamp.not_before = at(22, 0);
	clamp.has_not_after = true;
	clamp.not_after = at(21, 0);
	ScheduleSpec far = astro("x", SunEvent::sunset, 13 * 3600);
	const Date twice[] = {{2026, 12, 24}, {2026, 12, 24}};
	ScheduleSpec duplicate_date = dates("x", twice, 2, at(7, 0));
	const Date invalid[] = {{2026, 2, 29}};
	ScheduleSpec invalid_date = dates("x", invalid, 1, at(7, 0));
	ScheduleSpec bad_once = once("x", Date{2026, 13, 1}, at(7, 0));
	const Case cases[] = {
		{bad_id, ScheduleStatus::malformed, "id must be 1..32 characters"},
		{no_label, ScheduleStatus::malformed, "label must be 1..63 printable bytes"},
		{no_days, ScheduleStatus::malformed, "days must name 1..7 days of the week"},
		{bad_time, ScheduleStatus::malformed, "time out of range"},
		{bad_day, ScheduleStatus::malformed, "month/day is not a date"},
		{inverted, ScheduleStatus::malformed, "until is earlier than from"},
		{zero_every, ScheduleStatus::malformed, "every must be 1 to 720 minutes"},
		{clamp, ScheduleStatus::malformed, "notBefore is later than notAfter"},
		{far, ScheduleStatus::malformed, "offset beyond 12 h"},
		{duplicate_date, ScheduleStatus::duplicate_id, "a date is listed twice"},
		{invalid_date, ScheduleStatus::malformed, "a date is not a valid date"},
		{bad_once, ScheduleStatus::malformed, "at is not a valid date and time"},
	};
	for (const Case& c : cases) {
		CHECK(rig.load({c.spec}) == c.status);
		CHECK(capture.sink.contains(c.log));
	}
	CHECK(rig.load({weekly("a", 1, at(7, 0)), weekly("a", 2, at(7, 0))}) == ScheduleStatus::duplicate_id);
	CHECK(capture.sink.contains("E hc.sched: refusing schedules: entry 1: id a used twice"));
	std::vector<ScheduleSpec> many(Scheduler::kMaxEntries + 1, weekly("x", 1, at(7, 0)));
	CHECK(rig.load(many) == ScheduleStatus::limit_exceeded);
	std::vector<Date> lots;
	for (int i = 0; i < 129; ++i) {
		lots.push_back(hc::date_from_days(hc::days_from_civil(2027, 1, 1) + i));
	}
	CHECK(rig.load({dates("x", lots.data(), lots.size(), at(7, 0))}) == ScheduleStatus::limit_exceeded);
	REQUIRE(rig.scheduler->size() == 1);
	CHECK(std::string(rig.scheduler->id(0)) == "keep");
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 12, 7, 0));
	CHECK(std::string(hc::schedule_status_name(ScheduleStatus::output_conflict)) == "outputConflict");
	CHECK(std::string(hc::schedule_kind_name(ScheduleKind::interval)) == "interval");
	CHECK(std::string(hc::missed_policy_name(MissedPolicy::catch_up)) == "catchUp");
}

TEST_CASE("Scheduler.ListenersAndFiringOrder") {
	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 6, 0));
	// Two entries due at the same time fire in image order; the earlier one first.
	REQUIRE(rig.load({weekly("b", Weekdays::kAll, at(7, 0)), weekly("a", Weekdays::kAll, at(7, 0)),
		weekly("early", Weekdays::kAll, at(6, 59))}) == ScheduleStatus::ok);
	struct Switcher : hc::ScheduleListener {
		Scheduler* scheduler = nullptr;
		std::vector<std::string> seen;
		bool loaded = true;
		void schedule_fired(const char* id) override {
			seen.push_back(id);
			if (std::string(id) == "b") {
				scheduler->set_enabled("a", false); // takes effect at once: "a" does not fire
				loaded = scheduler->load(nullptr, 0) == ScheduleStatus::ok;
			}
		}
	};
	Switcher switcher;
	switcher.scheduler = rig.scheduler.get();
	CHECK(rig.scheduler->add_listener(switcher));
	CHECK_FALSE(rig.scheduler->add_listener(switcher));
	rig.run_until(cest(2026, 10, 12, 7, 1));
	CHECK(switcher.seen == std::vector<std::string>{"early", "b"});
	CHECK_FALSE(switcher.loaded);
	CHECK(capture.sink.contains("E hc.sched: refusing schedules: load from within a firing"));
	CHECK(rig.scheduler->remove_listener(switcher));
	CHECK_FALSE(rig.scheduler->remove_listener(switcher));
}

TEST_CASE("Scheduler.AWeekAcrossTheOctoberDstChangeInSeconds") {
	// The real TimeService over a fake platform clock and the EU zone: the full wiring, with the
	// service's watch delivering day changes and the scheduler's single timer.
	struct EuZone : hc::TimeZone {
		FakeTimeSource source;
		void apply(const char*) override {}
		bool to_local(int64_t t, LocalTime& out) const override { return source.to_local(t, out); }
	};
	EuZone zone;
	hc_test::FakeClock clock;
	FakeSettings settings;
	settings.set_string("location.lat", "48.137");
	settings.set_string("location.lon", "11.575");
	hc::EventLoop loop;
	hc::TimeService time(loop, clock, zone, settings);
	hc::DatapointModel model(clock);
	const hc::DatapointSpec datapoints[] = {{"night", hc::ValueType::boolean, 0}};
	REQUIRE(model.load(datapoints, 1) == hc::DatapointStatus::ok);
	Scheduler scheduler(loop, time, settings, &model);
	struct Log : hc::ScheduleListener {
		const hc_test::FakeClock* clock;
		Firings firings;
		void schedule_fired(const char* id) override { firings.emplace_back(id, clock->wall_seconds); }
	} log;
	log.clock = &clock;
	scheduler.add_listener(log);
	time.add_listener(scheduler);
	clock.synchronized = true;
	clock.wall_seconds = cest(2026, 10, 19, 0, 0); // Monday
	time.start();

	const Date list[] = {{2026, 10, 23}, {2026, 10, 30}};
	ScheduleSpec night = weekly("night", Weekdays::kAll, at(22, 0));
	night.output = "night";
	ScheduleSpec late = weekly("late", Weekdays::kAll, at(2, 30));
	ScheduleSpec dusk = astro("dusk", SunEvent::sunset, -15 * 60);
	dusk.has_not_after = true;
	dusk.not_after = at(18, 0);
	std::vector<ScheduleSpec> specs = {night, late, interval("pump", kWorkdays, 30, at(8, 0), at(9, 0)), dusk,
		yearly("birthday", 10, 21, at(12, 0)), dates("trash", list, 2, at(6, 15)),
		once("party", Date{2026, 10, 24}, at(20, 15, 30))};
	REQUIRE(scheduler.load(specs.data(), specs.size()) == ScheduleStatus::ok);

	Firings expected;
	for (int day = 19; day <= 25; ++day) {
		bool winter = day >= 25; // 25 October 2026 03:00 CEST -> 02:00 CET
		auto local = [&](int hour, int minute, int second = 0) {
			return winter && !(day == 25 && hour < 2) ? cet(2026, 10, day, hour, minute, second)
				: cest(2026, 10, day, hour, minute, second);
		};
		expected.emplace_back("late", day == 25 ? cest(2026, 10, 25, 2, 30) : local(2, 30));
		if (day == 23) {
			expected.emplace_back("trash", local(6, 15));
		}
		if (day <= 23) {
			expected.emplace_back("pump", local(8, 0)); // the slots are 08:00, 08:30, 09:00
			expected.emplace_back("pump", local(8, 30));
			expected.emplace_back("pump", local(9, 0));
		}
		if (day == 21) {
			expected.emplace_back("birthday", local(12, 0));
		}
		hc::GeoLocation munich;
		munich.latitude = 48.137;
		munich.longitude = 11.575;
		int64_t sunset = hc::sun_times(2026, 10, static_cast<unsigned>(day), munich).at(SunEvent::sunset) - 900;
		int64_t clamp = local(18, 0);
		expected.emplace_back("dusk", sunset < clamp ? sunset : clamp);
		if (day == 24) {
			expected.emplace_back("party", local(20, 15, 30));
		}
		expected.emplace_back("night", local(22, 0));
	}
	// Drive the loop like the platform, a week of wall time.
	int64_t end = cet(2026, 10, 26, 0, 0, 2);
	int night_true = 0;
	bool night_value = false;
	for (;;) {
		loop.run_once(clock.now_ms);
		bool value = model.find("night")->value().as_bool();
		if (value && !night_value) {
			++night_true;
		}
		night_value = value;
		if (clock.wall_seconds >= end) {
			break;
		}
		uint64_t deadline = loop.next_deadline();
		if (deadline > clock.now_ms) {
			uint64_t step = deadline - clock.now_ms;
			clock.now_ms += step;
			clock.wall_seconds += static_cast<int64_t>(step / 1000);
		}
	}
	// In the order of time (no two entries share a firing time here).
	std::stable_sort(expected.begin(), expected.end(),
		[](const auto& a, const auto& b) { return a.second < b.second; });
	Firings got = log.firings;
	INFO(describe(got));
	CHECK(got.size() == expected.size());
	CHECK(got == expected);
	CHECK(night_true == 7);       // set at 22:00 every day ...
	CHECK_FALSE(night_value);     // ... and reset at midnight
}

TEST_CASE("Scheduler.IdleTimeIsNoGap") {
	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 6, 0));
	// Off by the image's default, with catch-up: days pass, the controller reboots, a new image
	// switches it on. Nothing that passed while it was off is caught up.
	ScheduleSpec wake = weekly("wake", Weekdays::kAll, at(7, 0));
	wake.missed = MissedPolicy::catch_up;
	wake.enabled = false;
	REQUIRE(rig.load({wake}) == ScheduleStatus::ok);
	char key[Scheduler::kRecordKeySize];
	Scheduler::record_key("wake", key);
	char value[128];
	REQUIRE(rig.settings.get_string(key, value, sizeof(value)) == hc::SettingsStatus::ok);
	CHECK(std::string(value) == "1;;" + std::to_string(cest(2026, 10, 12, 6, 0) - 1) + ";i;wake");
	rig.run_until(cest(2026, 10, 14, 12, 0));
	CHECK(rig.take().empty());
	rig.scheduler.reset();
	rig.time.set_utc(cest(2026, 10, 15, 12, 0));
	rig.boot();
	wake.enabled = true;
	REQUIRE(rig.load({wake}) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 15, 12, 1));
	CHECK(rig.take().empty());
	CHECK(capture.sink.count("missed") == 0);
	rig.run_until(cest(2026, 10, 16, 7, 1));
	CHECK(rig.take() == Firings{{"wake", cest(2026, 10, 16, 7, 0)}});

	// Switched off at run time, then an image swap switches it on again: no gap either.
	CHECK(rig.scheduler->set_enabled("wake", false));
	rig.run_until(cest(2026, 10, 18, 12, 0));
	wake.enabled = true;
	REQUIRE(rig.load({wake}) == ScheduleStatus::ok);
	CHECK_FALSE(rig.scheduler->enabled(0)); // the override stays
	CHECK(rig.scheduler->set_enabled("wake", true));
	rig.run_until(cest(2026, 10, 18, 12, 1));
	CHECK(rig.take().empty());
	CHECK(capture.sink.count("missed") == 0);
}

TEST_CASE("Scheduler.AstroWithoutLocationMissesNothing") {
	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 6, 20, 12, 0));
	ScheduleSpec eve = astro("eve", SunEvent::sunset, 0);
	eve.missed = MissedPolicy::catch_up;
	REQUIRE(rig.load({eve}) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 6, 22, 23, 0));
	// A reboot while still without location, then the location arrives.
	rig.boot();
	REQUIRE(rig.load({eve}) == ScheduleStatus::ok);
	rig.time.has_location = true;
	rig.time.location.latitude = 48.137;
	rig.time.location.longitude = 11.575;
	rig.scheduler->time_changed(TimeEvents::kLocationChanged);
	rig.run_until(cest(2026, 6, 22, 23, 1));
	CHECK(rig.take().empty());
	CHECK(capture.sink.count("missed") == 0);
	rig.run_until(cest(2026, 6, 23, 23, 0));
	Firings got = rig.take();
	REQUIRE(got.size() == 1);
	CHECK(got[0].second == hc::sun_times(2026, 6, 23, rig.time.location).at(SunEvent::sunset));
}

TEST_CASE("Scheduler.CatchUpAfterARebootThatWaitsForSntp") {
	// The device boots with an unsynchronized clock (the usual case); the gap is found when SNTP
	// delivers.
	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 6, 0));
	ScheduleSpec heating = weekly("heating", Weekdays::kAll, at(5, 0));
	heating.missed = MissedPolicy::catch_up;
	ScheduleSpec yearly_entry = yearly("anniversary", 10, 13, at(9, 0));
	yearly_entry.missed = MissedPolicy::catch_up;
	std::vector<ScheduleSpec> specs = {heating, yearly_entry};
	REQUIRE(rig.load(specs) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 12, 6, 1));
	rig.scheduler.reset();
	rig.time.synchronized = false;
	rig.time.set_utc(0);
	rig.boot();
	REQUIRE(rig.load(specs) == ScheduleStatus::ok);
	rig.run_until(30);
	CHECK(rig.take().empty());
	rig.time.synchronized = true;
	rig.time.set_utc(cest(2026, 10, 13, 10, 0));
	rig.scheduler->time_changed(TimeEvents::kSynchronized);
	rig.run_until(cest(2026, 10, 13, 10, 1));
	CHECK(rig.take() == Firings{{"heating", cest(2026, 10, 13, 10, 0)}, {"anniversary", cest(2026, 10, 13, 10, 0)}});
	CHECK(capture.sink.contains("schedule anniversary missed its firing of 2026-10-13 09:00:00 CEST (UTC+02:00); "
		"catching up once"));
	CHECK(rig.scheduler->next_firing(1) == cest(2027, 10, 13, 9, 0));
}
