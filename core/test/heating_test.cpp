/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The heating demand (spec/blocks.md 16) and the heating curve (spec/blocks.md 17) on the device's
// block runtime: their behaviors over hours of simulated time, their costs. The contract cases of
// the same behaviors are in spec/fixtures/blocks/.
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "allocation_counter.h"
#include "block_rig.h"
#include "doctest.h"
#include "hc/blocks/block_runtime.h"
#include "hc/blocks/heating_curve.h"
#include "hc/blocks/heating_demand.h"
#include "hc/blocks/room_controller.h"

using hc::blocks::HeatingCurve;
using hc::blocks::HeatingDemand;
using hc::blocks::RoomController;

namespace {

using hc_test::blocks::kHour;
using hc_test::blocks::kMinute;
using hc_test::blocks::Rig;

using Points = std::vector<std::pair<std::string, hc::ValueType>>;

Points heating_datapoints() {
	Points p;
	for (int i = 1; i <= 8; ++i) {
		p.emplace_back("v" + std::to_string(i), hc::ValueType::real);
	}
	for (int i = 1; i <= 4; ++i) {
		p.emplace_back("d" + std::to_string(i), hc::ValueType::boolean);
	}
	for (const char* b : {"en", "su", "req", "pump", "csw", "csum"}) {
		p.emplace_back(b, hc::ValueType::boolean);
	}
	for (const char* r : {"lvl", "ofs", "out1", "out2", "rs", "flow", "temp"}) {
		p.emplace_back(r, hc::ValueType::real);
	}
	return p;
}

std::string demand_block(const std::string& parameters,
	const std::string& inputs = R"("valves": ["v1", "v2", "v3", "v4"])") {
	return R"([{"id": "hd", "type": "org.homecontrol.heating-demand", "version": "1.0", "label": "Demand",
		"inputs": {)" + inputs + R"(}, "outputs": {"request": "req", "pump": "pump", "level": "lvl",
		"flowSetpointOffset": "ofs"}, "parameters": {)" + parameters + "}}]";
}

std::string curve_block(const std::string& parameters, const std::string& inputs = R"("outside": "out1")") {
	return R"([{"id": "hk", "type": "org.homecontrol.heating-curve", "version": "1.0", "label": "Curve",
		"inputs": {)" + inputs + R"(}, "outputs": {"flowSetpoint": "flow", "summer": "csum"},
		"parameters": {)" + parameters + "}}]";
}

/// A deterministic pseudo-random sequence (a linear congruential generator).
struct Lcg {
	uint32_t state = 12345;
	double next() {
		state = state * 1103515245u + 12345u;
		return static_cast<double>((state >> 8) & 0xffff) / 65535.0;
	}
};

/// The heap of the first instance of a rig: its runtime and compiled spec, and its mirror datapoints
/// (each the model's datapoint object and its name).
size_t instance_heap(Rig& rig) {
	hc::blocks::BlockMemory m = rig.blocks().memory();
	size_t mirrors = 0;
	for (size_t i = 0; i < rig.model().size(); ++i) {
		const char* name = rig.model().get(static_cast<uint16_t>(i))->name();
		if (std::strncmp(name, "block/", 6) == 0) {
			mirrors += sizeof(hc::Datapoint) + std::strlen(name) + 1;
		}
	}
	return m.runtime + m.set + mirrors;
}

/// Reports the outside temperature and runs to the curve's next period (its trigger is `none`).
void outside(Rig& rig, double value) {
	rig.report("out1", value);
	rig.run(rig.now() + kMinute);
}

} // namespace

TEST_CASE("Heating: the manifests are valid and the types are built in") {
	CHECK(hc::blocks::check_manifest(HeatingDemand::instance().manifest()) == nullptr);
	CHECK(hc::blocks::check_manifest(HeatingCurve::instance().manifest()) == nullptr);
	CHECK(hc::blocks::BlockRegistry::device().find("org.homecontrol.heating-demand", 1) == &HeatingDemand::instance());
	CHECK(hc::blocks::BlockRegistry::device().find("org.homecontrol.heating-curve", 1) == &HeatingCurve::instance());
}

