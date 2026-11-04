/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The sun shading (spec/blocks.md 18) on the device's block runtime: its behaviors over simulated
// days in Munich, its cost. The contract cases of the same behaviors are in spec/fixtures/blocks/.
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "allocation_counter.h"
#include "block_rig.h"
#include "doctest.h"
#include "hc/astro.h"
#include "hc/blocks/block_runtime.h"
#include "hc/blocks/block_set.h"
#include "hc/blocks/sun_shading.h"
#include "hc/tz.h"

using hc::blocks::SunShading;

namespace {

using hc_test::blocks::kHour;
using hc_test::blocks::kMinute;
using hc_test::blocks::Rig;

using Points = std::vector<std::pair<std::string, hc::ValueType>>;

constexpr double kPi = 3.14159265358979323846;

double rad(double degrees) { return degrees * kPi / 180.0; }
double deg(double radians) { return radians * 180.0 / kPi; }

Points shading_datapoints() {
	Points p;
	for (const char* r : {"lux", "lux2", "wind", "out", "room", "pos", "slat"}) {
		p.emplace_back(r, hc::ValueType::real);
	}
	for (const char* b : {"rain", "frost", "sw", "sw2", "pres", "sum", "en", "ud"}) {
		p.emplace_back(b, hc::ValueType::boolean);
	}
	return p;
}

constexpr const char* kAllInputs = R"("brightness": "lux", "wind": "wind", "rain": "rain", "frost": "frost",
	"outside": "out", "manual": ["sw", "sw2"], "presence": "pres", "summer": "sum", "roomTemperature": "room",
	"enable": "en")";

std::string shading_block(const std::string& parameters, const std::string& inputs = "",
	const std::string& outputs = R"("position": "pos", "slat": "slat")") {
	return R"([{"id": "ss", "type": "org.homecontrol.sun-shading", "version": "1.0", "label": "Shading",
		"inputs": {)" + inputs + R"(}, "outputs": {)" + outputs + R"(}, "parameters": {)" + parameters + "}}]";
}

/// 2026-06-21 00:00 UTC.
int64_t june21() { return hc::days_from_civil(2026, 6, 21) * 86400; }

/// Munich, a synchronized UTC wall clock from `start_utc` at the rig's time 0.
void munich(Rig& rig, int64_t start_utc) {
	rig.wall().synchronized = true;
	rig.wall().start_utc = start_utc;
	rig.wall().has_location = true;
	rig.wall().location.latitude = 48.137;
	rig.wall().location.longitude = 11.576;
}

/// Every command of the block's outputs.
struct Moves : hc::blocks::BlockObserver {
	struct Move {
		uint64_t at;
		std::string datapoint;
		double value;
	};
	explicit Moves(Rig& rig) : rig_(rig) { rig.blocks().set_observer(this); }
	~Moves() { rig_.blocks().set_observer(nullptr); }

	void block_commanded(const char*, const hc::Datapoint& datapoint, const hc::Value& value) override {
		double v = value.type() == hc::ValueType::boolean ? (value.as_bool() ? 1 : 0) : value.as_real();
		list.push_back(Move{rig_.now(), datapoint.name(), v});
	}
	size_t count(const char* datapoint) const {
		size_t n = 0;
		for (const auto& m : list) {
			n += m.datapoint == datapoint;
		}
		return n;
	}
	void clear() { list.clear(); }

	std::vector<Move> list;

private:
	Rig& rig_;
};

double state(Rig& rig) { return rig.number("block/ss/state"); }

/// Reports a boolean datapoint.
void flag(Rig& rig, const char* name, bool value) { rig.report(name, hc::Value::of_bool(value)); }

/// The sun on the facade by the geometry of spec/blocks.md 18.2 D8, on the library's functions:
/// +1 on, -1 off, 0 within `margin` degrees of a threshold.
struct Facade {
	double azimuth = 180;
	double opening = 180;
	double min_elevation = 5;
	double overhang = 90;
	std::vector<std::pair<double, double>> horizon;

