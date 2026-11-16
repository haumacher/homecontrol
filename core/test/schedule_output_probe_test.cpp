/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of a schedule entry's output beside a rule (#33, decided 2026-10-18; spec/rules.md 10.1): a
// rule that writes the output datapoint of a schedule entry is refused when the image is validated,
// not when the write happens. Since the binary image (issue #87) that refusal is E-Home's
// (notWritable); the device loads the image whose rule writes another datapoint, with the output
// datapoint owned by the scheduler.

#include <string>

#include "doctest.h"
#include "hc/history.h"
#include "hc/image/loader.h"
#include "hc/rules/rule_set.h"
#include "image_support.h"
#include "test_support.h"

using hc::image::Loader;
using hc::image::Plan;
using hc::image::Report;
namespace bin = hc::image::bin;

namespace {

std::string image_with_rule_writing(const char* datapoint) {
	return std::string(
		"{\"image\": {\"schemaVersion\": \"1.0\", \"projectId\": \"sched\", \"revision\": 1},\n"
		"\"settings\": {\"tz\": \"CET-1CEST,M3.5.0,M10.5.0/3\"},\n"
		"\"datapoints\": [\n"
		" {\"id\": \"schedule.morning\", \"label\": \"Morning fired\", \"type\": \"boolean\", \"writable\": true},\n"
		" {\"id\": \"hall.light\", \"label\": \"Hall light\", \"type\": \"boolean\", \"writable\": true}\n"
		"],\n"
		"\"schedules\": [{\"id\": \"morning\", \"label\": \"Morning\", \"kind\": \"weekly\", \"days\": [1, 2, 3, 4, 5],\n"
		"  \"time\": {\"hour\": 6, \"minute\": 30, \"second\": 0}, \"output\": \"schedule.morning\"}],\n"
		"\"rules\": [{\"id\": \"r\", \"name\": \"Rule\", \"reactions\": [{\"triggers\": [{\"kind\": \"schedule\", \"schedule\": \"morning\"}],\n"
		"  \"actions\": [{\"kind\": \"write\", \"datapoint\": \"") + datapoint +
		"\", \"value\": {\"kind\": \"literal\", \"type\": \"boolean\", \"value\": true}}]}]}]}";
}

std::string refusals(const Report& report) {
	std::string out;
	for (size_t i = 0; i < report.count; ++i) {
		out += std::string(report.refusals[i].path) + ": " + report.refusals[i].reason + "\n";
	}
	return out;
}

} // namespace

TEST_CASE("ScheduleOutput.OtherWritesLoad") {
	hc_test::CompiledImage image;
	REQUIRE(image.compile(image_with_rule_writing("hall.light")));
	hc_test::FakeClock clock;
	Loader loader(clock, hc::HistoryLimits().cap_bytes);
	Plan plan;
	Report report;
	REQUIRE_MESSAGE(loader.build(image.view, plan, report), refusals(report));
	CHECK(plan.info.rules == 1);
	CHECK(plan.info.schedules == 1);
	// The output datapoint is the scheduler's (owner 2, member 0: spec/runtime-image.md 10.7); the rule
	// triggers on the schedule by its index and writes hall.light.
	const bin::DatapointRec& output = image.view.at<bin::DatapointRec>(bin::kDatapoints, 0);
	CHECK(std::string(image.view.str(output.name)) == "schedule.morning");
	CHECK(output.owner == 2);
	CHECK(output.owner_index == 0);
	CHECK(output.owner_member == 0);
	const hc::rules::RuleSet& rules = plan.rule_set;
	REQUIRE(rules.rule_count() == 1);
	const hc::rules::Trigger& trigger = rules.trigger(rules.reaction(rules.rule(0).first_reaction).first_trigger);
	CHECK(trigger.kind == hc::rules::TriggerKind::schedule);
	CHECK(std::string(rules.schedule(trigger.ref)) == "morning");
	const hc::rules::Action& write = rules.action(rules.reaction(rules.rule(0).first_reaction).actions.first);
	CHECK(std::string(image.view.str(image.view.at<bin::DatapointRec>(bin::kDatapoints, write.target).name)) ==
		"hall.light");
}
