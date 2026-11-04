/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The PV surplus control (spec/blocks.md 20) on the device's block runtime, driven by a simulated
// installation: a grid meter that reports the PV generation minus the house's base load and the loads
// that are on, 5 s after each period. The contract cases of the same behaviors are in
// spec/fixtures/blocks/.
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "allocation_counter.h"
#include "block_rig.h"
#include "doctest.h"
#include "hc/blocks/block_runtime.h"
#include "hc/blocks/pv_surplus.h"
#include "hc/tz.h"

using hc::blocks::PvSurplus;

namespace {

using hc_test::blocks::kHour;
using hc_test::blocks::kMinute;
using hc_test::blocks::Rig;

using Points = std::vector<std::pair<std::string, hc::ValueType>>;

Points pv_datapoints() {
	Points p;
	for (const char* r : {"grid", "pv", "house", "batt", "soc", "sp1", "sp2", "low", "temp", "temp2", "valve"}) {
		p.emplace_back(r, hc::ValueType::real);
	}
	for (const char* b : {"en", "r1", "r2", "s1", "s2", "sw"}) {
		p.emplace_back(b, hc::ValueType::boolean);
	}
	return p;
}

/// An instance `id` with the grid meter and every output of the loads bound.
std::string instance(const std::string& id, const std::string& parameters, const std::string& inputs,
	const std::string& outputs) {
	return R"({"id": ")" + id + R"(", "type": "org.homecontrol.pv-surplus", "version": "1.0", "label": "PV",
		"inputs": {)" + inputs + R"(}, "outputs": {)" + outputs + R"(}, "parameters": {)" + parameters + "}}";
}

constexpr const char* kOutputs = R"("switch1": "s1", "setpoint1": "sp1", "switch2": "s2", "setpoint2": "sp2")";

/// Two switched loads of 2000 and 1000 W without minimum times or filter.
constexpr const char* kTwo = R"("surplusFilter": 0, "power1": 2000, "minOn1": 0, "minOff1": 0,
	"power2": 1000, "minOn2": 0, "minOff2": 0)";

Rig rig_of(const std::string& parameters, const std::string& inputs = R"("gridPower": "grid")",
	hc_test::FakeBlobStore* blobs = nullptr) {
	return Rig("[" + instance("pv", parameters, inputs, kOutputs) + "]", blobs, pv_datapoints());
}

double level(Rig& rig, size_t k) { return rig.field(PvSurplus::kLevel1 + PvSurplus::kFieldsPerLoad * k); }
double run_of(Rig& rig, size_t k) { return rig.field(PvSurplus::kRun1 + PvSurplus::kFieldsPerLoad * k); }
double state_of(Rig& rig, size_t k) { return rig.number(("block/pv/state" + std::to_string(k + 1)).c_str()); }

/// The installation: PV generation, the house's base load, the loads at the power of their setpoints
/// (the mirrors: what the block last wrote, 0 after the failsafe). The grid meter (import positive)
/// reports every 10 s, 5 s after the periods of the block.
struct House {
	Rig& rig;
	double pv = 0;
	double base = 300;
	double draw = 1;          ///< The share of its setpoint a load draws.
	double scale = 1;         ///< W per unit of the setpoints (the power / 100 with `percent`).
	bool battery = false;     ///< A battery that takes the export up to `charge_max` (charging positive).
	double charge_max = 0;

	double loads() {
		return (rig.number("block/pv/setpoint1") + rig.number("block/pv/setpoint2")) * scale * draw;
	}
	void report() {
		double grid = base + loads() - pv;
		if (battery) {
			double charge = std::min(std::max(-grid, 0.0), charge_max);
			rig.report("batt", charge);
			grid += charge;
		}
		rig.report("grid", grid);
	}
	/// Runs to `ms`, the meter reporting at 5, 15, 25, ... s.
	void until(uint64_t ms) {
		for (;;) {
			uint64_t next = (rig.now() / 10000) * 10000 + 5000;
			if (next <= rig.now()) {
				next += 10000;
			}
			if (next > ms) {
				break;
			}
			rig.run(next);
			report();
		}
		rig.run(ms);
	}
	void for_(uint64_t ms) { until(rig.now() + ms); }
};

} // namespace