	int on(const hc::SunPosition& sun, double margin) const {
		double g = std::fmod(sun.azimuth - azimuth + 540.0, 360.0) - 180.0;
		double profile = deg(std::atan2(std::sin(rad(sun.elevation)), std::cos(rad(sun.elevation)) * std::cos(rad(g))));
		double h = -90;
		if (!horizon.empty()) {
			if (g <= horizon.front().first) {
				h = horizon.front().second;
			} else if (g >= horizon.back().first) {
				h = horizon.back().second;
			} else {
				for (size_t i = 0; i + 1 < horizon.size(); ++i) {
					if (g <= horizon[i + 1].first) {
						auto a = horizon[i];
						auto b = horizon[i + 1];
						h = a.second + (b.second - a.second) * (g - a.first) / (b.first - a.first);
						break;
					}
				}
			}
		}
		double margins[] = {opening / 2 - std::fabs(g), sun.elevation - min_elevation, sun.elevation - h,
			overhang - profile};
		int result = 1;
		for (double m : margins) {
			if (std::fabs(m) < margin) {
				return 0;
			}
			if (m < 0) {
				result = -1;
			}
		}
		return result;
	}
};

/// Runs a day minute by minute and compares the block's state with the facade's geometry; returns the
/// minutes of shading.
int check_day(Rig& rig, const Facade& facade) {
	int shading = 0;
	int idle = 0;
	int mismatches = 0;
	for (int minute = 1; minute < 24 * 60; ++minute) {
		rig.run(static_cast<uint64_t>(minute) * kMinute);
		hc::SunPosition sun = hc::sun_position(rig.wall().utc(), rig.wall().location);
		CHECK(rig.number("block/ss/sunAzimuth") == sun.azimuth);
		CHECK(rig.number("block/ss/sunElevation") == sun.elevation);
		int expected = facade.on(sun, 0.01);
		double s = state(rig);
		if (expected == 1) {
			mismatches += s != SunShading::shading;
			++shading;
		} else if (expected == -1) {
			mismatches += s != SunShading::idle;
			++idle;
		}
	}
	CHECK(mismatches == 0);
	CHECK(idle > 0);
	return shading;
}

} // namespace

TEST_CASE("SunShading: the manifest is valid and the type is built in") {
	CHECK(hc::blocks::check_manifest(SunShading::instance().manifest()) == nullptr);
	CHECK(hc::blocks::BlockRegistry::device().find("org.homecontrol.sun-shading", 1) == &SunShading::instance());
	CHECK(!SunShading::instance().manifest().persists());
}

TEST_CASE("SunShading: the sun on the facade by azimuth and elevation, the horizon and the overhang") {
	SUBCASE("a south-east facade") {
		Rig rig(shading_block(R"("facadeAzimuth": 135, "openingAngle": 160, "minElevation": 10, "minMoveInterval": 0)"),
			nullptr, shading_datapoints());
		munich(rig, june21());
		Facade f;
		f.azimuth = 135;
		f.opening = 160;
		f.min_elevation = 10;
		int minutes = check_day(rig, f);
		MESSAGE("south-east facade: " << minutes << " minutes of shading on June 21");
		CHECK(minutes > 6 * 60);
	}
	SUBCASE("a west facade behind a building opposite") {
		Rig rig(shading_block(R"("facadeAzimuth": 270, "openingAngle": 170, "minElevation": 3, "horizonPoints": 3,
			"horizonAngle1": -40, "horizonElevation1": 35, "horizonAngle2": 0, "horizonElevation2": 20,
			"horizonAngle3": 40, "horizonElevation3": 8, "minMoveInterval": 0)"),
			nullptr, shading_datapoints());
		munich(rig, june21());
		Facade f;
		f.azimuth = 270;
		f.opening = 170;
		f.min_elevation = 3;
		f.horizon = {{-40, 35}, {0, 20}, {40, 8}};
		int minutes = check_day(rig, f);
		Facade open = f;
		open.horizon.clear();
		int without = 0;
		for (int minute = 0; minute < 24 * 60; ++minute) {
			without += open.on(hc::sun_position(june21() + minute * 60, rig.wall().location), 0) == 1;
		}
		MESSAGE("west facade: " << minutes << " minutes behind the horizon, " << without << " without it");
		CHECK(minutes < without);
		CHECK(minutes > 0);
	}
	SUBCASE("a south window under a balcony") {
		Rig rig(shading_block(R"("overhangAngle": 55, "minMoveInterval": 0)"), nullptr, shading_datapoints());
		munich(rig, june21());
		Facade f;
		f.overhang = 55;
		check_day(rig, f);
		// At noon the balcony shades the window: the sun stands at 65 degrees.
		rig.run(rig.now()); // the last minute of the day
		CHECK(f.on(hc::sun_position(june21() + 11 * 3600, rig.wall().location), 0) == -1);
	}
	SUBCASE("without a wall clock or a location there is no sun: idle") {
		Rig rig(shading_block(R"("minMoveInterval": 0)"), nullptr, shading_datapoints());
		munich(rig, june21() + 11 * 3600);
		rig.wall().has_location = false;
		rig.run(10 * kMinute);
		CHECK(state(rig) == SunShading::idle);
		rig.wall().has_location = true;
		rig.wall().synchronized = false;
		rig.run(20 * kMinute);
		CHECK(state(rig) == SunShading::idle);
		rig.wall().synchronized = true;
		rig.run(30 * kMinute);
		CHECK(state(rig) == SunShading::shading);
	}
}

