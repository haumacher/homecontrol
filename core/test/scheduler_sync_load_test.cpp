/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The scheduler's work per turn of the loop with a house's schedules (homecontrol #105): the first
// evaluation after the clock synchronizes needed the sun of five or six dates in one turn (about
// 5 ms each on the C6: `slow periodic handler time.tick took 31 ms`); it computes at most
// Scheduler::kSunDatesPerTurn uncached dates per turn now, and decides exactly what the evaluation in
// one turn decides.
#include <algorithm>

#include "doctest.h"
#include "scheduler_test_support.h"

using hc::TimeService;
using hc_test::FakeClock;
using hc_test::FakeTimeZone;

namespace {

/// The TimeService without its cache's answer (sun_known() is the default: every date known): the
/// scheduler evaluates in one turn, as before #105. The reference of the spread evaluation.
class UnspreadTime : public hc::TimeSource {
public:
	explicit UnspreadTime(const TimeService& time) : time_(time) {}
	uint64_t monotonic_ms() const override { return time_.monotonic_ms(); }
	hc::WallTime now_utc() const override { return time_.now_utc(); }
	bool to_local(int64_t t, LocalTime& out) const override { return time_.to_local(t, out); }
	bool sun_times(const Date& date, hc::SunTimes& out) const override { return time_.sun_times(date, out); }
	bool sun_window(const Date& date, hc::SunWindow& out) const override { return time_.sun_window(date, out); }
	bool location(hc::GeoLocation& out) const override { return time_.location(out); }

private:
	const TimeService& time_;
};

/// A house's 33 schedules: blinds per facade on weekdays and at the weekend with clamps, lights at
/// dusk and dawn, a Friday entry, heating and ventilation times, holidays and an interval.
std::vector<ScheduleSpec> house() {
	std::vector<ScheduleSpec> s;
	auto clamp = [](ScheduleSpec spec, int before_h, int before_m, int after_h, int after_m) {
		if (before_h >= 0) {
			spec.has_not_before = true;
			spec.not_before = at(before_h, before_m);
		}
		if (after_h >= 0) {
			spec.has_not_after = true;
			spec.not_after = at(after_h, after_m);
		}
		return spec;
	};
	auto on = [](ScheduleSpec spec, uint8_t days) {
		spec.days = days;
		return spec;
	};
	constexpr uint8_t kWeekend = 0x20 | 0x40;
	s.push_back(on(clamp(astro("up-east-wd", SunEvent::sunrise, 0), 6, 30, -1, 0), kWorkdays));
	s.push_back(on(clamp(astro("up-east-we", SunEvent::sunrise, 0), 8, 0, -1, 0), kWeekend));
	s.push_back(on(clamp(astro("up-south-wd", SunEvent::sunrise, 30 * 60), 7, 0, -1, 0), kWorkdays));
	s.push_back(on(clamp(astro("up-south-we", SunEvent::sunrise, 30 * 60), 8, 30, -1, 0), kWeekend));
	s.push_back(clamp(astro("down-west", SunEvent::sunset, 15 * 60), -1, 0, 22, 0));
	s.push_back(clamp(astro("down-south", SunEvent::sunset, -30 * 60), 17, 0, 22, 0));
	s.push_back(astro("down-north", SunEvent::dusk, 0));
	s.push_back(astro("light-dusk", SunEvent::dusk, 0));
	s.push_back(astro("light-dawn", SunEvent::dawn, 0));
	s.push_back(clamp(astro("light-off", SunEvent::dusk, 3 * 3600), 23, 30, -1, 0));
	s.push_back(on(astro("garden-fri", SunEvent::sunset, 0), 0x10));
	s.push_back(on(astro("dawn-we", SunEvent::dawn, -30 * 60), kWeekend));
	static const char* const kWeekly[] = {"w01", "w02", "w03", "w04", "w05", "w06", "w07", "w08", "w09", "w10",
		"w11", "w12", "w13", "w14", "w15", "w16", "w17", "w18"};
	for (int i = 0; i < 18; ++i) {
		s.push_back(weekly(kWeekly[i], i % 3 == 0 ? kWeekend : (i % 3 == 1 ? kWorkdays : 0x7F), at(5 + i, 15)));
	}
	s.push_back(yearly("christmas", 12, 24, at(16, 0)));
	s.push_back(yearly("new-year", 1, 1, at(0, 5)));
	s.push_back(interval("vent", kWorkdays, 60, at(8, 0), at(20, 0)));
	return s;
}

/// Records the firings with the wall time they happened at.
class Firings : public hc::ScheduleListener {
public:
	explicit Firings(const FakeClock& clock) : clock_(clock) {}
	void schedule_fired(const char* id) override { list.emplace_back(id, clock_.wall_seconds); }
	std::vector<std::pair<std::string, int64_t>> list;

private:
	const FakeClock& clock_;
};

/// The controller's time service and a scheduler of the house on it (or on its unspread view).
struct House {
	House(FakeClock& clock, FakeSettings& settings, bool spread)
		: time(loop, clock, zone, settings), unspread(time),
		  scheduler(loop, spread ? static_cast<const hc::TimeSource&>(time) : unspread, settings, nullptr),
		  firings(clock), clock_(clock) {
		loop.set_clock(&clock);
		hc::GeoLocation munich;
		munich.latitude = 48.137;
		munich.longitude = 11.575;
		REQUIRE(time.set_location(munich));
		REQUIRE(time.add_listener(scheduler));
		REQUIRE(scheduler.add_listener(firings));
		specs = house();
		REQUIRE(specs.size() == 33);
		REQUIRE(scheduler.load(specs.data(), specs.size()) == ScheduleStatus::ok);
		time.start();
	}

	/// One turn of the loop at the current time; the sun dates it computed.
	uint32_t turn() {
		uint32_t before = time.sun_computations();
		loop.run_once(clock_.now_ms);
		uint32_t computed = time.sun_computations() - before;
		most_per_turn = std::max(most_per_turn, computed);
		return computed;
	}

	/// The turns due now; how many there were.
	int settle() {
		int turns = 0;
		while (loop.next_deadline() <= clock_.now_ms) {
			REQUIRE(turns < 100);
			turn();
			++turns;
		}
		return turns;
	}

	/// Runs the loop until `until` (monotonic ms), the wall clock following.
	void run(uint64_t until) {
		for (;;) {
			settle();
			uint64_t next = loop.next_deadline();
			if (next > until) {
				break;
			}
			clock_.wall_seconds += static_cast<int64_t>(next / 1000 - clock_.now_ms / 1000);
			clock_.now_ms = next;
		}
		clock_.wall_seconds += static_cast<int64_t>(until / 1000 - clock_.now_ms / 1000);
		clock_.now_ms = until;
	}

	std::vector<int64_t> nexts() const {
		std::vector<int64_t> out;
		for (size_t i = 0; i < specs.size(); ++i) {
			std::optional<int64_t> next = scheduler.next_firing(i);
			out.push_back(next ? *next : -1);
		}
		return out;
	}

	hc::EventLoop loop;
	FakeTimeZone zone;
	TimeService time;
	UnspreadTime unspread;
	Scheduler scheduler;
	Firings firings;
	std::vector<ScheduleSpec> specs;
	uint32_t most_per_turn = 0;

private:
	FakeClock& clock_;
};

/// The records of a controller that ran the house for three days and stopped a minute before
/// `sync_at`, in `settings`; the clock then reads 1 s after a boot, not synchronized.
void three_days_before(FakeClock& clock, FakeSettings& settings, int64_t sync_at) {
	clock.synchronized = true;
	clock.now_ms = 1000;
	clock.wall_seconds = sync_at - 3 * 86400;
	{
		House before(clock, settings, true);
		before.run(clock.now_ms + (3 * 86400 - 60) * 1000ull);
	}
	clock.synchronized = false;
	clock.now_ms = 1000;
}

// Morning, evening, before and after midnight, a Friday evening, midsummer.
const int64_t kMoments[] = {cest(2026, 11, 22, 10, 0), cest(2026, 11, 22, 7, 0), cest(2026, 11, 21, 23, 55),
	cest(2026, 11, 23, 0, 5), cest(2026, 11, 20, 18, 0), cest(2026, 6, 21, 12, 0)};

} // namespace

TEST_CASE("SchedulerSyncLoad.TheFirstEvaluationComputesASunDatePerTurn") {
	for (int64_t sync_at : kMoments) {
		INFO("synchronized at " << sync_at);
		FakeClock clock;
		FakeSettings settings;
		three_days_before(clock, settings, sync_at);
		FakeSettings reference_settings = settings;
		FakeClock reference_clock = clock;

		// Before #105: the evaluation in the watch's turn computes every date it needs at once.
		House reference(reference_clock, reference_settings, false);
		reference_clock.wall_seconds = sync_at;
		reference_clock.synchronized = true;
		reference_clock.now_ms += 1000;
		reference.settle(); // the watch's tick (and a firing due from the minute the controller was off)
		CHECK(reference.most_per_turn >= 5);

		House house(clock, settings, true);
		clock.wall_seconds = sync_at;
		clock.synchronized = true;
		clock.now_ms += 1000;
		int turns = house.settle();
		CHECK(house.most_per_turn == static_cast<uint32_t>(Scheduler::kSunDatesPerTurn));
		CHECK(house.time.sun_computations() == reference.time.sun_computations()); // nothing computed twice
		CHECK(turns <= static_cast<int>(house.time.sun_computations()) + 1);
		CHECK(house.nexts() == reference.nexts());
		CHECK(house.firings.list == reference.firings.list);
	}
}

TEST_CASE("SchedulerSyncLoad.AWeekFiresWhatTheUnspreadEvaluationFires") {
	// Day changes, firings with their evaluations, a clock jump and a new location: no turn computes
	// more than one date, and the firings are those of the evaluation in one turn.
	FakeClock clock;
	FakeSettings settings;
	const int64_t sync_at = cest(2026, 11, 20, 18, 0);
	three_days_before(clock, settings, sync_at);
	FakeSettings reference_settings = settings;
	FakeClock reference_clock = clock;
	House reference(reference_clock, reference_settings, false);
	House house(clock, settings, true);
	for (auto* side : {&clock, &reference_clock}) {
		side->wall_seconds = sync_at;
		side->synchronized = true;
		side->now_ms += 1000;
	}
	house.run(clock.now_ms + 3 * 86400 * 1000ull);
	reference.run(reference_clock.now_ms + 3 * 86400 * 1000ull);
	for (auto* side : {&clock, &reference_clock}) {
		side->wall_seconds += 2 * 3600; // a step of the clock: kClockJumped
	}
	house.run(clock.now_ms + 86400 * 1000ull);
	reference.run(reference_clock.now_ms + 86400 * 1000ull);
	hc::GeoLocation hamburg;
	hamburg.latitude = 53.55;
	hamburg.longitude = 9.99;
	REQUIRE(house.time.set_location(hamburg)); // kLocationChanged: the cache holds no date of it
	REQUIRE(reference.time.set_location(hamburg));
	house.run(clock.now_ms + 3 * 86400 * 1000ull);
	reference.run(reference_clock.now_ms + 3 * 86400 * 1000ull);

	CHECK(house.firings.list.size() > 7 * 20);
	CHECK(house.firings.list == reference.firings.list);
	CHECK(house.nexts() == reference.nexts());
	CHECK(house.most_per_turn == static_cast<uint32_t>(Scheduler::kSunDatesPerTurn));
	CHECK(reference.most_per_turn >= 5); // the new location
}

TEST_CASE("SchedulerSyncLoad.AnInhabitantsChangeEvaluatesInItsTurn") {
	// set_enabled() answers with the entry's next firing at once (the web API's response), whatever
	// a spread evaluation in progress has not reached yet.
	FakeClock clock;
	FakeSettings settings;
	const int64_t sync_at = cest(2026, 11, 22, 10, 0);
	three_days_before(clock, settings, sync_at);
	House house(clock, settings, true);
	clock.wall_seconds = sync_at;
	clock.synchronized = true;
	clock.now_ms += 1000;
	house.turn(); // the watch's tick: the pass has begun
	REQUIRE(house.loop.next_deadline() <= clock.now_ms);
	REQUIRE(house.scheduler.set_enabled("w01", false));
	for (size_t i = 0; i < house.specs.size(); ++i) {
		INFO(house.specs[i].id);
		CHECK(house.scheduler.next_firing(i).has_value() == (std::string(house.specs[i].id) != "w01"));
	}
	std::vector<int64_t> now = house.nexts();
	house.settle();
	CHECK(house.nexts() == now);
}

TEST_CASE("SchedulerSyncLoadProbe.ANewImageDuringASpreadPassEndsLikeAFreshEvaluation") {
	// The pass holds a position in the old schedules; a smaller image loaded mid-pass must neither run
	// past its end nor keep a next firing of the old one.
	const int64_t sync_at = cest(2026, 11, 22, 10, 0);
	FakeClock clock;
	FakeSettings settings;
	three_days_before(clock, settings, sync_at);
	FakeSettings reference_settings = settings;
	FakeClock reference_clock = clock;
	House reference(reference_clock, reference_settings, false);
	House house(clock, settings, true);
	for (auto* side : {&clock, &reference_clock}) {
		side->wall_seconds = sync_at;
		side->synchronized = true;
		side->now_ms += 1000;
	}
	house.turn(); // the pass has begun
	REQUIRE(house.loop.next_deadline() <= clock.now_ms);
	reference.settle();
	std::vector<ScheduleSpec> smaller(house.specs.rbegin(), house.specs.rbegin() + 12);
	house.specs = smaller;
	reference.specs = smaller;
	REQUIRE(house.scheduler.load(house.specs.data(), house.specs.size()) == ScheduleStatus::ok);
	REQUIRE(reference.scheduler.load(reference.specs.data(), reference.specs.size()) == ScheduleStatus::ok);
	house.settle();
	reference.settle();
	CHECK(house.nexts() == reference.nexts());
	CHECK(house.most_per_turn == static_cast<uint32_t>(Scheduler::kSunDatesPerTurn));
	house.run(clock.now_ms + 2 * 86400 * 1000ull);
	reference.run(reference_clock.now_ms + 2 * 86400 * 1000ull);
	CHECK(house.firings.list == reference.firings.list);
}