TEST_CASE("PvSurplus: the manifest is valid and within the limits; the type is built in") {
	const hc::blocks::Manifest& m = PvSurplus::instance().manifest();
	CHECK(hc::blocks::check_manifest(m) == nullptr);
	CHECK(hc::blocks::BlockRegistry::device().find("org.homecontrol.pv-surplus", 1) == &PvSurplus::instance());
	CHECK(m.port_count == 19);
	CHECK(m.param_count == 24);
	CHECK(m.state_count == 15);
	CHECK(m.persists());
	CHECK(std::strcmp(m.ports[PvSurplus::kSwitch1 + 4].id, "switch2") == 0);
	CHECK(std::strcmp(m.ports[PvSurplus::kEnergy1 + 4].id, "energy2") == 0);
	CHECK(std::strcmp(m.ports[PvSurplus::kSurplus].id, "surplus") == 0);
	CHECK(std::strcmp(m.params[PvSurplus::kPower1 + 8 + 7].id, "mustRunFor2") == 0);
	CHECK(std::strcmp(m.state[PvSurplus::kEnergyField1 + 4].id, "energy2") == 0);
}

TEST_CASE("PvSurplus: the cross-parameter checks blame minPower and mustRunFor (B17)") {
	const hc::blocks::Manifest& m = PvSurplus::instance().manifest();
	std::vector<double> p(m.param_count);
	for (size_t i = 0; i < p.size(); ++i) {
		p[i] = m.params[i].default_value;
	}
	size_t blamed = 0;
	CHECK(PvSurplus::instance().check(p.data(), blamed) == nullptr);
	p[PvSurplus::kPower1 + 8 + 0] = 3000; // load 2 modulated without a lowest power
	p[PvSurplus::kPower1 + 8 + 2] = 1000;
	REQUIRE(PvSurplus::instance().check(p.data(), blamed) != nullptr);
	CHECK(std::strcmp(m.params[blamed].id, "minPower2") == 0);
	p[PvSurplus::kPower1 + 8 + 1] = 3500; // above its power
	REQUIRE(PvSurplus::instance().check(p.data(), blamed) != nullptr);
	CHECK(std::strcmp(m.params[blamed].id, "minPower2") == 0);
	p[PvSurplus::kPower1 + 8 + 1] = 1000;
	CHECK(PvSurplus::instance().check(p.data(), blamed) == nullptr);
	p[PvSurplus::kPower1 + 5] = 3600000; // load 1: at most 1 h a day, must run 2 h
	p[PvSurplus::kPower1 + 7] = 7200000;
	REQUIRE(PvSurplus::instance().check(p.data(), blamed) != nullptr);
	CHECK(std::strcmp(m.params[blamed].id, "mustRunFor1") == 0);
	p[PvSurplus::kPower1 + 5] = 0; // no daily maximum: any must-run
	CHECK(PvSurplus::instance().check(p.data(), blamed) == nullptr);
}

TEST_CASE("PvSurplus: the loads go on by priority as the surplus grows; a higher load takes a lower one's place; "
		  "the lowest is shed first") {
	Rig rig = rig_of(kTwo);
	House house{rig};
	house.until(kMinute);
	CHECK(state_of(rig, 0) == PvSurplus::waiting);
	CHECK(rig.number("s1") == 0);
	CHECK(rig.number("block/pv/surplus") == -300);

	house.pv = 300 + 1150; // only load 2 fits with onMargin 100
	house.for_(kMinute);
	CHECK(level(rig, 0) == 0);
	CHECK(level(rig, 1) == 1000);
	CHECK(rig.number("s2") == 1);
	CHECK(rig.number("sp2") == 1000);
	house.pv = 300 + 2150; // load 1 fits on its budget: load 2 makes room
	house.for_(kMinute);
	CHECK(level(rig, 0) == 2000);
	CHECK(level(rig, 1) == 0);
	CHECK(rig.number("block/pv/loadPower") == 2000);
	CHECK(rig.field(PvSurplus::kFiltered) == 2150);
	CHECK(rig.number("block/pv/surplus") == 150); // what the loads leave
	house.pv = 300 + 3200; // both
	house.for_(kMinute);
	CHECK(level(rig, 0) == 2000);
	CHECK(level(rig, 1) == 1000);
	CHECK(state_of(rig, 0) == PvSurplus::running);

	house.pv = 300 + 2950; // 50 W from the grid: within allowedImport
	house.for_(kMinute);
	CHECK(level(rig, 1) == 1000);
	house.pv = 300 + 2850; // 150 W: the lowest is shed (its budget is the export)
	house.for_(kMinute);
	CHECK(level(rig, 0) == 2000);
	CHECK(level(rig, 1) == 0);
	house.pv = 300 + 1850; // load 1 is shed, and load 2 fits in what it left
	house.for_(kMinute);
	CHECK(level(rig, 0) == 0);
	CHECK(level(rig, 1) == 1000);
}