TEST_CASE("SunShading: the slats at the cut-off angle over a day, in steps") {
	// An east facade: the low morning sun needs closed slats, the higher sun lets them open.
	Rig rig(shading_block(R"("facadeAzimuth": 90, "slatMode": "tracking", "slatRatio": 0.85, "slatStep": 5,
				"minMoveInterval": 0)",
				"", R"("position": "pos", "slat": "slat", "upDown": "ud")"),
		nullptr, shading_datapoints());
	munich(rig, june21());
	Moves moves(rig);
	int checked = 0;
	int blocked = 0;
	double lowest = 100;
	double highest = 0;
	for (int minute = 1; minute < 24 * 60; ++minute) {
		rig.run(static_cast<uint64_t>(minute) * kMinute);
		if (state(rig) != SunShading::shading) {
			continue;
		}
		hc::SunPosition sun = hc::sun_position(rig.wall().utc(), rig.wall().location);
		double g = std::fmod(sun.azimuth - 90.0 + 540.0, 360.0) - 180.0;
		double alpha = deg(std::atan2(std::sin(rad(sun.elevation)), std::cos(rad(sun.elevation)) * std::cos(rad(g))));
		double beta = std::fmin(std::fmax(deg(std::asin(0.85 * std::cos(rad(alpha)))) - alpha, 0.0), 90.0);
		double q = 100.0 * beta / 90.0;
		double slat = rig.number("slat");
		if (std::fabs(q / 5 - std::round(q / 5)) > 1e-6) {
			CHECK(slat == doctest::Approx(std::ceil(q / 5) * 5));
			++checked;
		}
		CHECK(std::fmod(slat, 5.0) == 0);
		// At the slat position written the direct sun does not pass: sin(alpha + beta) >= (s/w) cos(alpha).
		double b = slat * 90.0 / 100.0;
		blocked += std::sin(rad(alpha + b)) >= 0.85 * std::cos(rad(alpha)) - 1e-9 || b >= 90 - 1e-9;
		lowest = std::fmin(lowest, slat);
		highest = std::fmax(highest, slat);
		CHECK(rig.number("pos") == 100);
		CHECK(rig.number("ud") == 1);
	}
	MESSAGE("slats " << lowest << " .. " << highest << " %, " << moves.count("slat") << " slat moves, "
		<< checked << " minutes checked");
	CHECK(checked > 150);
	CHECK(blocked > 150);
	CHECK(lowest < highest);
	CHECK(moves.count("slat") < 40); // in steps, not every minute
	CHECK(moves.count("pos") == 2);  // down when shading begins, up when it ends (D2)
	CHECK(moves.count("ud") == 2);
}

