/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The room controller (spec/blocks.md 14) in closed loop with simulated rooms, and the block
// runtime's costs. The contract cases of the same behaviors are in spec/fixtures/blocks/.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "allocation_counter.h"
#include "conformance_block.h"
#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/blocks/block_runtime.h"
#include "hc/blocks/room_controller.h"
#include "hc/event_loop.h"
#include "hc/json.h"
#include "hc/rules/engine.h"
#include "rules_test_support.h"
#include "test_support.h"

using hc::blocks::BlockRuntime;
using hc::blocks::RoomController;

namespace {

constexpr uint64_t kBase = 1000000;
constexpr uint64_t kMinute = 60000;
constexpr uint64_t kHour = 60 * kMinute;

/// The wall clock of the rules and blocks: settable, UTC.
class Wall : public hc::rules::TimeSource {
public:
	bool local_now(hc::DateTime& out) const override {
		if (!synchronized) {
			return false;
		}
		return utc_to_local(start_utc + static_cast<int64_t>((clock->now_ms - kBase) / 1000), out);
	}
	bool sun_time(hc::rules::SunEvent, hc::TimeOfDay&) const override { return false; }
	const hc_test::FakeClock* clock = nullptr;
	bool synchronized = false;
	int64_t start_utc = 0;
};

/// A controller on the device's runtime: the engine (no rules), the block runtime, a model with the
/// room's datapoints and the block's mirrors.
class Rig {
public:
	explicit Rig(const std::string& blocks, hc_test::FakeBlobStore* shared = nullptr) : blocks_text_(blocks) {
		clock_.now_ms = kBase;
		wall_.clock = &clock_;
		blobs_ = shared != nullptr ? shared : &own_blobs_;
		hc::JsonSpan span;
		REQUIRE(hc::json_parse(blocks_text_.data(), blocks_text_.size(), span, nullptr, 48));
		const char* names[] = {"temp", "temp2", "valve", "sw", "win", "mode", "on"};
		hc::ValueType types[] = {hc::ValueType::real, hc::ValueType::real, hc::ValueType::real, hc::ValueType::boolean,
			hc::ValueType::boolean, hc::ValueType::enumeration, hc::ValueType::boolean};
		for (size_t i = 0; i < 7; ++i) {
			names_.push_back(names[i]);
			specs_types_.push_back(types[i]);
			flags_.push_back(hc::DatapointFlags::kWritable | hc::DatapointFlags::kTransmit);
		}
		std::vector<hc::blocks::ImpliedDatapoint> mirrors(hc::blocks::implied_datapoints(span, hc_test::corpus_registry(), nullptr, 0));
		hc::blocks::implied_datapoints(span, hc_test::corpus_registry(), mirrors.data(), mirrors.size());
		for (const auto& m : mirrors) {
			names_.push_back(m.name);
			specs_types_.push_back(m.type);
			flags_.push_back(m.flags);
		}
		for (size_t i = 0; i < names_.size(); ++i) {
			specs_.push_back(hc::DatapointSpec{names_[i].c_str(), specs_types_[i], flags_[i]});
		}
		REQUIRE(model_.load(specs_.data(), specs_.size()) == hc::DatapointStatus::ok);
		for (uint16_t i = 0; i < 7; ++i) {
			REQUIRE(model_.bind(i, technology_) == hc::DatapointStatus::ok);
		}
		boot(span);
	}

	void boot(const hc::JsonSpan& span) {
		engine_ = std::make_unique<hc::rules::Engine>(loop_, model_, clock_, settings_, wall_);
		hc_test::JsonTree tree;
		hc_test::Json none;
		none.kind = hc_test::Json::Kind::array;
		REQUIRE(engine_->load(tree.value(none), nullptr).ok());
		blocks_ = std::make_unique<BlockRuntime>(loop_, model_, clock_, settings_, *blobs_, *engine_);
		hc::blocks::BlockSet set;
		struct NoOwners : hc::blocks::BlockDirectory {
			const char* owner(const char*) const override { return nullptr; }
		} owners;
		hc::blocks::BlockLoadResult result = set.load(span, model_, owners, hc_test::corpus_registry());
		REQUIRE_MESSAGE(result.ok(), result.path << ": " << result.text);
		REQUIRE(blocks_->adopt(std::move(set)));
		engine_->start(hc::rules::StartReason::boot);
		run(0);
	}