TEST_CASE("PvSurplus: the hysteresis of onMargin and allowedImport") {
	Rig rig = rig_of(R"("surplusFilter": 0, "onMargin": 200, "allowedImport": 150, "power1": 1000, "minOn1": 0,
		"minOff1": 0)");
	House house{rig};
	house.pv = 300 + 1199; // 1 W short of the margin
	house.for_(kMinute);
	CHECK(level(rig, 0) == 0);
	house.pv = 300 + 1200;
	house.for_(kMinute);
	CHECK(level(rig, 0) == 1000);
	house.pv = 300 + 850; // 150 W from the grid: stays
	house.for_(5 * kMinute);
	CHECK(level(rig, 0) == 1000);
	house.pv = 300 + 849;
	house.for_(kMinute);
	CHECK(level(rig, 0) == 0);
}

TEST_CASE("PvSurplus: the minimum times hold a load under a cloud flicker; the filter rides it out") {
	for (double filter : {0.0, 300000.0}) {
		CAPTURE(filter);
		Rig rig = rig_of(R"("surplusFilter": )" + std::to_string(static_cast<int>(filter)) +
			R"(, "power1": 2000, "minOn1": 600000, "minOff1": 300000)");
		House house{rig};
		std::vector<uint64_t> switched;
		double last = 0;
		auto watch = [&](uint64_t until) {
			while (rig.now() < until) {
				house.for_(1000);
				double on = rig.number("s1");
				if (on != last) {
					switched.push_back(rig.now());
					last = on;
				}
			}
		};
		house.pv = 300 + 3000;
		watch(5 * kMinute);
		REQUIRE(switched.size() == 1);
		for (int cloud = 0; cloud < 12; ++cloud) { // a cloud of 60 s every 3 min: 2500 W less
			house.pv = 300 + 500;
			watch(rig.now() + kMinute);
			house.pv = 300 + 3000;
			watch(rig.now() + 2 * kMinute);
		}
		if (filter == 0) {
			CHECK(switched.size() > 2);
			for (size_t i = 1; i < switched.size(); ++i) {
				CHECK(switched[i] - switched[i - 1] >= (i % 2 == 1 ? 600000u : 300000u));
			}
		} else {
			CHECK(switched.size() == 1); // the 5-min filter keeps the budget above 1900 W
			CHECK(rig.field(PvSurplus::kFiltered) > 1900);
		}
	}
}

TEST_CASE("PvSurplus: a modulated load follows the surplus in steps, in W and in %") {
	for (int percent = 0; percent <= 1; ++percent) {
		CAPTURE(percent);
		Rig rig = rig_of(std::string(R"("surplusFilter": 0, "setpointUnit": )") + (percent ? "\"percent\"" : "\"watt\"") +
			R"(, "power1": 3680, "minPower1": 1380, "step1": 230, "minOn1": 0, "minOff1": 0)");
		House house{rig};
		house.scale = percent ? 36.8 : 1;
		auto check = [&](double export_w, double expected) {
			house.pv = 300 + export_w;
			house.for_(2 * kMinute);
			CHECK(level(rig, 0) == expected);
			CHECK(rig.number("sp1") == doctest::Approx(percent ? expected / 3680 * 100 : expected));
			CHECK(rig.number("s1") == (expected > 0 ? 1 : 0));
		};
		check(1450, 0);     // 1380 + onMargin 100 not reached
		check(1480, 1380);  // the lowest level
		check(2000, 1840);  // 1380 + 2 x 230 <= 1900
		check(1990, 1840);  // stays: 1840 within 1990 + 100
		check(1739, 1610);  // down a step: 1840 > 1739 + 100
		check(5000, 3680);  // the highest
		check(1200, 0);     // below the lowest level + allowedImport: off
	}
}