TEST_CASE("SunShading: brightness hysteresis and delays") {
	// From 10:00 UTC the sun is on the south facade all the time.
	Rig rig(shading_block(R"("minMoveInterval": 0)", R"("brightness": ["lux", "lux2"])"), nullptr,
		shading_datapoints());
	munich(rig, june21() + 10 * 3600);
	Moves moves(rig);
	rig.report("lux", 25000.0); // between the thresholds: dark stays dark
	rig.run(10 * kMinute);
	CHECK(state(rig) == SunShading::idle);
	rig.report("lux", 35000.0);
	uint64_t bright_from = rig.now();
	rig.run(bright_from + 119000);
	CHECK(state(rig) == SunShading::idle);
	rig.report("lux", 31000.0); // still bright: the delay runs on
	rig.run(bright_from + 2 * kMinute);
	CHECK(state(rig) == SunShading::shading);
	REQUIRE(!moves.list.empty());
	CHECK(moves.list.front().at == bright_from + 2 * kMinute); // the timer wake, not the next period
	CHECK(rig.number("pos") == 100);
	// A dip below brightnessOff shorter than the off delay, and a value between the thresholds, keep it.
	rig.report("lux", 15000.0);
	rig.run(rig.now() + 14 * kMinute);
	rig.report("lux", 25000.0);
	rig.run(rig.now() + 30 * kMinute);
	CHECK(state(rig) == SunShading::shading);
	// The second sensor: the maximum decides.
	rig.report("lux2", 50000.0);
	rig.report("lux", 1000.0);
	rig.run(rig.now() + 30 * kMinute);
	CHECK(state(rig) == SunShading::shading);
	CHECK(rig.number("block/ss/brightness") == 50000);
	// Dark for the off delay: the blind is retracted once.
	moves.clear();
	rig.report("lux2", 10000.0);
	uint64_t dark_from = rig.now();
	rig.run(dark_from + 15 * kMinute);
	CHECK(state(rig) == SunShading::idle);
	REQUIRE(moves.count("pos") == 1);
	CHECK(moves.list.front().at == dark_from + 15 * kMinute);
	CHECK(rig.number("pos") == 0);
	// A sensor that is not good counts as dark.
	rig.report("lux", 80000.0);
	rig.run(rig.now() + 5 * kMinute);
	CHECK(state(rig) == SunShading::shading);
	rig.stale("lux");
	rig.stale("lux2");
	rig.run(rig.now() + 16 * kMinute);
	CHECK(state(rig) == SunShading::idle);
}

TEST_CASE("SunShading: a wind alarm overrides everything and releases after the delay") {
	Rig rig(shading_block(R"("minMoveInterval": 0, "windTimeout": 1800000)", kAllInputs,
				R"("position": "pos", "slat": "slat", "upDown": "ud")"),
		nullptr, shading_datapoints());
	munich(rig, june21() + 10 * 3600);
	CHECK(state(rig) == SunShading::wind); // windTimeout: a wind sensor without a value is an alarm
	rig.report("lux", 50000.0);
	rig.report("wind", 3.0);
	rig.run(10 * kMinute - 1);
	CHECK(state(rig) == SunShading::wind);
	rig.run(10 * kMinute);
	REQUIRE(state(rig) == SunShading::shading);
	Moves moves(rig);
	// Wind during shading: retracted at once.
	rig.report("wind", 12.0);
	CHECK(state(rig) == SunShading::wind);
	CHECK(rig.number("pos") == 0);
	CHECK(rig.number("slat") == 0);
	CHECK(rig.number("ud") == 0);
	CHECK(moves.list.size() == 3);
	// An operation during the alarm locks nothing and writes the retracted position again.
	moves.clear();
	flag(rig, "sw", true);
	CHECK(state(rig) == SunShading::wind);
	CHECK(moves.list.size() == 3);
	// Heat protection would close: wind wins.
	flag(rig, "pres", false);
	flag(rig, "sum", true);
	rig.report("room", 30.0);
	CHECK(state(rig) == SunShading::wind);
	// Calm again: the alarm ends 10 minutes later, a gust restarts the count.
	rig.report("wind", 4.0);
	rig.run(rig.now() + 6 * kMinute);
	CHECK(state(rig) == SunShading::wind);
	rig.report("wind", 11.0);
	rig.report("wind", 4.0);
	uint64_t calm = rig.now();
	rig.run(calm + 10 * kMinute - 1);
	CHECK(state(rig) == SunShading::wind);
	moves.clear();
	rig.run(calm + 10 * kMinute);
	CHECK(state(rig) == SunShading::heat_protection);
	CHECK(rig.number("pos") == 100);
	CHECK(rig.number("slat") == 100);
	CHECK(moves.list.size() == 3); // all three: after an alarm the blind is moved, not assumed
	// A stale storm is a storm.
	rig.report("wind", 20.0);
	rig.stale("wind");
	rig.run(rig.now() + 20 * kMinute);
	CHECK(state(rig) == SunShading::wind);
	// A silent sensor is an alarm after windTimeout.
	rig.report("wind", 2.0);
	uint64_t last = rig.now();
	rig.run(last + 10 * kMinute + 1);
	CHECK(state(rig) == SunShading::heat_protection);
	rig.run(last + 30 * kMinute - 1);
	CHECK(state(rig) == SunShading::heat_protection);
	rig.run(last + 30 * kMinute);
	CHECK(state(rig) == SunShading::wind);
}