	/// An orderly reboot of the controller (the blobs and settings stay, the values go).
	void reboot() {
		blocks_.reset();
		engine_.reset();
		REQUIRE(model_.load(specs_.data(), specs_.size()) == hc::DatapointStatus::ok);
		for (uint16_t i = 0; i < 7; ++i) {
			REQUIRE(model_.bind(i, technology_) == hc::DatapointStatus::ok);
		}
		hc::JsonSpan span;
		REQUIRE(hc::json_parse(blocks_text_.data(), blocks_text_.size(), span, nullptr, 48));
		boot(span);
	}

	/// Runs the loop up to `ms` after the start, every timer at its time.
	void run(uint64_t ms) {
		uint64_t target = kBase + ms;
		for (;;) {
			uint64_t next = loop_.next_deadline();
			if (next > target) {
				break;
			}
			if (next > clock_.now_ms) {
				clock_.now_ms = next;
			}
			loop_.run_once(clock_.now_ms);
		}
		clock_.now_ms = target;
	}

	uint64_t now() const { return clock_.now_ms - kBase; }

	void report(const char* name, const hc::Value& value) {
		const hc::Datapoint* dp = model_.find(name);
		REQUIRE(dp != nullptr);
		REQUIRE(model_.report(technology_, dp->id(), value) == hc::DatapointStatus::ok);
		run(now());
	}
	void report(const char* name, double value) { report(name, hc::Value::of_real(value)); }
	void stale(const char* name) {
		REQUIRE(model_.report_stale(technology_, model_.find(name)->id()) == hc::DatapointStatus::ok);
		run(now());
	}

	double number(const char* name) const {
		const hc::Datapoint* dp = model_.find(name);
		REQUIRE(dp != nullptr);
		const hc::Value& v = dp->value();
		return v.type() == hc::ValueType::boolean ? (v.as_bool() ? 1 : 0)
			: v.type() == hc::ValueType::enumeration ? v.as_enum() : v.as_real();
	}
	double field(RoomController::Field f) const { return blocks_->state(0)[f]; }
	void set(const char* parameter, double value) {
		const char* reason = nullptr;
		REQUIRE(blocks_->set_parameter("wz", parameter, value, &reason) == hc::blocks::ChangeStatus::ok);
		run(now());
	}

	BlockRuntime& blocks() { return *blocks_; }
	hc::DatapointModel& model() { return model_; }
	hc_test::TestTechnology& technology() { return technology_; }
	Wall& wall() { return wall_; }
	hc_test::FakeBlobStore& blobs() { return *blobs_; }

private:
	std::string blocks_text_;
	hc_test::FakeClock clock_;
	Wall wall_;
	hc_test::FakeSettings settings_;
	hc_test::FakeBlobStore own_blobs_;
	hc_test::FakeBlobStore* blobs_ = nullptr;
	hc::EventLoop loop_;
	hc::DatapointModel model_{clock_};
	hc_test::TestTechnology technology_;
	std::vector<std::string> names_;
	std::vector<hc::ValueType> specs_types_;
	std::vector<uint8_t> flags_;
	std::vector<hc::DatapointSpec> specs_;
	std::unique_ptr<hc::rules::Engine> engine_;
	std::unique_ptr<BlockRuntime> blocks_; ///< Destroyed before the engine.
};

std::string room_block(const std::string& parameters, const std::string& inputs = R"("temperature": "temp")",
	const std::string& outputs = R"("valve": "valve", "switch": "sw")") {
	return R"([{"id": "wz", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "Room",
		"inputs": {)" + inputs + R"(}, "outputs": {)" + outputs + R"(}, "parameters": {)" + parameters + "}}]";
}

/// A room of first order plus dead time (spec/blocks.md 14.2): dθ/dt = (outside + gain·u/100 − θ)/τ,
/// u delayed by the dead time; a window couples the room to the outside with its own time constant.
struct Room {
	Room(double tau, double dead, double g) : tau_s(tau), dead_s(dead), gain(g) {}
	double tau_s, dead_s, gain;
	double outside = 5;
	double theta = 19;
	bool window = false;
	double window_tau_s = 3 * 3600.0; ///< A tilted window: the room cools, but stays above frost.
	std::deque<double> pipe;

	void step(double valve) {
		pipe.push_back(valve);
		double u = pipe.size() > static_cast<size_t>(dead_s) ? pipe.front() : pipe.front();
		if (pipe.size() > static_cast<size_t>(dead_s)) {
			pipe.pop_front();
		}
		theta += (outside + gain * u / 100 - theta) / tau_s;
		if (window) {
			theta += (outside - theta) / window_tau_s;
		}
	}
	double sensor() const { return std::round(theta * 100) / 100; } // 0.01 K resolution
};