TEST_CASE("PvSurplus: a must-run forces the load in time from the grid; past the deadline when it started late") {
	Rig rig = rig_of(R"("surplusFilter": 0, "power2": 600, "minOn2": 0, "minOff2": 0,
		"mustRunBy2": {"hour": 18, "minute": 0, "second": 0}, "mustRunFor2": 10800000)",
		R"("gridPower": "grid", "ready2": "r2")");
	const int64_t day = hc::days_from_civil(2026, 6, 21) * 86400;
	rig.wall().start_utc = day + 14 * 3600; // 14:00 UTC at 0
	rig.wall().synchronized = true;
	rig.report("r2", hc::Value::of_bool(true));
	House house{rig}; // no PV: everything from the grid
	house.until(kHour - 30000);
	CHECK(level(rig, 1) == 0);
	CHECK(state_of(rig, 1) == PvSurplus::waiting);
	house.until(kHour); // 15:00: 3 h before 18:00
	CHECK(level(rig, 1) == 600);
	CHECK(state_of(rig, 1) == PvSurplus::forced);
	house.until(4 * kHour - 30000);
	CHECK(level(rig, 1) == 600);
	house.until(4 * kHour); // 18:00: done
	CHECK(run_of(rig, 1) == 3 * kHour);
	CHECK(level(rig, 1) == 0);
	CHECK(rig.number("block/pv/energy2") == doctest::Approx(1800));

	// The next day the load is not ready until 16:00: forced from then on for 3 h, until 19:00.
	rig.report("r2", hc::Value::of_bool(false));
	house.until(26 * kHour); // 16:00 the next day; the counters started again at midnight
	CHECK(run_of(rig, 1) == 0);
	CHECK(state_of(rig, 1) == PvSurplus::off);
	rig.report("r2", hc::Value::of_bool(true));
	house.until(26 * kHour + 30000);
	CHECK(state_of(rig, 1) == PvSurplus::forced);
	house.until(29 * kHour);
	CHECK(level(rig, 1) == 600);
	house.until(29 * kHour + 30000);
	CHECK(level(rig, 1) == 0);
	CHECK(run_of(rig, 1) == 3 * kHour);
}

TEST_CASE("PvSurplus: surplus runs count for the must-run; forced loads take the surplus first") {
	Rig rig = rig_of(R"("surplusFilter": 0, "power1": 1000, "minOn1": 0, "minOff1": 0, "power2": 1000,
		"minOn2": 0, "minOff2": 0, "mustRunBy2": {"hour": 12, "minute": 0, "second": 0}, "mustRunFor2": 3600000)");
	rig.wall().start_utc = hc::days_from_civil(2026, 6, 21) * 86400 + 10 * 3600 + 30 * 60; // 10:30
	rig.wall().synchronized = true;
	House house{rig};
	house.pv = 300 + 1200; // room for one load: the priority gives it to load 1
	house.until(10 * kMinute);
	CHECK(level(rig, 0) == 1000);
	CHECK(level(rig, 1) == 0);
	house.until(30 * kMinute); // 11:00: load 2 must run now and takes the surplus first
	CHECK(level(rig, 1) == 1000);
	CHECK(state_of(rig, 1) == PvSurplus::forced);
	CHECK(level(rig, 0) == 0);
	CHECK(state_of(rig, 0) == PvSurplus::waiting);
	house.until(90 * kMinute + 30000); // 12:00 and a period: done; load 1 takes its place again
	CHECK(level(rig, 1) == 0);
	CHECK(level(rig, 0) == 1000);
}