TEST_CASE("SunShading: rain and frost alarms, and the order of the alarms") {
	Rig rig(shading_block(R"("minMoveInterval": 0, "alarmRelease": 300000)", kAllInputs), nullptr,
		shading_datapoints());
	munich(rig, june21() + 10 * 3600);
	rig.report("lux", 50000.0);
	rig.run(5 * kMinute);
	REQUIRE(state(rig) == SunShading::shading);
	flag(rig, "rain", true);
	CHECK(state(rig) == SunShading::rain);
	CHECK(rig.number("pos") == 0);
	rig.report("wind", 15.0);
	CHECK(state(rig) == SunShading::wind);
	rig.report("wind", 1.0);
	rig.run(rig.now() + 5 * kMinute);
	CHECK(state(rig) == SunShading::rain);
	flag(rig, "rain", false);
	rig.report("out", 1.5); // below frostLimit 2
	CHECK(state(rig) == SunShading::rain); // rain's release runs, and frost waits below it
	rig.run(rig.now() + 5 * kMinute);
	CHECK(state(rig) == SunShading::frost);
	rig.report("out", 5.0);
	flag(rig, "frost", true);
	rig.run(rig.now() + 10 * kMinute);
	CHECK(state(rig) == SunShading::frost);
	flag(rig, "frost", false);
	rig.run(rig.now() + 5 * kMinute);
	CHECK(state(rig) == SunShading::shading);
	CHECK(rig.number("pos") == 100);
}