Room room_of(RoomController::System system) {
	switch (system) {
		case RoomController::underfloor: return Room(8 * 3600.0, 45 * 60.0, 20);
		case RoomController::convector: return Room(3600.0, 3 * 60.0, 25);
		default: return Room(2 * 3600.0, 10 * 60.0, 30);
	}
}

/// Runs the closed loop for `seconds`, reporting every `report_s` seconds (offset `offset_s` from the
/// grid, `twice`: each report doubled); calls `each` once per second.
template <class Each>
void close_loop(Rig& rig, Room& room, uint64_t seconds, uint64_t report_s, uint64_t offset_s, bool twice, Each each) {
	uint64_t start = rig.now() / 1000;
	for (uint64_t s = start; s < start + seconds; ++s) {
		rig.run(s * 1000);
		if (s % report_s == offset_s) {
			rig.report("temp", room.sensor());
			if (twice) {
				rig.report("temp", room.sensor());
			}
		}
		room.step(rig.number("block/wz/valve"));
		each(s);
	}
}

} // namespace

TEST_CASE("RoomController: the manifests of the built-in and the conformance types are valid") {
	CHECK(hc::blocks::check_manifest(RoomController::instance().manifest()) == nullptr);
	CHECK(hc::blocks::check_manifest(hc_test::ConformanceBlock::v1().manifest()) == nullptr);
	CHECK(hc::blocks::check_manifest(hc_test::ConformanceBlock::v2().manifest()) == nullptr);
	CHECK(hc::blocks::BlockRegistry::device().find("org.homecontrol.room-controller", 1) != nullptr);
	CHECK(hc::blocks::BlockRegistry::device().find("org.homecontrol.conformance", 1) == nullptr); // tests only (B18)
}

