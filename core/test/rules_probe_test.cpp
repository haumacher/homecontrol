/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the rule interpreter (issue #12): a rule from JSON driving
// a technology through the model, a parameter override that survives a new engine over the same
// settings, a refused load naming the rule, and the engine ignoring what it did not load.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/rules/engine.h"
#include "rules_test_support.h"
#include "test_support.h"

using hc::DatapointStatus;
using hc::Value;
using hc::ValueType;
using hc::rules::Engine;
using hc::rules::LoadResult;
using hc::rules::StartReason;
using hc_test::Json;
using hc_test::JsonTree;

namespace {

/// Records every command the rules send and can refuse them.
struct Recording : hc::Technology {
	const char* name() const override { return "recording"; }
	bool command(const hc::Datapoint& datapoint, const Value& value) override {
		commands.push_back({datapoint.id(), value});
		return !refuse;
	}
	struct Command {
		uint16_t id;
		Value value;
	};
	std::vector<Command> commands;
	bool refuse = false;
};

struct NoTime : hc::rules::TimeSource {
	bool local_now(hc::DateTime&) const override { return false; }
	bool sun_time(hc::rules::SunEvent, hc::TimeOfDay&) const override { return false; }
};

struct NoSchedules : hc::rules::ScheduleDirectory {
	bool has_schedule(const char*) const override { return false; }
};

const char* kRules = R"([
 {"id": "offset", "name": "b follows a plus p",
  "parameters": [{"id": "p", "label": "Offset", "type": "integer", "default": 1, "min": 0, "max": 10}],
  "reactions": [{
   "triggers": [{"kind": "datapoint", "datapoint": "a"}],
   "actions": [{"kind": "write", "datapoint": "b",
     "value": {"kind": "binary", "op": "+", "left": {"kind": "datapoint", "name": "a"}, "right": {"kind": "parameter", "id": "p"}}}]
  }]}
])";

struct Rig {
	hc_test::ScopedCapture capture;
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc::DatapointModel model{clock};
	hc_test::FakeSettings settings;
	Recording tech;
	NoTime time;
	NoSchedules schedules;
	JsonTree tree;
	Json json;
	std::vector<std::string> names{"a", "b"};
	hc::DatapointSpec specs[2] = {
		{"a", ValueType::signed_int, hc::DatapointFlags::kReadable},
		{"b", ValueType::signed_int, hc::DatapointFlags::kReadable | hc::DatapointFlags::kWritable | hc::DatapointFlags::kTransmit}};

	Rig() {
		REQUIRE(model.load(specs, 2) == DatapointStatus::ok);
		REQUIRE(model.bind(0, tech) == DatapointStatus::ok);
		REQUIRE(model.bind(1, tech) == DatapointStatus::ok);
	}
	void run() {
		for (int i = 0; i < 10; ++i) {
			loop.run_once(clock.now_ms);
		}
	}
};

} // namespace

