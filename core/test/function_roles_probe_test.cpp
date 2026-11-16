/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the function roles (spec/runtime-image.md 3.5, 9.8): the dimmer's relative-dimming role
// `step` (#30) of a controlled datapoint; the roles of schema 1.6 (haumacher/ehome #134, #136, #138):
// a dimmer with its switching command alone, the end positions of a blind, a climate's heating of a
// number and its heating level. Compiled into the binary image (issue #87), they reach the device as
// `functions` and `roles` and load; which roles a kind takes and of which types is E-Home's
// validation (I7), so the refusals of the roles are no longer the device's.

#include <string>

#include "doctest.h"
#include "hc/history.h"
#include "hc/image/loader.h"
#include "image_support.h"
#include "test_support.h"

using hc::image::Loader;
using hc::image::Plan;
using hc::image::Report;
namespace bin = hc::image::bin;

namespace {

std::string image_with_function(const char* function) {
	return std::string(
		"{\"image\": {\"schemaVersion\": \"1.6\", \"projectId\": \"roles\", \"revision\": 1},\n"
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

std::string refusals(const Report& report) {
	std::string out;
	for (size_t i = 0; i < report.count; ++i) {
		out += std::string(report.refusals[i].path) + ": " + report.refusals[i].reason + "\n";
	}
	return out;
}

/// The function compiled, checked and loaded as the device does: "<role>=<datapoint>" per role of
/// the image's function 0, in the image's order; "refused: ..." when the device refuses it.
std::string roles_of(const char* function) {
	hc_test::CompiledImage image;
	if (!image.compile(image_with_function(function))) {
		return "refused by the checks";
	}
	hc_test::FakeClock clock;
	Loader loader(clock, hc::HistoryLimits().cap_bytes);
	Plan plan;
	Report report;
	if (!loader.build(image.view, plan, report)) {
		return "refused: " + refusals(report);
	}
	REQUIRE(plan.info.functions == 1);
	const bin::FunctionRec& f = image.view.at<bin::FunctionRec>(bin::kFunctions, 0);
	std::string out;
	for (size_t r = 0; r < f.role_count; ++r) {
		const bin::RoleRec& role = image.view.at<bin::RoleRec>(bin::kRoles, f.first_role + r);
		const bin::DatapointRec& dp = image.view.at<bin::DatapointRec>(bin::kDatapoints, role.datapoint);
		out += std::string(out.empty() ? "" : " ") + image.view.str(role.name) + "=" + image.view.str(dp.name);
	}
	return out;
}

} // namespace

TEST_CASE("FunctionRoles.DimmerStepAcceptsControlled") {
	CHECK(roles_of("{\"id\": \"lamp\", \"label\": \"Lamp\", \"kind\": \"dimmer\", "
		"\"datapoints\": {\"levelCommand\": \"dim.level\", \"step\": \"dim.step\"}}") ==
		"levelCommand=dim.level step=dim.step");
}

TEST_CASE("FunctionRoles.DimmerWithItsCommandAlone") {
	CHECK(roles_of("{\"id\": \"lamp\", \"label\": \"Lamp\", \"kind\": \"dimmer\", "
		"\"datapoints\": {\"command\": \"dim.switch\", \"state\": \"dim.state\"}}") ==
		"command=dim.switch state=dim.state");
}

TEST_CASE("FunctionRoles.BlindEndPositions") {
	CHECK(roles_of("{\"id\": \"awning\", \"label\": \"Awning\", \"kind\": \"blind\", "
		"\"datapoints\": {\"move\": \"blind.move\", \"atTop\": \"blind.top\", \"atBottom\": \"blind.bottom\"}}") ==
		"move=blind.move atTop=blind.top atBottom=blind.bottom");
}

TEST_CASE("FunctionRoles.ClimateHeatingOfANumberAndItsLevel") {
	CHECK(roles_of("{\"id\": \"room\", \"label\": \"Room\", \"kind\": \"climate\", "
		"\"datapoints\": {\"temperature\": \"room.t\", \"heating\": \"room.valve\", \"heatingLevel\": \"room.valve\"}}") ==
		"temperature=room.t heating=room.valve heatingLevel=room.valve");
}