TEST_CASE("HeatingDemand: hysteresis and the minimum times under a fluctuating demand") {
	Rig rig(demand_block(R"("minOn": 600000, "minOff": 300000, "pumpRunOn": 0)"), nullptr, heating_datapoints());
	Lcg random;
	bool last = rig.number("req") != 0;
	uint64_t switched = 0;
	bool any_switch = false;
	size_t switches = 0;
	uint64_t shortest_on = UINT64_MAX;
	uint64_t shortest_off = UINT64_MAX;
	bool wrong_side = false;
	for (uint64_t s = 1; s < 8 * 3600; ++s) {
		// Every 20 s the valve moves to a random position around the thresholds (the other three
		// are unknown: ignored).
		rig.run(s * 1000);
		if (s % 20 == 0) {
			rig.report("v1", std::floor(random.next() * 30.0));
		}
		bool request = rig.number("req") != 0;
		double level = rig.number("lvl");
		if (request != last) {
			uint64_t t = rig.now();
			if (any_switch) {
				uint64_t held = t - switched;
				(last ? shortest_on : shortest_off) = std::min(last ? shortest_on : shortest_off, held);
			}
			any_switch = true;
			switched = t;
			last = request;
			++switches;
		}
		// The request never contradicts the hysteresis after the minimum times have passed.
		if (any_switch && rig.now() - switched >= 600000 && ((request && level <= 5) || (!request && level >= 20))) {
			wrong_side = true;
		}
		CHECK(rig.number("pump") == rig.number("req")); // no run-on
	}
	MESSAGE("switches " << switches << ", shortest on " << shortest_on << " ms, shortest off " << shortest_off << " ms");
	CHECK(switches >= 6);
	CHECK(shortest_on >= 600000);
	CHECK(shortest_off >= 300000);
	CHECK_FALSE(wrong_side);
}

TEST_CASE("HeatingDemand: a held switching happens when its minimum time expires; the pump runs on") {
	Rig rig(demand_block(R"("minOn": 300000, "minOff": 300000, "pumpRunOn": 180000)",
				R"("valves": ["v1", "v2", "v3", "v4"], "enable": "en")"),
		nullptr, heating_datapoints());
	rig.report("v1", 50.0);
	CHECK(rig.number("req") == 1);
	CHECK(rig.number("pump") == 1);
	uint64_t on_at = rig.now();
	rig.run(on_at + kMinute);
	rig.report("v1", 0.0);
	CHECK(rig.number("req") == 1); // held by minOn
	CHECK(rig.number("block/hd/state") == HeatingDemand::held_on);
	rig.run(on_at + 300000 - 1);
	CHECK(rig.number("req") == 1);
	rig.run(on_at + 300000);
	CHECK(rig.number("req") == 0); // at the expiry, by the timer
	CHECK(rig.number("pump") == 1);
	rig.run(on_at + 300000 + 180000 - 1);
	CHECK(rig.number("pump") == 1);
	rig.run(on_at + 300000 + 180000);
	CHECK(rig.number("pump") == 0); // the run-on ended at its time
	// On wanted again within minOff (from the switching off at 300 s): held off, switched at its expiry.
	rig.report("v1", 60.0);
	CHECK(rig.number("req") == 0);
	CHECK(rig.number("block/hd/state") == HeatingDemand::held_off);
	rig.run(on_at + 600000);
	CHECK(rig.number("req") == 1);
	// enable and summer switch off, respecting minOn too.
	rig.run(on_at + 600000 + kMinute);
	rig.report("en", hc::Value::of_bool(false));
	CHECK(rig.number("req") == 1);
	rig.run(on_at + 900000);
	CHECK(rig.number("req") == 0);
	CHECK(rig.number("block/hd/state") == HeatingDemand::disabled);
}

