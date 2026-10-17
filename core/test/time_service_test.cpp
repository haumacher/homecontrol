/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstdio>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/event_loop.h"
#include "hc/time_service.h"
#include "hc/tz.h"
#include "test_support.h"

using hc::Date;
using hc::LocalTime;
using hc::TimeEvents;
using hc::TimeService;
using hc_test::FakeClock;
using hc_test::FakeSettings;
using hc_test::FakeTimeZone;
using hc_test::ScopedCapture;

namespace {

/// Central European time with the EU rules (01:00 UTC on the last Sundays of March and October).
class EuTimeZone : public hc::TimeZone {
public:
	void apply(const char*) override {}

	static int64_t last_sunday_1am(int year, unsigned month) {
		int64_t days = hc::days_from_civil(year, month, 31);
		days -= hc::weekday_from_days(days) % 7; // back to Sunday (7 % 7 == 0)
		return days * 86400 + 3600;
	}

	bool to_local(int64_t utc, LocalTime& out) const override {
		int year = hc::date_from_days(utc >= 0 ? utc / 86400 : (utc - 86399) / 86400).year;
		bool dst = utc >= last_sunday_1am(year, 3) && utc < last_sunday_1am(year, 10);
		int64_t local = utc + (dst ? 7200 : 3600);
		int64_t days = local >= 0 ? local / 86400 : (local - 86399) / 86400;
		int64_t rest = local - days * 86400;
		Date date = hc::date_from_days(days);
		out.year = date.year;
		out.month = date.month;
		out.day = date.day;
		out.hour = static_cast<uint8_t>(rest / 3600);
		out.minute = static_cast<uint8_t>(rest % 3600 / 60);
		out.second = static_cast<uint8_t>(rest % 60);
		out.dst = dst;
		std::snprintf(out.zone, sizeof(out.zone), "%s", dst ? "CEST" : "CET");
		return true;
	}
};

/// Records every notification.
class Recorder : public hc::TimeListener {
public:
	void time_changed(uint8_t events) override { calls.push_back(events); }

