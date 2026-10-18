/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the function roles the image loader knows (spec/runtime-image.md 3.5): the dimmer's
// relative-dimming role `step` (#30) accepts only a writable controlled datapoint.

#include <string>

#include "doctest.h"
#include "hc/image/loader.h"
#include "test_support.h"

using hc::image::Loader;
using hc::image::Plan;
using hc::image::Report;

namespace {

std::string image_with_function(const char* function) {
	return std::string(
		"{\"image\": {\"schemaVersion\": \"1.0\", \"projectId\": \"roles\", \"revision\": 1},\n"
		"\"datapoints\": [\n"
		" {\"id\": \"dim.level\", \"label\": \"Level\", \"type\": \"unsigned\", \"writable\": true},\n"
		" {\"id\": \"dim.step\", \"label\": \"Step\", \"type\": \"controlled\", \"writable\": true},\n"
		" {\"id\": \"dim.bad\", \"label\": \"Bad\", \"type\": \"boolean\", \"writable\": true},\n"
		" {\"id\": \"dim.ro\", \"label\": \"Read only\", \"type\": \"controlled\"}\n"
		"],\n\"functions\": [") + function + "]}";
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

TEST_CASE("FunctionRoles.DimmerStepAcceptsControlled") {
	Plan plan;
	Report report;
	REQUIRE_MESSAGE(parse(image_with_function(
		"{\"id\": \"lamp\", \"label\": \"Lamp\", \"kind\": \"dimmer\", "
		"\"datapoints\": {\"levelCommand\": \"dim.level\", \"step\": \"dim.step\"}}"), plan, report), refusals(report));
	CHECK(plan.info.functions == 1);
}

TEST_CASE("FunctionRoles.DimmerStepRefusesOtherTypes") {
	Plan plan;
	Report report;
	CHECK_FALSE(parse(image_with_function(
		"{\"id\": \"lamp\", \"label\": \"Lamp\", \"kind\": \"dimmer\", "
		"\"datapoints\": {\"levelCommand\": \"dim.level\", \"step\": \"dim.bad\"}}"), plan, report));
	std::string text = refusals(report);
	CHECK_MESSAGE(text.find("step") != std::string::npos, text);
	CHECK_MESSAGE(text.find("controlled") != std::string::npos, text);
}

TEST_CASE("FunctionRoles.DimmerStepNeedsWritable") {
	Plan plan;
	Report report;
	CHECK_FALSE(parse(image_with_function(
		"{\"id\": \"lamp\", \"label\": \"Lamp\", \"kind\": \"dimmer\", "
		"\"datapoints\": {\"levelCommand\": \"dim.level\", \"step\": \"dim.ro\"}}"), plan, report));
	std::string text = refusals(report);
	CHECK_MESSAGE(text.find("step") != std::string::npos, text);
	CHECK_MESSAGE(text.find("writable") != std::string::npos, text);
}
