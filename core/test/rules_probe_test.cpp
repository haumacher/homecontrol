/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the rule interpreter (issue #12): a rule of a compiled image
// (issue #87) driving a technology through the model, and a parameter override that survives a new
// engine over the same settings.

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

const char* kRules = R"(
 {"id": "offset", "name": "b follows a plus p",
  "parameters": [{"id": "p", "label": "Offset", "type": "integer", "default": 1, "min": 0, "max": 10}],
  "reactions": [{
   "triggers": [{"kind": "datapoint", "datapoint": "a"}],
   "actions": [{"kind": "write", "datapoint": "b",
     "value": {"kind": "binary", "op": "+", "left": {"kind": "datapoint", "name": "a"}, "right": {"kind": "parameter", "id": "p"}}}]
  }]})";

struct Rig {
	hc_test::ScopedCapture capture;
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc::DatapointModel model{clock};
	hc_test::FakeSettings settings;
	Recording tech;
	NoTime time;
	/// a (0) readable, b (1) writable and transmitting; behind them the rule's parameter datapoints.
	hc_test::RuleImage image;

	Rig() {
		REQUIRE(image.compile(hc_test::rules_source(
			hc_test::datapoint_source("a", "signed", false) + "," + hc_test::datapoint_source("b", "signed"), kRules)));
		image.load_model(model);
		REQUIRE(model.get(0)->type() == ValueType::signed_int);
		REQUIRE(model.bind(0, tech) == DatapointStatus::ok);
		REQUIRE(model.bind(1, tech) == DatapointStatus::ok);
	}
	/// The image's rules into `engine`.
	void adopt(Engine& engine) {
		LoadResult result = engine.adopt(image.bind());
		REQUIRE_MESSAGE(result.ok(), result.path << ": " << result.text);
	}
	void run() {
		for (int i = 0; i < 10; ++i) {
			loop.run_once(clock.now_ms);
		}
	}
};

} // namespace

TEST_CASE("RulesProbe.ARuleOfTheImageDrivesTheTechnologyAndItsParameterOverrideSurvivesANewEngine") {
	Rig r;
	{
		Engine engine(r.loop, r.model, r.clock, r.settings, r.time);
		r.adopt(engine);
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
	// A reboot: a new model and engine over the same settings restore the override (the old
	// engine's parameter technology went with it: the model is loaded anew, as at a boot).
	r.image.load_model(r.model);
	REQUIRE(r.model.bind(0, r.tech) == DatapointStatus::ok);
	REQUIRE(r.model.bind(1, r.tech) == DatapointStatus::ok);
	{
		Engine engine(r.loop, r.model, r.clock, r.settings, r.time);
		r.adopt(engine);
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
	r.image.load_model(r.model);
	REQUIRE(r.model.bind(0, r.tech) == DatapointStatus::ok);
	REQUIRE(r.model.bind(1, r.tech) == DatapointStatus::ok);
	{
		Engine engine(r.loop, r.model, r.clock, r.settings, r.time);
		r.adopt(engine);
		engine.start(StartReason::boot);
		r.run();
		r.tech.refuse = true;
		REQUIRE(r.model.report(r.tech, 0, Value::of_signed(20)) == DatapointStatus::ok);
		r.run();
		CHECK(r.model.get(1)->value().empty()); // unchanged (the new model has no value yet)
		hc::rules::RuleDiagnostics d;
		REQUIRE(engine.diagnostics(0, d));
		CHECK(d.errors >= 1);
		CHECK(d.has_error);
	}
}

TEST_CASE("RulesProbe.AResumedDatapointTriggersNoRule") {
	// homecontrol #103: resume() makes a stale value good again without an update; the rule
	// triggered by a change of `a` must not run (it would write b = a + 1 again).
	Rig r;
	Engine engine(r.loop, r.model, r.clock, r.settings, r.time);
	r.adopt(engine);
	engine.start(StartReason::boot);
	r.run();
	REQUIRE(r.model.report(r.tech, 0, Value::of_signed(5)) == DatapointStatus::ok);
	r.run();
	REQUIRE(r.tech.commands.size() == 1);
	// Contact lost: the trigger fires, `a` has no value, nothing is written.
	REQUIRE(r.model.report_stale(r.tech, 0) == DatapointStatus::ok);
	r.run();
	CHECK(r.tech.commands.size() == 1);
	size_t no_value = r.capture.sink.count("valueNoValue");
	// Contact back: good again with 5, silently for the rules.
	REQUIRE(r.model.resume(r.tech, 0) == DatapointStatus::ok);
	r.run();
	CHECK(r.model.get(0)->quality() == hc::Quality::good);
	CHECK(r.tech.commands.size() == 1);
	CHECK(r.capture.sink.count("valueNoValue") == no_value);
	// The next change of `a` evaluates with the good value as usual.
	REQUIRE(r.model.report(r.tech, 0, Value::of_signed(7)) == DatapointStatus::ok);
	r.run();
	REQUIRE(r.tech.commands.size() == 2);
	CHECK(r.tech.commands.back().value == Value::of_signed(8));
}
