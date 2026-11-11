/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the function roles the image loader knows (spec/runtime-image.md 3.5): the dimmer's
// relative-dimming role `step` (#30) accepts only a writable controlled datapoint; the roles of
// schema 1.6 (haumacher/ehome #134, #136, #138): a dimmer with its switching command alone, the end
// positions of a blind, a climate's heating of a number and its heating level.

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
		" {\"id\": \"dim.ro\", \"label\": \"Read only\", \"type\": \"controlled\"},\n"
		" {\"id\": \"dim.switch\", \"type\": \"boolean\", \"writable\": true},\n"
		" {\"id\": \"dim.state\", \"type\": \"boolean\"},\n"
		" {\"id\": \"blind.move\", \"type\": \"boolean\", \"writable\": true},\n"
		" {\"id\": \"blind.top\", \"type\": \"boolean\"},\n"
		" {\"id\": \"blind.bottom\", \"type\": \"boolean\"},\n"
		" {\"id\": \"room.t\", \"type\": \"real\"},\n"
		" {\"id\": \"room.valve\", \"type\": \"unsigned\"}\n"
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

TEST_CASE("FunctionRoles.DimmerWithItsCommandAlone") {
	Plan plan;
	Report report;
	REQUIRE_MESSAGE(parse(image_with_function(
		"{\"id\": \"lamp\", \"label\": \"Lamp\", \"kind\": \"dimmer\", "
		"\"datapoints\": {\"command\": \"dim.switch\", \"state\": \"dim.state\"}}"), plan, report), refusals(report));
	CHECK(plan.info.functions == 1);
}

TEST_CASE("FunctionRoles.DimmerNeedsCommandOrLevelCommand") {
	Plan plan;
	Report report;
	CHECK_FALSE(parse(image_with_function(
		"{\"id\": \"lamp\", \"label\": \"Lamp\", \"kind\": \"dimmer\", "
		"\"datapoints\": {\"state\": \"dim.state\", \"step\": \"dim.step\"}}"), plan, report));
	CHECK(refusals(report) == "functions[0].datapoints: a dimmer needs the role command or levelCommand\n");
}

TEST_CASE("FunctionRoles.BlindEndPositions") {
	Plan plan;
	Report report;
	REQUIRE_MESSAGE(parse(image_with_function(
		"{\"id\": \"awning\", \"label\": \"Awning\", \"kind\": \"blind\", "
		"\"datapoints\": {\"move\": \"blind.move\", \"atTop\": \"blind.top\", \"atBottom\": \"blind.bottom\"}}"),
		plan, report), refusals(report));
	Plan refused;
	CHECK_FALSE(parse(image_with_function(
		"{\"id\": \"awning\", \"label\": \"Awning\", \"kind\": \"blind\", "
		"\"datapoints\": {\"move\": \"blind.move\", \"atTop\": \"dim.level\"}}"), refused, report));
	CHECK(refusals(report) ==
		"functions[0].datapoints.atTop: datapoint 'dim.level' is unsigned, but the role atTop of a blind takes boolean\n");
}

TEST_CASE("FunctionRoles.ClimateHeatingOfANumberAndItsLevel") {
	Plan plan;
	Report report;
	REQUIRE_MESSAGE(parse(image_with_function(
		"{\"id\": \"room\", \"label\": \"Room\", \"kind\": \"climate\", "
		"\"datapoints\": {\"temperature\": \"room.t\", \"heating\": \"room.valve\", \"heatingLevel\": \"room.valve\"}}"),
		plan, report), refusals(report));
	Plan refused;
	CHECK_FALSE(parse(image_with_function(
		"{\"id\": \"room\", \"label\": \"Room\", \"kind\": \"climate\", "
		"\"datapoints\": {\"temperature\": \"room.t\", \"heatingLevel\": \"dim.state\"}}"), refused, report));
	CHECK(refusals(report) == "functions[0].datapoints.heatingLevel: datapoint 'dim.state' is boolean, but the role "
		"heatingLevel of a climate takes a number (unsigned, signed or real)\n");
}