TEST_CASE("HeatingDemand: the level is the highest or the mean of the highest; a stale input per staleInput") {
	SUBCASE("max and topMean") {
		Rig rig(demand_block(R"("mode": "topMean", "topCount": 3)",
					R"("valves": ["v1", "v2", "v3", "v4"], "demands": ["d1"])"),
			nullptr, heating_datapoints());
		rig.report("v1", 80.0);
		rig.report("v2", 10.0);
		rig.report("v3", 60.0);
		rig.report("v4", 40.0);
		rig.report("d1", hc::Value::of_bool(false));
		CHECK(rig.number("lvl") == doctest::Approx((80.0 + 60.0 + 40.0) / 3.0));
		rig.report("d1", hc::Value::of_bool(true)); // a true demand counts as a valve at 100 %
		CHECK(rig.number("lvl") == doctest::Approx((100.0 + 80.0 + 60.0) / 3.0));
		rig.report("v1", 150.0); // clamped to 100
		CHECK(rig.number("lvl") == doctest::Approx((100.0 + 100.0 + 60.0) / 3.0));
		CHECK(rig.number("block/hd/valves") == 150.0); // the mirror: the port's max aggregation
	}
	SUBCASE("a stale valve is ignored") {
		Rig rig(demand_block(R"("staleInput": "ignore", "minOn": 0, "minOff": 0)", R"("valves": ["v1", "v2"])"), nullptr,
			heating_datapoints());
		rig.report("v1", 70.0);
		rig.report("v2", 30.0);
		CHECK(rig.number("lvl") == 70.0);
		rig.stale("v1");
		CHECK(rig.number("lvl") == 30.0);
		CHECK(rig.number("block/hd/staleInputs") == 1);
		rig.stale("v2");
		CHECK(rig.number("lvl") == 0.0); // nothing good: no demand
		CHECK(rig.number("req") == 0);
	}
	SUBCASE("a stale valve counts as fully open") {
		Rig rig(demand_block(R"("staleInput": "full")", R"("valves": ["v1", "v2"])"), nullptr, heating_datapoints());
		rig.report("v1", 0.0);
		rig.report("v2", 0.0);
		CHECK(rig.number("lvl") == 0.0);
		rig.stale("v2");
		CHECK(rig.number("lvl") == 100.0);
		CHECK(rig.number("req") == 1);
	}
	SUBCASE("an unknown valve counts like a stale one") {
		Rig rig(demand_block(R"("staleInput": "full", "minOn": 0, "minOff": 0)", R"("valves": ["v1", "v2"])"), nullptr,
			heating_datapoints());
		CHECK(rig.number("lvl") == 100.0); // nothing reported yet
		CHECK(rig.number("block/hd/staleInputs") == 2);
		rig.report("v1", 0.0);
		rig.report("v2", 0.0);
		CHECK(rig.number("lvl") == 0.0);
		CHECK(rig.number("req") == 0);
	}
}

TEST_CASE("HeatingDemand: the offset rises under a sustained high demand and falls again") {
	Rig rig(demand_block(R"("targetLevel": 70, "offsetGain": 0.1, "maxOffset": 5)"), nullptr, heating_datapoints());
	rig.report("v1", 100.0);
	uint64_t t0 = rig.now();
	rig.run(t0 + kHour);
	// 30 % above the target for an hour: 3 K (the level held since the first step).
	CHECK(rig.number("block/hd/flowSetpointOffset") == doctest::Approx(3.0).epsilon(1e-9));
	CHECK(std::fabs(rig.number("ofs") - 3.0) < 0.1); // sent in steps of 0.1 K
	rig.run(t0 + 3 * kHour);
	CHECK(rig.number("block/hd/flowSetpointOffset") == 5.0); // clamped at maxOffset
	CHECK(rig.number("ofs") == 5.0);
	rig.report("v1", 50.0); // 20 % below the target: falls by 2 K/h
	uint64_t t1 = rig.now();
	rig.run(t1 + kHour);
	CHECK(rig.number("block/hd/flowSetpointOffset") == doctest::Approx(3.0).epsilon(1e-9));
	rig.run(t1 + 4 * kHour);
	CHECK(rig.number("block/hd/flowSetpointOffset") == 0.0);
	CHECK(rig.number("ofs") == 0.0);
}

TEST_CASE("HeatingDemand: the offset survives a reboot") {
	hc_test::FakeBlobStore blobs;
	Rig rig(demand_block(R"("offsetGain": 0.1)"), &blobs, heating_datapoints());
	rig.report("v1", 100.0);
	rig.run(rig.now() + kHour);
	double offset = rig.field(HeatingDemand::kOffset);
	CHECK(offset == doctest::Approx(3.0));
	rig.reboot();
	CHECK(rig.field(HeatingDemand::kOffset) == offset);
	hc::blocks::BlockDiagnostics d;
	REQUIRE(rig.blocks().diagnostics(0, d));
	CHECK(d.restored);
}