	std::vector<uint8_t> calls;
};

int64_t utc(int year, unsigned month, unsigned day, int hour, int minute, int second = 0) {
	return hc::days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

struct Rig {
	FakeClock clock;
	FakeTimeZone zone; // fixed UTC+2 unless changed
	FakeSettings settings;
	hc::EventLoop loop;
	TimeService time{loop, clock, zone, settings};

	/// Advances both clocks by `ms`, running the loop at each of its deadlines.
	void advance(uint64_t ms) {
		uint64_t until = clock.now_ms + ms;
		for (;;) {
			loop.run_once(clock.now_ms);
			uint64_t deadline = loop.next_deadline();
			if (deadline > until) {
				step_to(until);
				loop.run_once(clock.now_ms);
				return;
			}
			if (deadline > clock.now_ms) {
				step_to(deadline);
			}
		}
	}

	void step_to(uint64_t mono) {
		uint64_t before_s = clock.now_ms / 1000;
		clock.now_ms = mono;
		clock.wall_seconds += static_cast<int64_t>(mono / 1000 - before_s);
	}
};

} // namespace

TEST_CASE("TimeService.WatchesUntilSynchronizedThenStopsWithoutContinuousListeners") {
	ScopedCapture capture;
	Rig rig;
	Recorder sync_only;
	rig.time.add_listener(sync_only, TimeEvents::kSynchronized);
	rig.time.start();
	CHECK(rig.loop.timer_count() == 1);
	CHECK(rig.loop.next_deadline() == rig.clock.now_ms + TimeService::kWatchPeriodMs);
	CHECK(capture.sink.contains("I hc.time: astronomy unavailable: no location set (settings location.lat, location.lon)"));

	rig.advance(5000);
	CHECK(sync_only.calls.empty());
	rig.clock.wall_seconds = utc(2026, 10, 17, 8, 0);
	rig.clock.synchronized = true;
	rig.advance(1000);
	REQUIRE(sync_only.calls.size() == 1);
	CHECK(sync_only.calls[0] == TimeEvents::kSynchronized);
	CHECK(rig.loop.timer_count() == 0); // nobody needs the watch any more

	// A continuous listener restarts it.
	Recorder all;
	CHECK(rig.time.add_listener(all));
	CHECK(rig.loop.timer_count() == 1);
	CHECK(rig.time.remove_listener(all));
	CHECK(rig.loop.timer_count() == 0);
}

TEST_CASE("TimeService.SynchronizedAtStartNotifiesBeforeStartReturns") {
	Rig rig;
	rig.clock.synchronized = true;
	rig.clock.wall_seconds = utc(2026, 10, 17, 8, 0);
	Recorder recorder;
	rig.time.add_listener(recorder);
	rig.time.start();
	REQUIRE(recorder.calls.size() == 1);
	CHECK(recorder.calls[0] == TimeEvents::kSynchronized);
	CHECK(rig.time.synchronized());
	LocalTime local;
	REQUIRE(rig.time.local_time(local));
	CHECK(local.hour == 10);
}

TEST_CASE("TimeService.DetectsJumpsBeyondAMinuteOnly") {
	ScopedCapture capture;
	Rig rig;
	rig.clock.synchronized = true;
	rig.clock.wall_seconds = utc(2026, 10, 17, 8, 0);
	Recorder recorder;
	rig.time.add_listener(recorder, TimeEvents::kClockJumped);
	rig.time.start();
	rig.advance(10000);
	CHECK(recorder.calls.empty());

	rig.clock.wall_seconds += 30; // a small SNTP correction is no jump
	rig.advance(1000);
	CHECK(recorder.calls.empty());

	rig.clock.wall_seconds += 3600;
	rig.advance(1000);
	REQUIRE(recorder.calls.size() == 1);
	CHECK(recorder.calls[0] == TimeEvents::kClockJumped);
	CHECK(capture.sink.contains("I hc.time: wall clock jumped by 3600 s"));

	rig.clock.wall_seconds -= 120;
	rig.advance(1000);
	CHECK(recorder.calls.size() == 2);
}

TEST_CASE("TimeService.DayChangeAtLocalMidnightAndOffsetChanges") {
	Rig rig;
	rig.clock.synchronized = true;
	rig.clock.wall_seconds = utc(2026, 10, 17, 21, 59, 50); // 23:59:50 local (UTC+2)
	Recorder recorder;
	rig.time.add_listener(recorder);
	rig.time.start();
	recorder.calls.clear();
	rig.advance(9000);
	CHECK(recorder.calls.empty());
	rig.advance(2000);
	REQUIRE(recorder.calls.size() == 1);
	CHECK(recorder.calls[0] == TimeEvents::kDayChanged);

	// A DST end at noon: the offset changes, the date does not.
	rig.advance(12 * 3600 * 1000);
	CHECK(recorder.calls.size() == 1);
	rig.zone.offset_s = 3600;
	rig.advance(1000);
	REQUIRE(recorder.calls.size() == 2);
	CHECK(recorder.calls[1] == TimeEvents::kOffsetChanged);

	// A zone change across midnight (11:00 local becomes 23:00 the day before): both at once.
	rig.zone.offset_s = -11 * 3600;
	rig.time.refresh();
	REQUIRE(recorder.calls.size() == 3);
	CHECK(recorder.calls[2] == (TimeEvents::kOffsetChanged | TimeEvents::kDayChanged));
}

TEST_CASE("TimeService.DayChangesOnceOnTheDstDays") {
	EuTimeZone eu;
	FakeClock clock;
	FakeSettings settings;
	hc::EventLoop loop;
	TimeService time(loop, clock, eu, settings);
	clock.synchronized = true;
	clock.wall_seconds = utc(2026, 10, 24, 12, 0); // Saturday before the fall-back
	Recorder recorder;
	time.add_listener(recorder, TimeEvents::kDayChanged | TimeEvents::kOffsetChanged);
	time.start();
	// Two days in 1 s steps.
	int days = 0;
	int offsets = 0;
	for (int i = 0; i < 2 * 86400; ++i) {
		clock.now_ms += 1000;
		clock.wall_seconds += 1;
		loop.run_once(clock.now_ms);
	}
	for (uint8_t events : recorder.calls) {
		days += (events & TimeEvents::kDayChanged) != 0;
		offsets += (events & TimeEvents::kOffsetChanged) != 0;
	}
	CHECK(days == 2);
	CHECK(offsets == 1);
}

TEST_CASE("TimeService.SynchronizationLost") {
	ScopedCapture capture;
	Rig rig;
	rig.clock.synchronized = true;
	rig.clock.wall_seconds = utc(2026, 10, 17, 8, 0);
	Recorder recorder;
	rig.time.add_listener(recorder, TimeEvents::kSynchronizationLost | TimeEvents::kSynchronized);
	rig.time.start();
	recorder.calls.clear();
	rig.clock.synchronized = false;
	rig.advance(1000);
	REQUIRE(recorder.calls.size() == 1);
	CHECK(recorder.calls[0] == TimeEvents::kSynchronizationLost);
	CHECK(capture.sink.contains("W hc.time: wall clock lost its synchronization"));
	CHECK_FALSE(rig.time.synchronized());
	LocalTime local;
	CHECK_FALSE(rig.time.local_time(local));
	rig.clock.synchronized = true;
	rig.advance(1000);
	CHECK(recorder.calls.back() == TimeEvents::kSynchronized);
}

TEST_CASE("TimeService.ListenerRegistrationRules") {
	ScopedCapture capture;
	Rig rig;
	Recorder a, b, c, d, e;
	CHECK(rig.time.add_listener(a));
	CHECK_FALSE(rig.time.add_listener(a));
	CHECK(capture.sink.contains("E hc.time: refusing to add a time listener twice"));
	CHECK(rig.time.add_listener(b));
	CHECK(rig.time.add_listener(c));
	CHECK(rig.time.add_listener(d));
	CHECK_FALSE(rig.time.add_listener(e));
	CHECK(capture.sink.contains("E hc.time: refusing time listener: capacity of 4 exhausted"));
	CHECK_FALSE(rig.time.remove_listener(e));

	// From within a notification: refused.
	struct Meddler : hc::TimeListener {
		TimeService* service = nullptr;
		Recorder* other = nullptr;
		bool added = true;
		void time_changed(uint8_t) override { added = service->add_listener(*other); }
	};
	Rig second;
	Meddler meddler;
	meddler.service = &second.time;
	meddler.other = &e;
	second.time.add_listener(meddler);
	second.clock.synchronized = true;
	second.time.start();
	CHECK_FALSE(meddler.added);
	CHECK(capture.sink.contains("E hc.time: refusing to add a time listener from within a notification"));

	rig.time.start();
	rig.time.start();
	CHECK(capture.sink.contains("E hc.time: refusing start: time service already started"));
}

TEST_CASE("TimeService.LocationFromTheSettings") {
	SUBCASE("valid") {
		ScopedCapture capture;
		Rig rig;
		rig.settings.set_string("location.lat", "48.137");
		rig.settings.set_string("location.lon", "11.575");
		rig.settings.set_int("location.elev", 520);
		rig.time.start();
		CHECK(capture.sink.contains("I hc.time: location 48.137, 11.575, 520 m"));
		hc::GeoLocation where;
		REQUIRE(rig.time.location(where));
		CHECK(where.latitude == doctest::Approx(48.137));
		CHECK(where.elevation_m == 520);
		hc::SunTimes sun;
		Date day;
		day.year = 2026;
		day.month = 6;
		day.day = 21;
		REQUIRE(rig.time.sun_times(day, sun));
		CHECK(sun.occurs(hc::SunEvent::sunrise));
	}
	SUBCASE("only one coordinate") {
		ScopedCapture capture;
		Rig rig;
		rig.settings.set_string("location.lat", "48.137");
		rig.time.start();
		CHECK(capture.sink.contains("E hc.time: astronomy unavailable: location.lat ok, location.lon not found"));
		hc::GeoLocation where;
		CHECK_FALSE(rig.time.location(where));
	}
	SUBCASE("out of range") {
		ScopedCapture capture;
		Rig rig;
		rig.settings.set_string("location.lat", "91");
		rig.settings.set_string("location.lon", "11.575");
		rig.time.start();
		CHECK(capture.sink.contains("E hc.time: astronomy unavailable: location.lat \"91\": coordinate out of range"));
		hc::SunTimes sun;
		CHECK_FALSE(rig.time.sun_times(Date{}, sun));
	}
	SUBCASE("bad elevation falls back to 0 m") {
		ScopedCapture capture;
		Rig rig;
		rig.settings.set_string("location.lat", "48.137");
		rig.settings.set_string("location.lon", "11.575");
		rig.settings.set_int("location.elev", 12000);
		rig.time.start();
		CHECK(capture.sink.contains("E hc.time: ignoring location.elev 12000: elevation must lie in -500..9000 m; using 0 m"));
		hc::GeoLocation where;
		REQUIRE(rig.time.location(where));
		CHECK(where.elevation_m == 0);
	}
}

TEST_CASE("TimeService.SetAndClearLocationNotify") {
	ScopedCapture capture;
	Rig rig;
	Recorder recorder;
	rig.time.add_listener(recorder, TimeEvents::kLocationChanged);
	rig.time.start();
	hc::GeoLocation where;
	where.latitude = -33.87;
	where.longitude = 151.21;
	where.elevation_m = 10.4;
	CHECK(rig.time.set_location(where));
	REQUIRE(recorder.calls.size() == 1);
	CHECK(recorder.calls[0] == TimeEvents::kLocationChanged);
	char text[32];
	REQUIRE(rig.settings.get_string("location.lat", text, sizeof(text)) == hc::SettingsStatus::ok);
	CHECK(std::string(text) == "-33.870000");
	int32_t elevation = 0;
	REQUIRE(rig.settings.get_int("location.elev", elevation) == hc::SettingsStatus::ok);
	CHECK(elevation == 10);

	where.longitude = 200;
	const char* error = nullptr;
	CHECK_FALSE(rig.time.set_location(where, &error));
	CHECK(std::string(error) == "longitude must lie in -180..180 degrees");
	CHECK(capture.sink.contains("E hc.time: refusing location: longitude must lie in -180..180 degrees"));
	CHECK(recorder.calls.size() == 1);

	rig.time.clear_location();
	CHECK(recorder.calls.size() == 2);
	hc::GeoLocation now;
	CHECK_FALSE(rig.time.location(now));
	CHECK(rig.settings.entries.empty());

	// A store failure is refused with the reason.
	rig.settings.fail_writes = true;
	where.longitude = 151.21;
	CHECK_FALSE(rig.time.set_location(where, &error));
	CHECK(std::string(error) == "storage error");
}

TEST_CASE("TimeService.ParseCoordinate") {
	double value = 0;
	const char* error = nullptr;
	CHECK(hc::parse_coordinate("48.1374", -90, 90, value));
	CHECK(value == doctest::Approx(48.1374));
	CHECK(hc::parse_coordinate("-0.5", -90, 90, value));
	CHECK(value == doctest::Approx(-0.5));
	CHECK(hc::parse_coordinate("+151", -180, 180, value));
	CHECK(value == 151);
	CHECK(hc::parse_coordinate("-180.00000000", -180, 180, value));
	CHECK_FALSE(hc::parse_coordinate("", -90, 90, value, &error));
	CHECK(std::string(error) == "empty coordinate");
	CHECK_FALSE(hc::parse_coordinate("1234", -180, 180, value, &error));
	CHECK(std::string(error) == "coordinate has more than 3 integer digits");
	CHECK_FALSE(hc::parse_coordinate("48.", -90, 90, value, &error));
	CHECK_FALSE(hc::parse_coordinate(".5", -90, 90, value, &error));
	CHECK_FALSE(hc::parse_coordinate("48,13", -90, 90, value, &error));
	CHECK(std::string(error) == "coordinate must be decimal degrees (\"48.1374\", \"-0.5\")");
	CHECK_FALSE(hc::parse_coordinate("1.123456789", -90, 90, value, &error));
	CHECK(std::string(error) == "coordinate has more than 8 decimals");
	CHECK_FALSE(hc::parse_coordinate("90.0001", -90, 90, value, &error));
	CHECK(std::string(error) == "coordinate out of range");
	CHECK_FALSE(hc::parse_coordinate(nullptr, -90, 90, value));
}

TEST_CASE("TimeService.CalendarHelpers") {
	CHECK(hc::date_from_days(0) == Date{1970, 1, 1});
	CHECK(hc::date_from_days(hc::days_from_civil(2028, 2, 29)) == Date{2028, 2, 29});
	CHECK(hc::date_from_days(-1) == Date{1969, 12, 31});
	CHECK(hc::weekday_from_days(0) == 4);                                // Thursday
	CHECK(hc::weekday_from_days(hc::days_from_civil(2026, 10, 17)) == 6); // Saturday
	CHECK(hc::weekday_from_days(hc::days_from_civil(2026, 10, 18)) == 7); // Sunday
	CHECK(hc::days_in_month(2026, 2) == 28);
	CHECK(hc::days_in_month(2028, 2) == 29);
	CHECK(hc::days_in_month(2100, 2) == 28);
	CHECK(hc::days_in_month(2000, 2) == 29);
	CHECK(hc::days_in_month(2026, 12) == 31);
	CHECK(hc::days_in_month(2026, 13) == 0);
}

TEST_CASE("TimeService.LocalToUtcAcrossTheDstChanges") {
	EuTimeZone eu;
	FakeClock clock;
	FakeSettings settings;
	hc::EventLoop loop;
	TimeService time(loop, clock, eu, settings);
	// Ordinary days.
	CHECK(time.local_to_utc(Date{2026, 1, 15}, 12, 0, 0) == utc(2026, 1, 15, 11, 0));
	CHECK(time.local_to_utc(Date{2026, 7, 15}, 12, 0, 0) == utc(2026, 7, 15, 10, 0));
	// Spring forward, 2026-03-29: 02:00..02:59 do not exist; they resolve to 03:00 CEST (01:00 UTC).
	CHECK(time.local_to_utc(Date{2026, 3, 29}, 1, 59, 59) == utc(2026, 3, 29, 0, 59, 59));
	CHECK(time.local_to_utc(Date{2026, 3, 29}, 2, 0, 0) == utc(2026, 3, 29, 1, 0));
	CHECK(time.local_to_utc(Date{2026, 3, 29}, 2, 30, 0) == utc(2026, 3, 29, 1, 0));
	CHECK(time.local_to_utc(Date{2026, 3, 29}, 3, 0, 0) == utc(2026, 3, 29, 1, 0));
	CHECK(time.local_to_utc(Date{2026, 3, 29}, 3, 0, 1) == utc(2026, 3, 29, 1, 0, 1));
	// Fall back, 2026-10-25: 02:00..02:59 occur twice; the first (CEST) counts.
	CHECK(time.local_to_utc(Date{2026, 10, 25}, 2, 30, 0) == utc(2026, 10, 25, 0, 30));
	CHECK(time.local_to_utc(Date{2026, 10, 25}, 3, 0, 0) == utc(2026, 10, 25, 2, 0));
	CHECK(time.local_to_utc(Date{2026, 10, 25}, 1, 59, 0) == utc(2026, 10, 24, 23, 59));
	CHECK(time.utc_offset(utc(2026, 10, 25, 0, 59)) == 7200);
	CHECK(time.utc_offset(utc(2026, 10, 25, 1, 0)) == 3600);
}

TEST_CASE("TimeService.SunEventsInLocalTimeOnTheSpringForwardDay") {
	// Berlin, 2026-03-29: USNO gives sunrise 06:48 and sunset 19:35 CEST, dawn 06:14, dusk 20:10.
	EuTimeZone eu;
	FakeClock clock;
	FakeSettings settings;
	settings.set_string("location.lat", "52.52");
	settings.set_string("location.lon", "13.405");
	hc::EventLoop loop;
	TimeService time(loop, clock, eu, settings);
	time.start();
	struct Expected {
		hc::SunEvent event;
		int hour;
		int minute;
	};
	const Expected expected[] = {
		{hc::SunEvent::dawn, 6, 14}, {hc::SunEvent::sunrise, 6, 48},
		{hc::SunEvent::sunset, 19, 35}, {hc::SunEvent::dusk, 20, 10}};
	for (const Expected& e : expected) {
		LocalTime local;
		REQUIRE(time.sun_event_local(Date{2026, 3, 29}, e.event, local));
		CHECK(std::string(local.zone) == "CEST");
		int minutes = local.hour * 60 + local.minute + (local.second >= 30 ? 1 : 0);
		CHECK(std::abs(minutes - (e.hour * 60 + e.minute)) <= 1);
	}
	// The day before is still CET: sunrise about an hour earlier on the clock.
	LocalTime saturday;
	REQUIRE(time.sun_event_local(Date{2026, 3, 28}, hc::SunEvent::sunrise, saturday));
	CHECK(std::string(saturday.zone) == "CET");
	CHECK(saturday.hour == 5);
}