TEST_CASE("RulesProbe.ARuleFromJsonDrivesTheTechnologyAndItsParameterOverrideSurvivesANewEngine") {
	Rig r;
	std::string error;
	REQUIRE(hc_test::parse_json(kRules, r.json, error));
	{
		Engine engine(r.loop, r.model, r.clock, r.settings, r.time);
		LoadResult result = engine.load(r.tree.value(r.json), &r.schedules);
		REQUIRE(result.ok());
		engine.start(StartReason::boot);
		r.run();
		// a = 5 -> b = 6 through the technology.
		REQUIRE(r.model.report(r.tech, 0, Value::of_signed(5)) == DatapointStatus::ok);
		r.run();
		REQUIRE(r.tech.commands.size() == 1);
		CHECK(r.tech.commands.back().value == Value::of_signed(6));
		CHECK(r.model.get(1)->value() == Value::of_signed(6));
		// The inhabitant raises the offset to 3; out of range is refused with a reason.
		const char* reason = nullptr;
		CHECK(engine.set_parameter("offset", "p", hc::rules::Val::of_int(3), &reason) == hc::rules::ParameterStatus::ok);
		CHECK(engine.set_parameter("offset", "p", hc::rules::Val::of_int(11), &reason) != hc::rules::ParameterStatus::ok);
		CHECK(reason != nullptr);
		CHECK(engine.set_parameter("offset", "nope", hc::rules::Val::of_int(1), &reason) != hc::rules::ParameterStatus::ok);
		r.run(); // a parameter change is an event of its own; index 0 is the enable switch, 1 is p
		CHECK(engine.parameter_overridden(0, 1));
		// The datapoint trigger fires on change: a new value is needed.
		REQUIRE(r.model.report(r.tech, 0, Value::of_signed(6)) == DatapointStatus::ok);
		r.run();
		CHECK(r.tech.commands.back().value == Value::of_signed(9));
		hc::rules::RuleDiagnostics d;
		REQUIRE(engine.diagnostics(0, d));
		CHECK(d.firings == 2);
		CHECK(std::string(d.id) == "offset");
	}
	// A reboot: a new engine over the same settings restores the override.
	{
		Engine engine(r.loop, r.model, r.clock, r.settings, r.time);
		REQUIRE(engine.load(r.tree.value(r.json), &r.schedules).ok());
		engine.start(StartReason::boot);
		r.run();
		CHECK(engine.parameter_overridden(0, 1));
		REQUIRE(r.model.report(r.tech, 0, Value::of_signed(10)) == DatapointStatus::ok);
		r.run();
		CHECK(r.tech.commands.back().value == Value::of_signed(13));
		// Reset to default: the stored override goes away.
		CHECK(engine.reset_parameter("offset", "p") == hc::rules::ParameterStatus::ok);
		r.run();
		CHECK_FALSE(engine.parameter_overridden(0, 1));
		REQUIRE(r.model.report(r.tech, 0, Value::of_signed(11)) == DatapointStatus::ok);
		r.run();
		CHECK(r.tech.commands.back().value == Value::of_signed(12));
	}
	// A refused command by the technology does not change the datapoint and is counted as an error.
	{
		Engine engine(r.loop, r.model, r.clock, r.settings, r.time);
		REQUIRE(engine.load(r.tree.value(r.json), &r.schedules).ok());
		engine.start(StartReason::boot);
		r.run();
		r.tech.refuse = true;
		REQUIRE(r.model.report(r.tech, 0, Value::of_signed(20)) == DatapointStatus::ok);
		r.run();
		CHECK(r.model.get(1)->value() == Value::of_signed(12)); // unchanged
		hc::rules::RuleDiagnostics d;
		REQUIRE(engine.diagnostics(0, d));
		CHECK(d.errors >= 1);
		CHECK(d.has_error);
	}
}

TEST_CASE("RulesProbe.ALoadRefusalNamesTheRuleAndLeavesTheEngineEmpty") {
	Rig r;
	std::string error;
	const char* bad = R"([
	 {"id": "broken", "name": "writes nowhere",
	  "reactions": [{"triggers": [{"kind": "datapoint", "datapoint": "a"}],
	   "actions": [{"kind": "write", "datapoint": "does.not.exist", "value": {"kind": "datapoint", "name": "a"}}]}]}
	])";
	REQUIRE(hc_test::parse_json(bad, r.json, error));
	Engine engine(r.loop, r.model, r.clock, r.settings, r.time);
	LoadResult result = engine.load(r.tree.value(r.json), &r.schedules);
	CHECK_FALSE(result.ok());
	CHECK(std::string(result.rule) == "broken");
	CHECK(std::string(result.text).size() > 0);
	CHECK_FALSE(engine.loaded());
	engine.start(StartReason::boot);
	r.run();
	REQUIRE(r.model.report(r.tech, 0, Value::of_signed(1)) == DatapointStatus::ok);
	r.run();
	CHECK(r.tech.commands.empty()); // nothing loaded, nothing fires
}