TEST_CASE("RoomController: each preset settles a 2 K setpoint step within its band, overshooting at most 0.3 K") {
	hc_test::ScopedCapture capture(hc::LogLevel::error);
	struct Case {
		RoomController::System system;
		const char* name;
		double band_hours;
	} cases[] = {{RoomController::radiator, "radiator", 2}, {RoomController::underfloor, "underfloor", 8},
		{RoomController::convector, "convector", 1}};
	for (const Case& c : cases) {
		CAPTURE(c.name);
		Rig rig(room_block(std::string(R"("system": ")") + c.name + R"(", "comfort": 19)"));
		Room room = room_of(c.system);
		// Settle at 19 °C first (the integral finds the operating point), then step to 21 °C.
		close_loop(rig, room, 36 * 3600, 60, 7, false, [](uint64_t) {});
		CHECK(std::fabs(room.theta - 19) < 0.1);
		rig.set("comfort", 21);
		double peak = 0;
		double last_outside = 0; // the last time outside the band
		uint64_t step_at = rig.now() / 1000;
		close_loop(rig, room, 24 * 3600, 60, 7, false, [&](uint64_t s) {
			peak = std::max(peak, room.theta);
			if (std::fabs(room.theta - 21) > 0.2) {
				last_outside = static_cast<double>(s - step_at);
			}
		});
		MESSAGE(std::string(c.name) << ": overshoot " << peak - 21 << " K, within 0.2 K after " << last_outside / 3600 << " h");
		CHECK(peak - 21 <= 0.3);
		CHECK(last_outside <= c.band_hours * 3600);
	}
}

TEST_CASE("RoomController: a 2 h window opening winds nothing up") {
	// Frost protection at the outside temperature: the room never falls below it.
	Rig rig(room_block(R"("system": "radiator", "windowDelay": 60000, "frost": 5)", R"("temperature": "temp", "window": "win")"));
	Room room = room_of(RoomController::radiator);
	room.theta = 21;
	close_loop(rig, room, 36 * 3600, 60, 7, false, [](uint64_t) {});
	double before = rig.field(RoomController::kIntegral);
	rig.report("win", hc::Value::of_bool(true));
	room.window = true;
	double highest = before;
	close_loop(rig, room, 2 * 3600, 60, 7, false, [&](uint64_t) { highest = std::max(highest, rig.field(RoomController::kIntegral)); });
	CHECK(rig.number("block/wz/state") == RoomController::window);
	CHECK(rig.number("block/wz/valve") == 0);
	CHECK(highest == before); // frozen while saturated at 0 with the room above the frost setpoint
	rig.report("win", hc::Value::of_bool(false));
	room.window = false;
	double peak = 0;
	close_loop(rig, room, 12 * 3600, 60, 7, false, [&](uint64_t) { peak = std::max(peak, room.theta); });
	MESSAGE("after the window: peak " << peak);
	// The room recovers from about 13 K below without the overshoot a wound-up integral would cause
	// (a radiator room with 10 min dead time overshoots a little from so far below anyway).
	CHECK(peak <= 21.5);
	CHECK(std::fabs(room.theta - 21) < 0.2);
}

TEST_CASE("RoomController: a silent or stale sensor puts the valve at the fallback after sensorTimeout") {
	Rig rig(room_block(R"("sensorTimeout": 600000, "fallbackPosition": 35)"));
	rig.report("temp", 20.0);
	rig.run(5 * kMinute);
	CHECK(rig.number("block/wz/state") == RoomController::heating);
	rig.run(10 * kMinute); // exactly at the timeout: still good
	CHECK(rig.number("block/wz/state") == RoomController::heating);
	rig.run(11 * kMinute);
	CHECK(rig.number("block/wz/state") == RoomController::fault);
	CHECK(rig.number("block/wz/valve") == 35);
	rig.report("temp", 20.0);
	rig.run(12 * kMinute);
	CHECK(rig.number("block/wz/state") == RoomController::heating);
	rig.stale("temp");
	rig.run(13 * kMinute);
	CHECK(rig.number("block/wz/state") == RoomController::fault);
	CHECK(rig.number("block/wz/valve") == 35);
}

TEST_CASE("RoomController: the response is time-based, independent of the report rate") {
	auto trajectory = [](uint64_t report_s, bool twice, std::vector<double>& valves, double& integral, double& theta) {
		Rig rig(room_block(R"("system": "radiator", "comfort": 21)"));
		Room room = room_of(RoomController::radiator);
		close_loop(rig, room, 8 * 3600, report_s, 7 % report_s, twice, [&](uint64_t s) {
			if (s % 60 == 30) {
				valves.push_back(rig.number("block/wz/valve"));
			}
		});
		integral = rig.field(RoomController::kIntegral);
		theta = room.theta;
	};
	std::vector<double> once, doubled, often;
	double i1 = 0, i2 = 0, i3 = 0, t1 = 0, t2 = 0, t3 = 0;
	trajectory(60, false, once, i1, t1);
	trajectory(60, true, doubled, i2, t2);
	trajectory(20, false, often, i3, t3);
	CHECK(once == doubled); // a doubled report changes nothing, bit for bit
	CHECK(i1 == i2);
	// Three times the reports: the same response up to the sampling (a per-report integrator would
	// integrate three times as fast).
	CHECK(std::fabs(t1 - t3) < 0.1);
	CHECK(std::fabs(i1 - i3) < 0.05 * std::max(1.0, std::fabs(i1)));
}

TEST_CASE("RoomController: PWM switches on for valve percent of every cycle") {
	Rig rig(room_block(R"("system": "radiator", "controlType": "pwm", "cycle": 600000)"));
	Room room = room_of(RoomController::radiator);
	room.theta = 20.4;
	uint64_t on_s = 0, cycle_s = 0;
	double valve_sum = 0;
	close_loop(rig, room, 6 * 3600, 60, 7, false, [](uint64_t) {});
	close_loop(rig, room, 2 * 3600, 60, 7, false, [&](uint64_t) {
		on_s += rig.number("sw") != 0 ? 1 : 0;
		valve_sum += rig.number("block/wz/valve");
		++cycle_s;
	});
	double duty = 100.0 * static_cast<double>(on_s) / static_cast<double>(cycle_s);
	double mean = valve_sum / static_cast<double>(cycle_s);
	MESSAGE("duty " << duty << " %, mean valve " << mean << " %");
	CHECK(std::fabs(duty - mean) < 2);
	CHECK(mean > 0);
}

TEST_CASE("RoomController: two-point control keeps its hysteresis") {
	Rig rig(room_block(R"("controlType": "twoPoint", "hysteresis": 0.6, "system": "custom", "filter": 0)"));
	struct Step {
		double t;
		bool on;
	} steps[] = {{20.6, true}, {20.9, true}, {21.29, true}, {21.3, false}, {21.0, false}, {20.71, false}, {20.7, true}};
	uint64_t at = 0;
	for (const Step& s : steps) {
		at += 3 * kMinute;
		rig.report("temp", s.t);
		rig.run(at);
		CAPTURE(s.t);
		CHECK((rig.number("sw") != 0) == s.on);
	}
}

TEST_CASE("RoomController: boost opens fully after a setpoint step and ends near the setpoint") {
	Rig rig(room_block(R"("system": "custom", "xp": 2, "tn": 60, "td": 0, "filter": 0, "boostThreshold": 2)",
		R"("temperature": "temp", "mode": "mode")"));
	rig.report("mode", hc::Value::of_enum(3));
	rig.report("temp", 17.0);
	rig.run(5 * kMinute);
	double frozen = rig.field(RoomController::kIntegral);
	rig.report("mode", hc::Value::of_enum(1)); // night 17 -> comfort 21
	CHECK(rig.number("block/wz/state") == RoomController::boost);
	CHECK(rig.number("block/wz/valve") == 100);
	rig.report("temp", 19.5);
	rig.run(20 * kMinute);
	CHECK(rig.number("block/wz/state") == RoomController::boost); // the error 1.5 K is still >= 1 K
	CHECK(rig.field(RoomController::kIntegral) == frozen);
	rig.report("temp", 20.2);
	rig.run(22 * kMinute);
	CHECK(rig.number("block/wz/state") == RoomController::heating);
	CHECK(rig.number("block/wz/valve") < 100);
}

TEST_CASE("RoomController: valve protection opens fully once on its weekday") {
	Rig rig(room_block(R"("protectionDay": 3, "protectionTime": {"hour": 10, "minute": 0, "second": 0}, "protectionDuration": 300000, "summerPosition": 0)",
		R"("temperature": "temp", "summer": "on")"));
	rig.wall().synchronized = true;
	rig.wall().start_utc = (hc::rules::days_of(2026, 11, 4) * 86400) + 9 * 3600 + 58 * 60; // Wednesday 09:58
	rig.report("on", hc::Value::of_bool(true));
	rig.report("temp", 22.0);
	rig.run(kMinute + 30000);
	CHECK(rig.number("block/wz/state") == RoomController::summer);
	rig.run(3 * kMinute);
	CHECK(rig.number("block/wz/state") == RoomController::protection);
	CHECK(rig.number("block/wz/valve") == 100);
	rig.run(8 * kMinute);
	CHECK(rig.number("block/wz/state") == RoomController::summer);
	CHECK(rig.number("block/wz/valve") == 0);
	rig.run(3 * kHour);
	CHECK(rig.number("block/wz/state") == RoomController::summer); // once per day
}

TEST_CASE("RoomController: the integral survives a restart") {
	hc_test::FakeBlobStore blobs;
	Rig rig(room_block(R"("system": "radiator")"), &blobs);
	Room room = room_of(RoomController::radiator);
	close_loop(rig, room, 4 * 3600, 60, 7, false, [](uint64_t) {});
	double integral = rig.field(RoomController::kIntegral);
	CHECK(integral > 0);
	rig.reboot();
	CHECK(rig.field(RoomController::kIntegral) == integral);
	hc::blocks::BlockDiagnostics d;
	REQUIRE(rig.blocks().diagnostics(0, d));
	CHECK(d.restored);
}

TEST_CASE("BlockRuntime: no allocation in steady state; the cost of a room controller") {
	Rig rig(room_block(R"("system": "radiator")", R"("temperature": ["temp", "temp2"], "window": "win")"));
	rig.report("temp", 20.0);
	rig.report("temp2", 20.4);
	rig.run(10 * kMinute);
	{
		hc_test::AllocationScope scope;
		uint16_t temp = rig.model().find("temp")->id();
		uint16_t win = rig.model().find("win")->id();
		size_t refused = 0;
		auto writes = rig.blobs().writes;
		for (int i = 0; i < 30; ++i) {
			refused += rig.model().report(rig.technology(), temp, hc::Value::of_real(20.0 + i * 0.01)) != hc::DatapointStatus::ok;
			refused += rig.model().report(rig.technology(), win, hc::Value::of_bool(i % 2 == 0)) != hc::DatapointStatus::ok;
			rig.run(rig.now() + kMinute);
		}
		// The fake store allocates for each record it keeps (the platform's writes to the flash do not).
		size_t allocations = scope.count() - static_cast<size_t>(rig.blobs().writes - writes);
		CHECK(rig.blobs().writes - writes == 3); // the record every 10 minutes (persistEvery)
		CHECK(refused == 0);
		CHECK(allocations == 0);
	}
	hc::blocks::BlockMemory m = rig.blocks().memory();
	size_t mirrors = 0;
	for (size_t i = 0; i < rig.model().size(); ++i) {
		mirrors += std::string(rig.model().get(static_cast<uint16_t>(i))->name()).compare(0, 6, "block/") == 0 ? 1 : 0;
	}
	MESSAGE("block runtime object " << m.object << " B, runtime " << m.runtime << " B, set " << m.set << " B, "
		<< mirrors << " mirror datapoints (host, 64-bit)");
	CHECK(mirrors == 1 + 4 + 10 + 1); // enabled, 4 setpoints, 10 outputs, the aggregated temperature
}