TEST_CASE("SunShading: a manual operation locks the automatic") {
	SUBCASE("for a duration") {
		Rig rig(shading_block(R"("minMoveInterval": 0)", kAllInputs), nullptr, shading_datapoints());
		munich(rig, june21() + 9 * 3600);
		rig.report("lux", 50000.0);
		rig.run(5 * kMinute);
		REQUIRE(state(rig) == SunShading::shading);
		Moves moves(rig);
		flag(rig, "sw", false); // the inhabitant raises the blind
		uint64_t op = rig.now();
		CHECK(state(rig) == SunShading::manual);
		rig.run(op + 2 * kHour - 1);
		CHECK(state(rig) == SunShading::manual);
		CHECK(moves.list.empty());
		flag(rig, "sw2", true); // another operation restarts the lock
		op = rig.now();
		rig.run(op + 2 * kHour);
		CHECK(state(rig) == SunShading::shading);
		CHECK(moves.list.size() == 2); // position and slat again (due)
		CHECK(moves.list.front().at == op + 2 * kHour);
	}
	SUBCASE("until the sun changes") {
		Rig rig(shading_block(R"("minMoveInterval": 0, "manualLock": "sunChange", "brightnessOffDelay": 0)", kAllInputs),
			nullptr, shading_datapoints());
		munich(rig, june21() + 9 * 3600);
		rig.report("lux", 50000.0);
		rig.run(5 * kMinute);
		flag(rig, "sw", true);
		rig.run(rig.now() + 30 * kMinute);
		CHECK(state(rig) == SunShading::manual);
		rig.report("lux", 1000.0); // the sun goes: the lock ends, and in idle nothing is written
		CHECK(state(rig) == SunShading::idle);
		CHECK(rig.field(SunShading::kManualAt) == -1);
	}
	SUBCASE("until midnight") {
		Rig rig(shading_block(R"("minMoveInterval": 0, "manualLock": "midnight")", kAllInputs), nullptr,
			shading_datapoints());
		munich(rig, june21() + 9 * 3600);
		rig.report("lux", 50000.0);
		rig.run(5 * kMinute);
		flag(rig, "sw", true);
		rig.run(14 * kHour + 59 * kMinute); // 23:59 UTC
		CHECK(state(rig) == SunShading::manual);
		rig.run(15 * kHour + 1 * kMinute);  // 00:01 the next day
		CHECK(state(rig) == SunShading::idle);
		CHECK(rig.field(SunShading::kManualAt) == -1);
	}
	SUBCASE("until midnight, without a wall clock: for the duration") {
		Rig rig(shading_block(R"("minMoveInterval": 0, "manualLock": "midnight")", kAllInputs), nullptr,
			shading_datapoints());
		munich(rig, june21() + 9 * 3600);
		rig.wall().synchronized = false;
		flag(rig, "sw", true);
		rig.run(2 * kHour - 1);
		CHECK(state(rig) == SunShading::manual);
		rig.run(2 * kHour);
		CHECK(state(rig) != SunShading::manual);
	}
}

TEST_CASE("SunShading: heat protection while nobody is at home, with its hysteresis, without the sun") {
	Rig rig(shading_block(R"("minMoveInterval": 0)", kAllInputs, R"("position": "pos", "slat": "slat", "upDown": "ud")"),
		nullptr, shading_datapoints());
	munich(rig, june21() + 22 * 3600); // night: no sun on the facade
	rig.wall().has_location = false;
	flag(rig, "pres", false);
	rig.report("room", 27.0);
	CHECK(state(rig) == SunShading::idle); // summer is bound but unknown
	flag(rig, "sum", true);
	CHECK(state(rig) == SunShading::heat_protection);
	CHECK(rig.number("pos") == 100);
	CHECK(rig.number("slat") == 100);
	CHECK(rig.number("ud") == 1);
	rig.report("room", 25.5);
	CHECK(state(rig) == SunShading::heat_protection);
	rig.report("room", 24.9);
	CHECK(state(rig) == SunShading::idle);
	CHECK(rig.number("pos") == 0); // released once
	rig.report("room", 26.5);
	CHECK(state(rig) == SunShading::heat_protection);
	flag(rig, "pres", true);
	CHECK(state(rig) == SunShading::idle);
	flag(rig, "pres", false);
	flag(rig, "en", false); // the automatic off: nothing written, alarms stay
	CHECK(state(rig) == SunShading::off);
	flag(rig, "rain", true);
	CHECK(state(rig) == SunShading::rain);
}

TEST_CASE("SunShading: no two moves closer than minMoveInterval") {
	Rig rig(shading_block(R"("slatMode": "tracking", "brightnessOnDelay": 0, "brightnessOffDelay": 0,
				"minMoveInterval": 600000)", R"("brightness": "lux")"),
		nullptr, shading_datapoints());
	munich(rig, june21() + 6 * 3600);
	Moves moves(rig);
	hc_test::blocks::Rig* r = &rig;
	uint32_t state_lcg = 4711;
	for (uint64_t s = 1; s < 12 * 3600; s += 20) {
		r->run(s * 1000);
		state_lcg = state_lcg * 1103515245u + 12345u;
		r->report("lux", static_cast<double>((state_lcg >> 8) % 50000));
	}
	uint64_t previous = 0;
	bool first = true;
	uint64_t closest = UINT64_MAX;
	size_t move_count = 0;
	for (size_t i = 0; i < moves.list.size(); ++i) {
		if (i > 0 && moves.list[i].at == moves.list[i - 1].at) {
			continue; // the outputs of one move
		}
		++move_count;
		if (!first) {
			closest = std::min(closest, moves.list[i].at - previous);
		}
		first = false;
		previous = moves.list[i].at;
	}
	MESSAGE(move_count << " moves in 12 h of fluctuating brightness, the closest " << closest << " ms apart");
	CHECK(move_count > 10);
	CHECK(closest >= 600000);
}

