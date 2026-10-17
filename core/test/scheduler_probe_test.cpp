/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the scheduler (issues #9 and #10): entries sharing a
// minute, disabling and re-enabling without a catch-up, one-shot entries in the past, an astro
// offset across midnight, a weekly entry across a leap-year end, and the entry limit.

#include <string>
#include <vector>

#include "doctest.h"
#include "scheduler_test_support.h"

TEST_CASE("SchedulerProbe.SharedMinuteFiresInImageOrderAndDisablingLeavesNoGap") {
	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 8, 0)); // Monday
	REQUIRE(rig.load({interval("every2h", Weekdays::kAll, 120, at(8, 0), at(20, 0)), weekly("noon", Weekdays::kAll, at(12, 0))})
		== ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 12, 12, 1));
	Firings f = rig.take();
	REQUIRE(f.size() == 4); // 08:00 10:00 12:00 of the interval, 12:00 of the weekly
	CHECK(f[2] == std::make_pair(std::string("every2h"), cest(2026, 10, 12, 12, 0)));
	CHECK(f[3] == std::make_pair(std::string("noon"), cest(2026, 10, 12, 12, 0))); // image order on the tie
	// Switched off over the afternoon, on again at 17:00: the 14:00 and 16:00 slots are no gap.
	REQUIRE(rig.scheduler->set_enabled("every2h", false));
	rig.run_until(cest(2026, 10, 12, 17, 0));
	CHECK(rig.take().empty());
	size_t misses = capture.sink.count("missed");
	REQUIRE(rig.scheduler->set_enabled("every2h", true));
	rig.run_until(cest(2026, 10, 12, 20, 1));
	CHECK(rig.take() == Firings{{"every2h", cest(2026, 10, 12, 18, 0)}, {"every2h", cest(2026, 10, 12, 20, 0)}});
	CHECK(capture.sink.count("missed") == misses);
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 13, 8, 0));
}

TEST_CASE("SchedulerProbe.OneShotEntriesAlreadyPastWhenFirstLoadedNeverFire") {
	// A gap can only exist for an entry the scheduler has seen before: deploying an image at 08:00
	// with a one-shot entry of yesterday 09:00 does not fire it, catch-up policy or not. (A record
	// from an earlier load is what makes a missed firing; see Scheduler.CatchUpVersusSkipAcrossAReboot.)
	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 8, 0));
	ScheduleSpec late = once("late", Date{2026, 10, 11}, at(9, 0));
	late.missed = MissedPolicy::catch_up;
	ScheduleSpec gone = once("gone", Date{2026, 10, 11}, at(9, 0));
	std::vector<ScheduleSpec> specs = {late, gone};
	REQUIRE(rig.load(specs) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 12, 8, 1));
	CHECK(rig.take().empty());
	CHECK_FALSE(rig.scheduler->next_firing(0).has_value());
	CHECK_FALSE(rig.scheduler->next_firing(1).has_value());
	// Nor after a reboot.
	rig.scheduler.reset();
	rig.time.mono_ms += 60000;
	rig.boot();
	REQUIRE(rig.load(specs) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 13, 8, 0));
	CHECK(rig.take().empty());
	// A one-shot entry still ahead fires exactly once and then has no next firing.
	ScheduleSpec soon = once("soon", Date{2026, 10, 13}, at(9, 0));
	REQUIRE(rig.load({soon}) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 14, 9, 1));
	CHECK(rig.take() == Firings{{"soon", cest(2026, 10, 13, 9, 0)}});
	CHECK_FALSE(rig.scheduler->next_firing(0).has_value());
}

TEST_CASE("SchedulerProbe.AstroOffsetAcrossMidnightLandsOnTheNextLocalDate") {
	Rig rig;
	rig.time.has_location = true;
	rig.time.location = hc::GeoLocation{48.137, 11.575, 520}; // Munich
	rig.time.set_utc(cest(2026, 6, 20, 12, 0));
	REQUIRE(rig.load({astro("late", SunEvent::sunset, 8 * 3600)}) == ScheduleStatus::ok);
	std::optional<int64_t> next = rig.scheduler->next_firing(0);
	REQUIRE(next.has_value());
	LocalTime local;
	REQUIRE(rig.time.to_local(*next, local));
	// Sunset in Munich around 21:15 CEST in June; plus 8 h is about 05:15 the next day.
	CHECK(local.day == 21);
	CHECK(local.hour == 5);
	CHECK(local.minute >= 5);
	CHECK(local.minute <= 30);
	rig.run_until(*next + 1);
	CHECK(rig.take().size() == 1);
}

TEST_CASE("SchedulerProbe.WeeklyAtMidnightAcrossALeapYearEnd") {
	Rig rig;
	rig.time.set_utc(cet(2027, 12, 31, 23, 0));
	REQUIRE(rig.load({weekly("night", Weekdays::kAll, at(0, 0, 30))}) == ScheduleStatus::ok);
	rig.run_until(cet(2028, 1, 2, 1, 0));
	CHECK(rig.take() == Firings{{"night", cet(2028, 1, 1, 0, 0, 30)}, {"night", cet(2028, 1, 2, 0, 0, 30)}});
	CHECK(rig.scheduler->next_firing(0) == cet(2028, 1, 3, 0, 0, 30));
	// 29 February exists in 2028: a yearly entry on it has a next firing this year.
	REQUIRE(rig.load({yearly("leap", 2, 29, at(12, 0))}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->next_firing(0) == cet(2028, 2, 29, 12, 0));
}

TEST_CASE("SchedulerProbe.EntryLimitAndUniqueRecordKeys") {
	ScopedCapture capture;
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 8, 0));
	std::vector<std::string> ids;
	std::vector<ScheduleSpec> specs;
	for (size_t i = 0; i < Scheduler::kMaxEntries + 1; ++i) {
		ids.push_back("entry-" + std::to_string(i));
	}
	for (size_t i = 0; i < Scheduler::kMaxEntries + 1; ++i) {
		specs.push_back(weekly(ids[i].c_str(), Weekdays::kAll, at(static_cast<int>(i % 24), static_cast<int>(i % 60))));
	}
	CHECK(rig.load(specs) == ScheduleStatus::limit_exceeded);
	CHECK(rig.scheduler->size() == 0);
	specs.pop_back();
	REQUIRE(rig.load(specs) == ScheduleStatus::ok);
	CHECK(rig.scheduler->size() == Scheduler::kMaxEntries);
	std::vector<std::string> keys;
	for (size_t i = 0; i < Scheduler::kMaxEntries; ++i) {
		char key[Scheduler::kRecordKeySize];
		Scheduler::record_key(ids[i].c_str(), key);
		CHECK(std::string(key).size() <= 15); // the NVS key limit
		keys.push_back(key);
	}
	std::sort(keys.begin(), keys.end());
	CHECK(std::unique(keys.begin(), keys.end()) == keys.end());
}