TEST_CASE("PvSurplus: maxPerDay blocks a load; the day starts again at midnight, without a clock after 24 h") {
	hc_test::FakeBlobStore blobs;
	Rig rig = rig_of(R"("surplusFilter": 0, "power1": 1000, "minOn1": 0, "minOff1": 0, "maxPerDay1": 3600000)",
		R"("gridPower": "grid")", &blobs);
	rig.wall().start_utc = hc::days_from_civil(2026, 6, 21) * 86400 + 20 * 3600; // 20:00
	rig.wall().synchronized = true;
	House house{rig};
	house.pv = 300 + 2000; // a steady surplus (the simulation needs no sun)
	house.until(kHour + 30000);
	CHECK(level(rig, 0) == 0);
	CHECK(state_of(rig, 0) == PvSurplus::blocked);
	CHECK(run_of(rig, 0) == kHour);
	rig.reboot(); // the counters are persisted: still blocked
	House again{rig, house.pv};
	again.until(rig.now() + kMinute);
	CHECK(state_of(rig, 0) == PvSurplus::blocked);
	again.until(4 * kHour + 30000); // after midnight
	CHECK(level(rig, 0) == 1000);
	CHECK(run_of(rig, 0) == 30000);

	rig.wall().synchronized = false; // without a clock: a day of 24 h from the last start of a day
	again.until(5 * kHour);
	CHECK(state_of(rig, 0) == PvSurplus::blocked);
	CHECK(rig.field(PvSurplus::kDay) >= 0);
	again.until(28 * kHour + 30000);
	CHECK(rig.field(PvSurplus::kDay) == -1);
	CHECK(level(rig, 0) == 1000);
	rig.wall().synchronized = true; // the clock takes the date without a new start
	double run = run_of(rig, 0);
	again.until(rig.now() + 30000);
	CHECK(rig.field(PvSurplus::kDay) >= 0);
	CHECK(run_of(rig, 0) == run + 30000);
}

TEST_CASE("PvSurplus: no forced run without a wall clock") {
	Rig rig = rig_of(R"("surplusFilter": 0, "power1": 600, "mustRunBy1": {"hour": 0, "minute": 30, "second": 0},
		"mustRunFor1": 10800000)");
	House house{rig};
	house.until(2 * kHour);
	CHECK(level(rig, 0) == 0);
	CHECK(state_of(rig, 0) == PvSurplus::waiting);
	rig.wall().start_utc = hc::days_from_civil(2026, 6, 21) * 86400; // 02:00 now: late, forced at once
	rig.wall().synchronized = true;
	house.for_(30000);
	CHECK(state_of(rig, 0) == PvSurplus::forced);
}

TEST_CASE("PvSurplus: the battery takes the surplus first below batteryPriority; discharging is never a source") {
	Rig rig = rig_of(R"("surplusFilter": 0, "batteryPriority": 80, "power1": 1000, "minOn1": 0, "minOff1": 0)",
		R"("gridPower": "grid", "batteryPower": "batt", "batterySoc": "soc")");
	House house{rig};
	house.battery = true;
	house.charge_max = 2000;
	rig.report("soc", 50.0);
	house.pv = 300 + 1500; // all into the battery
	house.for_(2 * kMinute);
	CHECK(level(rig, 0) == 0);
	CHECK(rig.number("block/pv/surplus") == 0);
	rig.report("soc", 80.0); // from 80 % on the loads take the charging power
	house.for_(2 * kMinute);
	CHECK(level(rig, 0) == 1000);
	CHECK(rig.field(PvSurplus::kFiltered) == 1500);
	CHECK(rig.number("block/pv/surplus") == 500);
	house.charge_max = 0; // the battery full: the export pays
	house.for_(2 * kMinute);
	CHECK(level(rig, 0) == 1000);
	rig.stale("soc"); // not good: the battery first again (batteryPriority > 0); a running load keeps
	house.charge_max = 2000; // its power, the battery takes what is left (20.2 D9)
	house.for_(2 * kMinute);
	CHECK(level(rig, 0) == 1000);
	CHECK(rig.number("batt") == 500);
	CHECK(rig.field(PvSurplus::kFiltered) == 1000);
	house.pv = 300 + 850; // less than the load: the battery charges nothing, the load is shed
	house.for_(2 * kMinute);
	CHECK(level(rig, 0) == 0);
	house.pv = 300 + 1500; // all into the battery again: the load does not go on
	house.for_(2 * kMinute);
	CHECK(level(rig, 0) == 0);
	CHECK(rig.number("block/pv/surplus") == 0);

	// A battery that covers the loads (discharging) lowers the surplus, also above batteryPriority.
	Rig cover = rig_of(R"("surplusFilter": 0, "batterySign": "dischargePositive", "batteryPriority": 0,
		"power1": 1000, "minOn1": 0, "minOff1": 0)", R"("gridPower": "grid", "batteryPower": "batt")");
	cover.report("batt", 0.0);
	cover.report("grid", -1200.0);
	cover.run(cover.now() + kMinute);
	CHECK(level(cover, 0) == 1000);
	cover.report("grid", 0.0); // the load runs, the battery discharges 1000 W to cover it: no surplus
	cover.report("batt", 1000.0);
	cover.run(cover.now() + 30000);
	CHECK(cover.number("block/pv/surplus") == 0);
	CHECK(level(cover, 0) == 0);
}