TEST_CASE("SunShading: a start writes only what the automatic holds; the failsafe is retracted") {
	Rig rig(shading_block(R"("minMoveInterval": 0)", kAllInputs, R"("position": "pos", "slat": "slat", "upDown": "ud")"),
		nullptr, shading_datapoints());
	munich(rig, june21() + 22 * 3600); // night
	{
		Moves moves(rig);
		rig.reboot();
		rig.run(rig.now() + 30 * kMinute);
		CHECK(state(rig) == SunShading::idle);
		CHECK(moves.list.empty()); // a reboot at night leaves the closed blinds closed
	}
	rig.wall().start_utc = june21() + 10 * 3600 - static_cast<int64_t>(rig.now() / 1000);
	rig.report("lux", 60000.0);
	rig.run(rig.now() + 3 * kMinute);
	REQUIRE(state(rig) == SunShading::shading);
	const char* reason = nullptr;
	Moves moves(rig);
	// The instance's enable switch: the failsafe, retracted (D5).
	REQUIRE(rig.blocks().set_enabled("ss", false, &reason) == hc::blocks::ChangeStatus::ok);
	rig.run(rig.now());
	REQUIRE(moves.list.size() == 3);
	CHECK(rig.number("pos") == 0);
	CHECK(rig.number("slat") == 0);
	CHECK(rig.number("ud") == 0);
	// Enabled again, the start finds the sun and moves at once: all three outputs.
	moves.clear();
	REQUIRE(rig.blocks().set_enabled("ss", true, &reason) == hc::blocks::ChangeStatus::ok);
	rig.run(rig.now());
	CHECK(state(rig) == SunShading::shading);
	CHECK(moves.list.size() == 3);
	CHECK(rig.number("pos") == 100);
	CHECK(rig.number("ud") == 1);
}

TEST_CASE("SunShading: an output bound to one of the block's inputs is refused (B22)") {
	hc_test::blocks::Rig rig(shading_block(""), nullptr, shading_datapoints());
	hc::JsonSpan span;
	std::string text = shading_block("", R"("manual": ["sw", "ud"])", R"("upDown": "ud")");
	REQUIRE(hc::json_parse(text.data(), text.size(), span, nullptr, 48));
	struct NoOwners : hc::blocks::BlockDirectory {
		const char* owner(const char*) const override { return nullptr; }
	} owners;
	hc::blocks::BlockSet set;
	hc::blocks::BlockLoadResult result = set.load(span, rig.model(), owners, hc_test::corpus_registry());
	CHECK(!result.ok());
	CHECK(std::string(result.path) == "blocks[0].outputs.upDown");
	CHECK(std::string(hc::blocks::block_error_name(result.error)) == "forbidden");
	MESSAGE(result.text);
}