TEST_CASE("HeatingDemand: room controllers feed it through their mirrors; it feeds a heating curve") {
	// Two rooms, the demand on their valves, the curve on the demand's offset and the outside.
	std::string blocks = R"([
		{"id": "r1", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "R1",
			"inputs": {"temperature": "v7"}},
		{"id": "r2", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "R2",
			"inputs": {"temperature": "v8"}},
		{"id": "hd", "type": "org.homecontrol.heating-demand", "version": "1.0", "label": "Demand",
			"inputs": {"valves": ["block/r1/valve", "block/r2/valve"], "summer": "block/hk/summer"},
			"outputs": {"request": "req"}, "parameters": {"offsetGain": 1, "targetLevel": 20}},
		{"id": "hk", "type": "org.homecontrol.heating-curve", "version": "1.0", "label": "Curve",
			"inputs": {"outside": "out1", "offset": "block/hd/flowSetpointOffset"}, "outputs": {"flowSetpoint": "flow"},
			"parameters": {"outsideFilter": 0, "minChange": 0}}])";
	Rig rig(blocks, nullptr, heating_datapoints());
	rig.report("out1", 0.0);
	rig.report("v7", 18.0); // 3 K below comfort: the valve opens
	rig.report("v8", 21.0);
	CHECK(rig.number("block/hd/level") > 20);
	CHECK(rig.number("req") == 1);
	double before = rig.number("flow");
	CHECK(before == doctest::Approx(20 + 1.2 * 20)); // slope 1.2, room 20, outside 0
	rig.run(rig.now() + kHour);
	CHECK(rig.number("block/hd/flowSetpointOffset") > 1);
	CHECK(rig.number("flow") > before); // the demand raised the flow
	// Summer from the curve switches the request off (an external switch, as a test shortcut).
	CHECK(rig.number("block/hk/summer") == 0);
}

TEST_CASE("HeatingCurve: the curve's forms, its points and the interpolation") {
	SUBCASE("slope and level, the room setpoint") {
		Rig rig(curve_block(R"("outsideFilter": 0, "slope": 1.5, "level": 2, "minChange": 0, "maxFlow": 90)",
					R"("outside": "out1", "roomSetpoint": "rs")"),
			nullptr, heating_datapoints());
		outside(rig, -10.0);
		CHECK(rig.number("flow") == 20.0 + 2.0 + 1.5 * (20.0 - -10.0));
		rig.report("rs", 22.0);
		CHECK(rig.number("flow") == 22.0 + 2.0 + 1.5 * (22.0 - -10.0));
		rig.set("slope", 1.0); // adjustable
		CHECK(rig.number("flow") == 22.0 + 2.0 + 1.0 * (22.0 - -10.0));
	}
	SUBCASE("the design point") {
		Rig rig(curve_block(R"("curve": "design", "outsideFilter": 0, "designOutside": -12, "designFlow": 55, "minChange": 0)"),
			nullptr, heating_datapoints());
		outside(rig, -12.0);
		CHECK(rig.number("flow") == doctest::Approx(55.0).epsilon(1e-12));
		outside(rig, 4.0);
		CHECK(rig.number("flow") == 20.0 + 0.0 + (55.0 - 20.0) / (20.0 - -12.0) * (20.0 - 4.0));
	}
	SUBCASE("points") {
		Rig rig(curve_block(R"("curve": "points", "outsideFilter": 0, "minChange": 0, "minFlow": 15, "maxFlow": 70)",
					R"("outside": "out1", "roomSetpoint": "rs")"),
			nullptr, heating_datapoints());
		const double xs[] = {-15, -10, -5, 0, 10, 20};
		const double ys[] = {60, 55, 49, 43, 32, 20};
		for (size_t i = 0; i < 6; ++i) {
			outside(rig, xs[i]);
			CHECK(rig.number("flow") == ys[i]);
		}
		outside(rig, -30.0);
		CHECK(rig.number("flow") == 60.0); // flat beyond the first point
		outside(rig, 30.0);
		CHECK(rig.number("flow") == 20.0); // and beyond the last
		outside(rig, 5.0);
		CHECK(rig.number("flow") == 43.0 + (32.0 - 43.0) * ((5.0 - 0.0) / (10.0 - 0.0)));
		outside(rig, -7.5);
		CHECK(rig.number("flow") == 55.0 + (49.0 - 55.0) * ((-7.5 - -10.0) / (-5.0 - -10.0)));
		// A room 2 K warmer: the flow of an outside 2 K colder, plus 2 K.
		rig.report("rs", 22.0);
		CHECK(rig.number("flow") == 55.0 + (49.0 - 55.0) * ((-9.5 - -10.0) / (-5.0 - -10.0)) + 2.0);
	}
	SUBCASE("cross-parameter checks") {
			double p[HeatingCurve::kParamCount];
		const hc::blocks::Manifest& m = HeatingCurve::instance().manifest();
		for (size_t i = 0; i < m.param_count; ++i) {
			p[i] = m.params[i].default_value;
		}
		size_t blamed = 99;
		CHECK(HeatingCurve::instance().check(p, blamed) == nullptr);
		p[HeatingCurve::kCurve] = HeatingCurve::points;
		p[HeatingCurve::kOutside3] = -10;
		CHECK(HeatingCurve::instance().check(p, blamed) != nullptr);
		CHECK(blamed == HeatingCurve::kOutside3);
		p[HeatingCurve::kPointCount] = 2; // only the first two points count
		CHECK(HeatingCurve::instance().check(p, blamed) == nullptr);
		p[HeatingCurve::kMinFlow] = 60;
		CHECK(HeatingCurve::instance().check(p, blamed) != nullptr);
		CHECK(blamed == HeatingCurve::kMinFlow);
	}
}