TEST_CASE("PvSurplus: pv and house meters, the grid meter exporting positive") {
	Rig rig = rig_of(R"("surplusFilter": 0, "power1": 1000, "minOn1": 0, "minOff1": 0)",
		R"("pvPower": "pv", "housePower": "house", "batteryPower": "batt")");
	rig.report("pv", 3000.0);
	rig.report("house", 800.0);
	rig.report("batt", 1000.0); // charging, the battery first (no SoC, batteryPriority 100)
	rig.run(30000);
	CHECK(rig.field(PvSurplus::kFiltered) == 1200);
	CHECK(level(rig, 0) == 1000);
	rig.report("house", 1800.0); // the house's consumption with the load
	rig.run(60000);
	CHECK(rig.field(PvSurplus::kFiltered) == 1200);
	CHECK(rig.number("block/pv/surplus") == 200);
	CHECK(level(rig, 0) == 1000);
	Rig other = rig_of(R"("surplusFilter": 0, "gridSign": "exportPositive", "power1": 1000)");
	other.report("grid", 1100.0);
	other.run(30000);
	CHECK(level(other, 0) == 1000);
	CHECK(other.field(PvSurplus::kFiltered) == 1100);
}

TEST_CASE("PvSurplus: a stale meter holds; a lost one releases the loads after meterTimeout, a forced run goes on") {
	Rig rig = rig_of(R"("surplusFilter": 60000, "meterTimeout": 120000, "power1": 1000, "minOn1": 300000,
		"minOff1": 0, "power2": 300, "mustRunBy2": {"hour": 23, "minute": 0, "second": 0}, "mustRunFor2": 10800000)");
	rig.wall().start_utc = hc::days_from_civil(2026, 6, 21) * 86400 + 20 * 3600; // 20:00: load 2 forced
	rig.wall().synchronized = true;
	House house{rig};
	house.pv = 300 + 300 + 700; // load 1 does not fit
	house.until(5 * kMinute);
	CHECK(level(rig, 0) == 0);
	CHECK(level(rig, 1) == 300);
	CHECK(state_of(rig, 1) == PvSurplus::forced);
	rig.stale("grid"); // a tunnel reconnecting: held, nothing goes on
	house.pv = 300 + 300 + 3000;
	rig.run(rig.now() + kMinute);
	CHECK(level(rig, 0) == 0);
	CHECK(state_of(rig, 0) == PvSurplus::waiting);
	house.report(); // good again
	uint64_t last = rig.now();
	rig.run(last + kMinute);
	CHECK(level(rig, 0) == 1000);
	rig.run(last + 120000); // the meter silent for meterTimeout: not yet lost
	CHECK(level(rig, 0) == 1000);
	rig.run(last + 150000); // lost: load 1 held by its minOn, the forced load goes on
	CHECK(rig.field(PvSurplus::kHasFiltered) == 0);
	CHECK(level(rig, 0) == 1000);
	CHECK(level(rig, 1) == 300);
	rig.run(last + 6 * kMinute + 30000); // minOn over: released
	CHECK(level(rig, 0) == 0);
	CHECK(state_of(rig, 0) == PvSurplus::waiting);
	CHECK(state_of(rig, 1) == PvSurplus::forced);
	house.until(rig.now() + kMinute); // the meter back: the filter starts from the first sample
	CHECK(level(rig, 0) == 1000);

	Rig none = rig_of(R"("power1": 1000, "power2": 300, "mustRunBy2": {"hour": 0, "minute": 0, "second": 0},
		"mustRunFor2": 3600000)", R"("pvPower": "pv")"); // without housePower: no meter
	none.wall().start_utc = hc::days_from_civil(2026, 6, 21) * 86400;
	none.wall().synchronized = true;
	none.report("pv", 5000.0);
	none.run(kMinute);
	CHECK(level(none, 0) == 0);
	CHECK(level(none, 1) == 300);
	CHECK(none.model().find("block/pv/surplus")->quality() == hc::Quality::unknown);
}

