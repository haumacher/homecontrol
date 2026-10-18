/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the image loader against spec/rules.md 10.1 (#33, decided 2026-10-18): a rule that writes
// the output datapoint of a schedule entry is refused when the image is loaded, not when the write
// happens, and the refusal names the rule's write and the reason.

#include <string>

#include "doctest.h"
#include "hc/image/loader.h"
#include "test_support.h"

using hc::image::Loader;
using hc::image::Plan;
using hc::image::Report;

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

bool parse(const std::string& text, Plan& plan, Report& report) {
	hc_test::FakeClock clock;
	Loader loader(clock, hc::HistoryLimits().cap_bytes);
	return loader.parse(text.data(), text.size(), plan, report);
}

std::string refusals(const Report& report) {
	std::string out;
	for (size_t i = 0; i < report.count; ++i) {
		out += std::string(report.refusals[i].path) + ": " + report.refusals[i].reason + "\n";
	}
	return out;
}

} // namespace

TEST_CASE("ScheduleOutput.RuleWriteRefusedAtLoad") {
	Plan plan;
	Report report;
	CHECK_FALSE(parse(image_with_rule_writing("schedule.morning"), plan, report));
	std::string text = refusals(report);
	CHECK_MESSAGE(text.find("notWritable") != std::string::npos, text);
	CHECK_MESSAGE(text.find("schedule.morning") != std::string::npos, text);
}

TEST_CASE("ScheduleOutput.OtherWritesLoad") {
	Plan plan;
	Report report;
	REQUIRE_MESSAGE(parse(image_with_rule_writing("hall.light"), plan, report), refusals(report));
	CHECK(plan.info.rules == 1);
	CHECK(plan.info.schedules == 1);
}