TEST_CASE("HeatingCurve: the filter lags a ramp of the outside temperature by its time constant") {
	Rig rig(curve_block(R"("outsideFilter": 43200000)"), nullptr, heating_datapoints());
	// -0.25 K per hour, reported every 10 minutes, for 4 days.
	const double rate = -0.25 / 3600.0;
	for (uint64_t s = 0; s <= 4 * 86400; s += 600) {
		rig.run(s * 1000);
		rig.report("out1", 10.0 + rate * static_cast<double>(s));
	}
	double x = 10.0 + rate * 4 * 86400;
	double lag_h = (rig.number("block/hk/outsideFiltered") - x) / (-rate) / 3600.0;
	MESSAGE("lag " << lag_h << " h");
	CHECK(lag_h > 11.8);
	CHECK(lag_h < 12.2);
}

TEST_CASE("HeatingCurve: the summer switch needs consecutive days above, with hysteresis; it survives a reboot") {
	hc_test::FakeBlobStore blobs;
	Rig rig(curve_block(R"("outsideFilter": 0, "summerOn": 18, "summerOff": 15, "summerDays": 2)"), &blobs,
		heating_datapoints());
	auto hold = [&](double value, uint64_t ms) {
		uint64_t end = rig.now() + ms;
		while (rig.now() < end) {
			rig.report("out1", value);
			rig.run(std::min(end, rig.now() + 30 * kMinute));
		}
	};
	hold(20, 47 * kHour);
	CHECK(rig.number("csum") == 0);
	hold(17, kHour); // not above summerOn: the count starts again
	CHECK(rig.field(HeatingCurve::kAboveFor) == 0);
	hold(20, 47 * kHour);
	CHECK(rig.number("csum") == 0);
	rig.reboot(); // the count survives
	CHECK(rig.field(HeatingCurve::kAboveFor) >= 46.5 * 3600);
	hold(20, 2 * kHour);
	CHECK(rig.number("csum") == 1);
	CHECK(rig.number("block/hk/state") == HeatingCurve::summer);
	CHECK(rig.number("flow") == 0); // offFlow
	hold(16, 10 * kHour); // between summerOff and summerOn: summer stays
	CHECK(rig.number("csum") == 1);
	hold(14.5, kMinute);
	CHECK(rig.number("csum") == 0); // below summerOff: winter at once
	CHECK(rig.number("block/hk/state") == HeatingCurve::heating);
}

TEST_CASE("HeatingCurve: frost protection holds the lowest flow in summer and when off") {
	Rig rig(curve_block(R"("outsideFilter": 0, "frostOutside": 3, "minFlow": 25, "offFlow": 0)",
				R"("outside": "out1", "summerSwitch": "csw", "enable": "en")"),
		nullptr, heating_datapoints());
	rig.report("csw", hc::Value::of_bool(true));
	outside(rig, 5.0);
	CHECK(rig.number("flow") == 0);
	CHECK(rig.number("block/hk/state") == HeatingCurve::summer);
	outside(rig, 2.5);
	CHECK(rig.number("flow") == 25);
	CHECK(rig.number("block/hk/state") == HeatingCurve::frost);
	outside(rig, 3.5); // within the 1 K hysteresis
	CHECK(rig.number("flow") == 25);
	outside(rig, 4.0);
	CHECK(rig.number("flow") == 0);
	rig.report("csw", hc::Value::of_bool(false));
	rig.report("en", hc::Value::of_bool(false));
	CHECK(rig.number("block/hk/state") == HeatingCurve::off);
	outside(rig, -5.0);
	CHECK(rig.number("flow") == 25);
	CHECK(rig.number("block/hk/state") == HeatingCurve::frost);
}