TEST_CASE("PvSurplus: a chain of two instances keeps the priorities across them; the second never takes what the "
		  "first just allocated") {
	const std::string a = instance("pv", R"("surplusFilter": 0, "power1": 2000, "minOn1": 0, "minOff1": 0)",
		R"("gridPower": "grid", "lowerPower": "block/pv2/loadPower")", R"("switch1": "s1")");
	const std::string b = instance("pv2", R"("surplusFilter": 0, "gridSign": "exportPositive", "power1": 2000,
		"minOn1": 0, "minOff1": 0)", R"("gridPower": "block/pv/surplus")", R"("switch1": "s2")");
	Rig rig("[" + a + ", " + b + "]", nullptr, pv_datapoints());
	auto on = [&](const char* sw) { return rig.number(sw); };
	double pv = 0;
	auto meter = [&](uint64_t until) {
		while (rig.now() + 10000 <= until) {
			rig.run(rig.now() + 10000);
			rig.report("grid", 300 + (on("s1") + on("s2")) * 2000 - pv);
		}
	};
	rig.run(5000);
	pv = 300 + 2200;
	meter(3 * kMinute);
	CHECK(on("s1") == 1); // the first instance's load comes first
	CHECK(on("s2") == 0);
	CHECK(rig.blocks().state(1)[PvSurplus::kSwitchedAt1] == -1); // not even for a period
	CHECK(rig.number("block/pv2/surplus") == 200);
	pv = 300 + 4300;
	meter(6 * kMinute);
	CHECK(on("s2") == 1);
	CHECK(rig.number("block/pv/loadPower") == 4000);
	CHECK(rig.field(PvSurplus::kFiltered) == 4300);
	CHECK(rig.number("block/pv/surplus") == 300);
	pv = 300 + 3000; // 1000 W from the grid: the last instance's load is shed, not the first's
	meter(9 * kMinute);
	CHECK(on("s1") == 1);
	CHECK(on("s2") == 0);
}

TEST_CASE("PvSurplus: the failsafe switches every load off; enabled again the first switching is not held") {
	Rig rig = rig_of(R"("surplusFilter": 0, "power1": 1000, "minOn1": 3600000, "minOff1": 3600000,
		"power2": 400, "minPower2": 100, "step2": 100)");
	House house{rig};
	house.pv = 300 + 2000;
	house.for_(2 * kMinute);
	CHECK(rig.number("s1") == 1);
	CHECK(rig.number("sp2") == 400);
	const char* reason = nullptr;
	REQUIRE(rig.blocks().set_enabled("pv", false, &reason) == hc::blocks::ChangeStatus::ok);
	rig.run(rig.now());
	CHECK(rig.number("s1") == 0);
	CHECK(rig.number("sp1") == 0);
	CHECK(rig.number("s2") == 0);
	CHECK(rig.number("sp2") == 0);
	CHECK(rig.number("block/pv/loadPower") == 0);
	house.for_(kMinute);
	REQUIRE(rig.blocks().set_enabled("pv", true, &reason) == hc::blocks::ChangeStatus::ok);
	rig.run(rig.now());
	CHECK(rig.number("s1") == 1); // the start allocates at once: minOff counts the block's own switchings
	CHECK(rig.number("sp2") == 400);
}

