/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The scheduler tests' rig: a scripted time source with central European time and NOAA astronomy,
// a recorder of firings, spec builders and a Rig that drives the loop and plays the time service's
// day changes. Shared by scheduler_test.cpp and scheduler_probe_test.cpp.
#ifndef HC_SCHEDULER_TEST_SUPPORT_H
#define HC_SCHEDULER_TEST_SUPPORT_H
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/scheduler.h"
#include "hc/time_service.h"
#include "hc/tz.h"
#include "test_support.h"

using hc::Date;
using hc::LocalClockTime;
using hc::LocalTime;
using hc::MissedPolicy;
using hc::ScheduleKind;
using hc::Scheduler;
using hc::ScheduleSpec;
using hc::ScheduleStatus;
using hc::SunEvent;
using hc::TimeEvents;
using hc::Weekdays;
using hc_test::FakeSettings;
using hc_test::ScopedCapture;

namespace sched_test {

/// UTC seconds of a UTC date and time.
inline int64_t utc(int year, unsigned month, unsigned day, int hour, int minute, int second = 0) {
	return hc::days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

/// UTC seconds of a central European wall time in winter (CET, UTC+1) and in summer (CEST, UTC+2).
inline int64_t cet(int year, unsigned month, unsigned day, int hour, int minute, int second = 0) {
	return utc(year, month, day, hour, minute, second) - 3600;
}
inline int64_t cest(int year, unsigned month, unsigned day, int hour, int minute, int second = 0) {
	return utc(year, month, day, hour, minute, second) - 7200;
}

inline int64_t eu_switch(int year, unsigned month) {
	int64_t days = hc::days_from_civil(year, month, 31);
	days -= hc::weekday_from_days(days) % 7;
	return days * 86400 + 3600;
}

/// A scripted time source: the wall clock follows the monotonic clock (whole seconds), local time is
/// central European time with the EU rules, astronomy is hc::sun_times() at a settable place (or
/// unavailable). It is also the Clock of the datapoint model.
class FakeTimeSource : public hc::TimeSource, public hc::Clock {
public:
	uint64_t monotonic_ms() const override { return mono_ms; }

	hc::WallTime now_utc() const override {
		return hc::WallTime{wall_base + static_cast<int64_t>(mono_ms / 1000), synchronized};
	}

	hc::WallTime wall_time() const override { return now_utc(); }

	bool to_local(int64_t t, LocalTime& out) const override {
		int year = hc::date_from_days(t >= 0 ? t / 86400 : (t - 86399) / 86400).year;
		bool dst = t >= eu_switch(year, 3) && t < eu_switch(year, 10);
		int64_t local = t + (dst ? 7200 : 3600);
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

	bool sun_times(const Date& date, hc::SunTimes& out) const override {
		if (!has_location) {
			return false;
		}
		out = hc::sun_times(date.year, date.month, date.day, location);
		return true;
	}

	bool sun_window(const Date& date, hc::SunWindow& out) const override {
		if (!has_location) {
			return false;
		}
		out = hc::sun_window(date.year, date.month, date.day, location);
		if (wide_windows) {
			out.earliest -= 30 * 86400;
			out.latest += 30 * 86400;
		}
		return true;
	}

	/// Sets the wall clock to `t` (a jump if it differs from the current reading).
	void set_utc(int64_t t) { wall_base = t - static_cast<int64_t>(mono_ms / 1000); }

	int64_t wall() const { return now_utc().seconds; }

	uint64_t mono_ms = 5000;
	int64_t wall_base = 0;
	bool synchronized = true;
	bool has_location = false;
	hc::GeoLocation location;
	/// Windows 30 days wider each side: still true, but they skip no date, so the scheduler computes
	/// its whole search window (the reference of the bounded search).
	bool wide_windows = false;
};

/// Records the firings with the wall time they happened at.
class Recorder : public hc::ScheduleListener {
public:
	explicit Recorder(const FakeTimeSource& time) : time_(time) {}

	void schedule_fired(const char* id) override { firings.emplace_back(id, time_.wall()); }

	std::vector<std::pair<std::string, int64_t>> firings;

private:
	const FakeTimeSource& time_;
};

using Firings = std::vector<std::pair<std::string, int64_t>>;

inline LocalClockTime at(int hour, int minute, int second = 0) {
	LocalClockTime t;
	t.hour = static_cast<uint8_t>(hour);
	t.minute = static_cast<uint8_t>(minute);
	t.second = static_cast<uint8_t>(second);
	return t;
}

inline ScheduleSpec weekly(const char* id, uint8_t days, LocalClockTime time) {
	ScheduleSpec spec;
	spec.id = id;
	spec.label = id;
	spec.kind = ScheduleKind::weekly;
	spec.days = days;
	spec.time = time;
	return spec;
}

inline ScheduleSpec yearly(const char* id, int month, int day, LocalClockTime time) {
	ScheduleSpec spec;
	spec.id = id;
	spec.label = id;
	spec.kind = ScheduleKind::yearly;
	spec.month = static_cast<uint8_t>(month);
	spec.day = static_cast<uint8_t>(day);
	spec.time = time;
	return spec;
}

inline ScheduleSpec dates(const char* id, const Date* list, size_t count, LocalClockTime time) {
	ScheduleSpec spec;
	spec.id = id;
	spec.label = id;
	spec.kind = ScheduleKind::dates;
	spec.dates = list;
	spec.date_count = count;
	spec.time = time;
	return spec;
}

inline ScheduleSpec astro(const char* id, SunEvent sun, int32_t offset_s) {
	ScheduleSpec spec;
	spec.id = id;
	spec.label = id;
	spec.kind = ScheduleKind::astro;
	spec.sun = sun;
	spec.offset_s = offset_s;
	return spec;
}

inline ScheduleSpec interval(const char* id, uint8_t days, int every, LocalClockTime from, LocalClockTime until) {
	ScheduleSpec spec;
	spec.id = id;
	spec.label = id;
	spec.kind = ScheduleKind::interval;
	spec.days = days;
	spec.every_minutes = static_cast<uint16_t>(every);
	spec.from = from;
	spec.until = until;
	return spec;
}

inline ScheduleSpec once(const char* id, Date date, LocalClockTime time) {
	ScheduleSpec spec;
	spec.id = id;
	spec.label = id;
	spec.kind = ScheduleKind::once;
	spec.at_date = date;
	spec.at_time = time;
	return spec;
}

constexpr uint8_t kWorkdays = 0x1F;
constexpr uint8_t kMonWedFri = 0x01 | 0x04 | 0x10;

/// The scheduler on a fake time source, an event loop and a datapoint model. run_until() drives the
/// loop like a platform and plays the time service's part: a day change at each local midnight.
struct Rig {
	FakeTimeSource time;
	FakeSettings settings;
	hc::EventLoop loop;
	hc::DatapointModel model{time};
	std::unique_ptr<Scheduler> scheduler;
	Recorder recorder{time};

	Rig() { boot(); }

	/// A fresh scheduler (a reboot: the settings stay, the RAM state is gone).
	void boot() {
		scheduler.reset();
		scheduler.reset(new Scheduler(loop, time, settings, &model));
		scheduler->add_listener(recorder);
	}

	ScheduleStatus load(const std::vector<ScheduleSpec>& specs) {
		return scheduler->load(specs.data(), specs.size());
	}

	/// Runs until the wall clock reads `until` (UTC seconds).
	void run_until(int64_t until) {
		for (;;) {
			loop.run_once(time.mono_ms);
			uint64_t target = time.mono_ms + static_cast<uint64_t>((until - time.wall()) * 1000);
			uint64_t deadline = loop.next_deadline();
			Date before;
			time.local_date(time.wall(), before);
			if (deadline > target) {
				step(target);
				loop.run_once(time.mono_ms);
				return;
			}
			if (deadline > time.mono_ms) {
				step(deadline);
			}
		}
	}

	/// Advances the monotonic clock to `mono`; tells the scheduler about local midnights on the way
	/// (in one-second resolution, like the service's watch).
	void step(uint64_t mono) {
		Date before;
		time.local_date(time.wall(), before);
		int64_t midnight = time.local_to_utc(hc::date_from_days(hc::days_from_civil(before.year, before.month, before.day) + 1), 0, 0, 0);
		uint64_t midnight_mono = time.mono_ms + static_cast<uint64_t>((midnight - time.wall()) * 1000) + 1000;
		if (mono > midnight_mono) {
			time.mono_ms = midnight_mono;
			loop.run_once(time.mono_ms);
			scheduler->time_changed(TimeEvents::kDayChanged);
			loop.run_once(time.mono_ms);
			step(mono);
			return;
		}
		time.mono_ms = mono;
	}

	Firings take() {
		Firings result;
		result.swap(recorder.firings);
		return result;
	}
};

inline std::string describe(const Firings& firings) {
	std::string text;
	for (const auto& firing : firings) {
		text += firing.first + "@" + std::to_string(firing.second) + " ";
	}
	return text;
}

} // namespace sched_test
using namespace sched_test; // NOLINT: test rig

#endif // HC_SCHEDULER_TEST_SUPPORT_H