TEST_CASE("HeatingCurve: a stale or silent outside temperature holds the filtered value, fault after the timeout") {
	SUBCASE("stale") {
		Rig rig(curve_block(R"("outsideFilter": 3600000, "sensorTimeout": 3600000, "minChange": 0)"), nullptr,
			heating_datapoints());
		outside(rig, 0.0);
		rig.run(rig.now() + 2 * kHour);
		uint64_t updated = rig.now();
		outside(rig, 10.0);
		rig.run(rig.now() + 30 * kMinute);
		double held = rig.number("block/hk/outsideFiltered");
		rig.stale("out1");
		rig.run(updated + kHour - kMinute);
		CHECK(rig.number("block/hk/outsideFiltered") == held); // not fed any more
		CHECK(rig.number("block/hk/state") == HeatingCurve::heating);
		rig.run(updated + kHour + kMinute);
		CHECK(rig.number("block/hk/state") == HeatingCurve::fault);
		CHECK(rig.number("block/hk/outsideFiltered") == held);
		CHECK(rig.number("flow") == doctest::Approx(20 + 1.2 * (20 - held)));
		outside(rig, 10.0);
		CHECK(rig.number("block/hk/state") == HeatingCurve::heating);
	}
	SUBCASE("silent") {
		Rig rig(curve_block(R"("outsideFilter": 3600000, "sensorTimeout": 3600000)"), nullptr, heating_datapoints());
		uint64_t updated = rig.now();
		outside(rig, 5.0);
		rig.run(updated + kHour - kMinute);
		CHECK(rig.number("block/hk/state") == HeatingCurve::heating);
		rig.run(updated + kHour + 2 * kMinute);
		CHECK(rig.number("block/hk/state") == HeatingCurve::fault);
		CHECK(rig.number("block/hk/outsideFiltered") == 5.0);
	}
}

TEST_CASE("Heating: no allocation in steady state; the cost of an instance is at most a room controller's") {
	Rig demand(demand_block(R"("mode": "topMean")",
				   R"("valves": ["v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8"], "demands": ["d1", "d2", "d3", "d4"])"),
		nullptr, heating_datapoints());
	Rig curve(curve_block("", R"("outside": ["out1", "out2"])"), nullptr, heating_datapoints());
	Rig room(R"([{"id": "wz", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "Room",
		"inputs": {"temperature": ["temp", "v1"], "window": "d1"}, "outputs": {"valve": "lvl", "switch": "req"}}])",
		nullptr, heating_datapoints());
	demand.report("v1", 100.0); // the offset rises: the record is stored before the measurement
	curve.report("out1", 5.0);
	curve.report("out2", 6.0);
	demand.run(demand.now() + 10 * kMinute);
	curve.run(curve.now() + 10 * kMinute);
	{
		hc_test::AllocationScope scope;
		auto writes = demand.blobs().writes + curve.blobs().writes;
		uint16_t v[8];
		for (int i = 0; i < 8; ++i) {
			v[i] = demand.model().find(("v" + std::to_string(i + 1)).c_str())->id();
		}
		uint16_t d1 = demand.model().find("d1")->id();
		uint16_t out1 = curve.model().find("out1")->id();
		size_t refused = 0;
		for (int i = 0; i < 60; ++i) {
			refused += demand.model().report(demand.technology(), v[i % 8], hc::Value::of_real((i * 7) % 100)) !=
				hc::DatapointStatus::ok;
			refused += demand.model().report(demand.technology(), d1, hc::Value::of_bool(i % 3 == 0)) !=
				hc::DatapointStatus::ok;
			refused += curve.model().report(curve.technology(), out1, hc::Value::of_real(5.0 + i * 0.1)) !=
				hc::DatapointStatus::ok;
			demand.run(demand.now() + kMinute);
			curve.run(curve.now() + kMinute);
		}
		size_t stored = static_cast<size_t>(demand.blobs().writes + curve.blobs().writes - writes);
		CHECK(refused == 0);
		CHECK(stored >= 1);
		CHECK(scope.count() - stored == 0); // the fake store allocates for each record it keeps
	}
	size_t demand_heap = instance_heap(demand);
	size_t curve_heap = instance_heap(curve);
	size_t room_heap = instance_heap(room);
	MESSAGE("heap per instance (host, 64-bit): heating demand " << demand_heap << " B, heating curve " << curve_heap
		<< " B, room controller " << room_heap << " B");
	CHECK(demand_heap <= room_heap);
	CHECK(curve_heap <= room_heap);
}