TEST_CASE("PvSurplus: enable and ready release the loads as minOn allows") {
	Rig rig = rig_of(R"("surplusFilter": 0, "power1": 1000, "minOn1": 600000, "minOff1": 0)",
		R"("gridPower": "grid", "enable": "en", "ready1": "r1")");
	House house{rig};
	house.pv = 300 + 2000;
	house.for_(kMinute);
	CHECK(level(rig, 0) == 0); // ready bound and unknown: not ready
	CHECK(state_of(rig, 0) == PvSurplus::off);
	rig.report("r1", hc::Value::of_bool(true));
	house.for_(kMinute);
	CHECK(level(rig, 0) == 1000);
	uint64_t on = rig.now();
	rig.report("en", hc::Value::of_bool(false));
	house.until(on + 9 * kMinute);
	CHECK(level(rig, 0) == 1000); // held by minOn
	house.until(on + 10 * kMinute + 30000);
	CHECK(level(rig, 0) == 0);
	CHECK(state_of(rig, 0) == PvSurplus::off);
	rig.stale("en"); // a stale false counts as unbound: on again
	house.for_(kMinute);
	CHECK(level(rig, 0) == 1000);
}

TEST_CASE("PvSurplus: a load that draws less than its power makes the surplus look larger (20.2 D2)") {
	Rig rig = rig_of(R"("surplusFilter": 0, "power1": 2000, "minOn1": 0, "minOff1": 0, "power2": 1000,
		"minOn2": 0, "minOff2": 0)");
	House house{rig};
	house.draw = 0.5;
	house.pv = 300 + 2200;
	house.for_(3 * kMinute);
	CHECK(level(rig, 0) == 2000);
	CHECK(level(rig, 1) == 1000); // 2200 - 1000 drawn + 2000 allocated = 3200: room for load 2
	CHECK(rig.field(PvSurplus::kFiltered) == 2200 - 1500 + 3000);
	CHECK(rig.number("block/pv/surplus") == 700);
}

TEST_CASE("PvSurplus: no allocation in steady state; the cost of an instance is at most a room controller's") {
	Rig rig = rig_of(R"("surplusFilter": 120000, "power1": 2000, "mustRunBy1": {"hour": 18, "minute": 0, "second": 0},
		"mustRunFor1": 3600000, "maxPerDay1": 7200000, "power2": 3680, "minPower2": 1380, "step2": 230)",
		R"("gridPower": "grid", "batteryPower": "batt", "batterySoc": "soc", "lowerPower": "low",
		"enable": "en", "ready1": "r1", "ready2": "r2")");
	Rig room(R"([{"id": "wz", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "Room",
		"inputs": {"temperature": ["temp", "temp2"], "window": "en"}, "outputs": {"valve": "valve", "switch": "sw"}}])",
		nullptr, pv_datapoints());
	rig.wall().start_utc = hc::days_from_civil(2026, 6, 21) * 86400 + 12 * 3600;
	rig.wall().synchronized = true;
	for (const char* r : {"r1", "r2"}) {
		rig.report(r, hc::Value::of_bool(true));
	}
	rig.report("soc", 90.0);
	rig.report("low", 0.0);
	House house{rig};
	house.battery = true;
	house.charge_max = 500;
	house.pv = 300 + 5000;
	house.for_(10 * kMinute); // the record is stored before the measurement
	{
		hc_test::AllocationScope scope;
		auto writes = rig.blobs().writes;
		for (int i = 0; i < 240; ++i) {
			house.pv = 300 + 2500 + 2500 * ((i / 7) % 3) - 1500 * (i % 2);
			house.for_(30000);
		}
		size_t stored = static_cast<size_t>(rig.blobs().writes - writes);
		CHECK(stored >= 1);
		CHECK(scope.count() - stored == 0); // the fake store allocates for each record it keeps
	}
	auto heap = [](Rig& r) {
		hc::blocks::BlockMemory m = r.blocks().memory();
		size_t bytes = m.runtime + m.set;
		for (size_t i = 0; i < r.model().size(); ++i) {
			const char* name = r.model().get(static_cast<uint16_t>(i))->name();
			if (std::strncmp(name, "block/", 6) == 0) {
				bytes += sizeof(hc::Datapoint) + std::strlen(name) + 1;
			}
		}
		return bytes;
	};
	size_t pv_heap = heap(rig);
	size_t room_heap = heap(room);
	MESSAGE("heap per instance (host, 64-bit): pv surplus " << pv_heap << " B, room controller " << room_heap << " B");
	CHECK(pv_heap <= room_heap);
}