TEST_CASE("SunShading: no allocation in steady state; the cost of an instance is at most a room controller's") {
	Rig rig(shading_block(R"("slatMode": "tracking", "minMoveInterval": 0, "brightnessOnDelay": 0)", kAllInputs,
				R"("position": "pos", "slat": "slat", "upDown": "ud")"),
		nullptr, shading_datapoints());
	Rig room(R"([{"id": "wz", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "Room",
		"inputs": {"temperature": ["room", "out"], "window": "rain"}, "outputs": {"valve": "pos", "switch": "ud"}}])",
		nullptr, shading_datapoints());
	munich(rig, june21() + 8 * 3600);
	rig.report("lux", 50000.0);
	rig.run(rig.now() + 10 * kMinute);
	REQUIRE(state(rig) == SunShading::shading);
	{
		hc_test::AllocationScope scope;
		uint16_t lux = rig.model().find("lux")->id();
		uint16_t wind = rig.model().find("wind")->id();
		uint16_t sw = rig.model().find("sw")->id();
		size_t refused = 0;
		for (int i = 0; i < 120; ++i) {
			refused += rig.model().report(rig.technology(), lux, hc::Value::of_real(40000.0 + i * 100)) !=
				hc::DatapointStatus::ok;
			refused += rig.model().report(rig.technology(), wind, hc::Value::of_real(i % 30 == 0 ? 15.0 : 2.0)) !=
				hc::DatapointStatus::ok;
			if (i % 40 == 0) {
				refused += rig.model().report(rig.technology(), sw, hc::Value::of_bool(true)) != hc::DatapointStatus::ok;
			}
			rig.run(rig.now() + kMinute);
		}
		CHECK(refused == 0);
		CHECK(scope.count() == 0);
	}
	hc::blocks::BlockMemory ms = rig.blocks().memory();
	hc::blocks::BlockMemory mr = room.blocks().memory();
	auto mirrors = [](Rig& r) {
		size_t bytes = 0;
		for (size_t i = 0; i < r.model().size(); ++i) {
			const char* name = r.model().get(static_cast<uint16_t>(i))->name();
			if (std::strncmp(name, "block/", 6) == 0) {
				bytes += sizeof(hc::Datapoint) + std::strlen(name) + 1;
			}
		}
		return bytes;
	};
	size_t shading_heap = ms.runtime + ms.set + mirrors(rig);
	size_t room_heap = mr.runtime + mr.set + mirrors(room);
	MESSAGE("heap per instance (host, 64-bit): sun shading " << shading_heap << " B (runtime " << ms.runtime
		<< ", set " << ms.set << "), room controller " << room_heap << " B");
	CHECK(shading_heap <= room_heap);
}

TEST_CASE("SunShading probe: sensors one by one, and what is an operation") {
	Rig rig(shading_block(R"("minMoveInterval": 0, "brightnessOnDelay": 0, "windTimeout": 1800000)",
				R"("brightness": "lux", "wind": ["wind", "out"], "manual": ["sw", "sw2"])"),
		nullptr, shading_datapoints());
	munich(rig, june21() + 10 * 3600);
	rig.report("wind", 3.0);
	rig.report("out", 3.0);
	rig.report("lux", 60000.0);
	rig.run(11 * kMinute);
	REQUIRE(state(rig) == SunShading::shading);
	// A stale storm behind a good calm: the mirror shows the calm, the block sees the storm.
	rig.report("wind", 15.0);
	rig.stale("wind");
	rig.report("out", 2.0);
	CHECK(rig.number("block/ss/wind") == 2);
	CHECK(state(rig) == SunShading::wind);
	rig.report("wind", 1.0);
	rig.run(rig.now() + 10 * kMinute);
	CHECK(state(rig) == SunShading::shading);
	// One silent sensor of two: an alarm windTimeout after its last report, although the other reports.
	rig.report("out", 2.5);
	uint64_t last_out = rig.now();
	for (int i = 1; i <= 5; ++i) {
		rig.run(last_out + i * 5 * kMinute);
		rig.report("wind", 1.0 + i * 0.1);
	}
	rig.run(last_out + 30 * kMinute - 1);
	CHECK(state(rig) == SunShading::shading);
	rig.run(last_out + 30 * kMinute);
	CHECK(state(rig) == SunShading::wind);
	rig.report("out", 1.0);
	rig.report("wind", 1.0);
	rig.run(rig.now() + 10 * kMinute);
	REQUIRE(state(rig) == SunShading::shading);
	// A manual datapoint going stale is no operation; a new report is.
	flag(rig, "sw", true);
	CHECK(state(rig) == SunShading::manual);
	for (int i = 0; i < 4; ++i) { // keep the wind sensors alive through the lock
		rig.run(rig.now() + 25 * kMinute);
		rig.report("out", 1.0);
		rig.report("wind", 1.0);
	}
	rig.run(rig.now() + 25 * kMinute);
	REQUIRE(state(rig) == SunShading::shading);
	rig.stale("sw");
	CHECK(state(rig) == SunShading::shading);
	flag(rig, "sw2", false);
	CHECK(state(rig) == SunShading::manual);
}
