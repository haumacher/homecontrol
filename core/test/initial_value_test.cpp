/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The initial value of an application datapoint (homecontrol #107, haumacher/ehome #217;
// spec/runtime-image.md 3.3, format 1.2): after a start, or a new image, a datapoint with `virtual`
// that has no value (none kept across the swap, none persisted) takes its initial value with quality
// good, so a rule reading it never meets "no value". A persisted value wins; so does a value kept
// across an image swap. The restart is the device's own: a second image service on the same blob
// store and image slot.

#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/image/binary.h"
#include "hc/image/image_service.h"
#include "hc/image/memory_image_slot.h"
#include "hc/image/validator.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "hc/rules/engine.h"
#include "hc/value_store.h"
#include "image_support.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"

using hc::DatapointStatus;
using hc::Quality;
using hc::Value;
using hc::image::ImageService;
using hc::image::MemoryImageSlot;
using hc_test::describe;

namespace {

/// What survives a restart: the value store's blobs and the image slot.
struct Storage {
	hc_test::FakeBlobStore blobs;
	MemoryImageSlot slot;
};

/// One boot of the controller on `storage`.
struct Boot {
	hc_test::FakePlatform p;
	hc::Controller controller{p.platform()};
	hc::DatapointModel model{p.clock};
	hc_test::FakeUdpSocket socket{p.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), p.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter knx{controller.loop(), p.clock, model, tunnel};
	ImageService service;

	explicit Boot(Storage& storage) : service(controller, model, storage.blobs, storage.slot, knx, &tunnel) {
		controller.start();
		knx.start();
		service.start();
	}

	bool load(const std::string& json) { return hc_test::deploy(service, json); }

	void run(int turns = 20) {
		for (int i = 0; i < turns; ++i) {
			controller.run_once();
		}
	}

	const hc::Datapoint& dp(const char* name) const {
		const hc::Datapoint* d = model.find(name);
		REQUIRE_MESSAGE(d != nullptr, "no datapoint " << name);
		return *d;
	}

	void command(const char* name, const Value& value) {
		REQUIRE(model.command(dp(name).id(), value) == DatapointStatus::ok);
	}
};

std::string fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

/// The house's case: `sonnenschutz` (persisted, nobody set it) is the condition of a rule fired by
/// `sun`; `initial` is its member, empty for none.
std::string sun_shading(const char* initial) {
	return std::string(R"({"image": {"schemaVersion": "1.9", "projectId": "house", "revision": 1},
"datapoints": [
 {"id": "sonnenschutz", "type": "boolean", "writable": true, "persist": true, "virtual": {})") + initial + R"(},
 {"id": "sun", "type": "boolean", "writable": true, "virtual": {}, "initial": false},
 {"id": "blinds", "type": "boolean", "writable": true, "virtual": {}}],
"rules": [{"id": "rule-sonnenschutz", "name": "Sonnenschutz", "reactions": [{"triggers": [{"kind": "datapoint", "datapoint": "sun"}],
 "condition": {"kind": "datapoint", "name": "sonnenschutz"},
 "actions": [{"kind": "write", "datapoint": "blinds", "value": {"kind": "datapoint", "name": "sun"}}]}]}]})";
}

/// The rule's start reads two application datapoints with initial values: `setting` persisted,
/// `plain` not.
const char* const kRestart = R"({"image": {"schemaVersion": "1.9", "projectId": "restart", "revision": 1},
"datapoints": [
 {"id": "setting", "type": "boolean", "writable": true, "persist": true, "virtual": {}, "initial": true},
 {"id": "plain", "type": "unsigned", "writable": true, "virtual": {}, "initial": 5},
 {"id": "x", "type": "unsigned", "writable": true, "virtual": {}},
 {"id": "y", "type": "unsigned", "writable": true, "virtual": {}}],
"rules": [{"id": "start", "name": "Start", "reactions": [{"triggers": [{"kind": "start"}], "actions": [
 {"kind": "write", "datapoint": "x", "value": {"kind": "call", "function": "if", "args": [
  {"kind": "datapoint", "name": "setting"},
  {"kind": "literal", "type": "integer", "value": 100}, {"kind": "literal", "type": "integer", "value": 80}]}},
 {"kind": "write", "datapoint": "y", "value": {"kind": "datapoint", "name": "plain"}}]}]}]})";

/// The record of datapoint `i` in `bytes` (an image opened by hc_test::CompiledImage).
hc::image::bin::DatapointRec& record(hc_test::CompiledImage& image, size_t i) {
	const auto& r = image.view.at<hc::image::bin::DatapointRec>(hc::image::bin::kDatapoints, i);
	return *reinterpret_cast<hc::image::bin::DatapointRec*>(
		image.bytes.data() + (reinterpret_cast<const uint8_t*>(&r) - image.bytes.data()));
}

void fix_crc(std::vector<uint8_t>& b) {
	uint32_t crc = hc::image::bin::image_crc(b.data(), b.size()); // unsigned: the whole image
	for (int i = 0; i < 4; ++i) {
		b[12 + i] = static_cast<uint8_t>(crc >> (8 * i));
	}
}

} // namespace

TEST_CASE("InitialValue.EveryValueTypeOfTheCorpusCaseStartsWithItsInitialValue") {
	// The corpus case E-Home's compiler writes (spec/fixtures/images/cases/accept-initial-values.bin).
	std::string text = fixture("cases/accept-initial-values.bin");
	std::vector<uint8_t> bytes(text.begin(), text.end());
	REQUIRE(bytes.size() > 32);
	CHECK(bytes[6] == 2); // format 1.2
	Storage storage;
	Boot boot(storage);
	REQUIRE_MESSAGE(hc_test::deploy_bytes(boot.service, bytes), describe(boot.service.report()));
	hc::TimeOfDay wake{0, 6, 30, 0};
	hc::Date start{1970, 1, 1};
	const uint8_t blob[] = {0x0A, 0xFF};
	struct Expected {
		const char* name;
		Value value;
	};
	const Expected expected[] = {
		{"sonnenschutz", Value::of_bool(false)},
		{"holiday", Value::of_bool(true)},
		{"counter", Value::of_unsigned(0)},
		{"offset", Value::of_signed(-15)},
		{"setpoint", Value::of_real(21.5)},
		{"message", *Value::of_string("", 0)},
		{"greeting", *Value::of_string("Willkommen", 10)},
		{"wake", Value::of_time(wake)},
		{"holiday.start", Value::of_date(start)},
		{"mode", Value::of_enum(2)},
		{"step", Value::of_controlled(true, 3)},
		{"mood", Value::of_color(hc::Color{255, 128, 0, 0, 0x0E})},
		{"mood.white", Value::of_color(hc::Color{0, 0, 0, 200, 0x0F})},
		{"blob", *Value::of_raw(blob, 2)},
		{"nothing", *Value::of_raw(blob, 0)},
	};
	for (const Expected& e : expected) {
		CAPTURE(e.name);
		const hc::Datapoint& dp = boot.dp(e.name);
		CHECK(dp.quality() == Quality::good);
		CHECK(dp.value() == e.value);
		CHECK(dp.updated_ms() == 0); // the start, not an update: no trigger fired on it
	}
	const hc::Datapoint& last = boot.dp("last.service");
	CHECK(last.quality() == Quality::good);
	CHECK(last.value().as_date_time().year == 2026);
	CHECK(last.value().as_date_time().hour == 8);
	CHECK(last.value().as_date_time().second == 30);
	// Without an initial value (an image before 1.9) and with a technology: no value until one comes.
	CHECK(boot.dp("legacy").quality() == Quality::unknown);
	CHECK(boot.dp("sun").quality() == Quality::unknown);
}

TEST_CASE("InitialValue.TheSunShadingRuleFindsAValueInsteadOfConditionNoValue") {
	// On the house controller (2026-11-22): `rule-sonnenschutz conditionNoValue: @sonnenschutz –
	// sonnenschutz has no value` at every firing, nobody having set it since the deployment.
	{
		Storage storage;
		Boot boot(storage);
		REQUIRE_MESSAGE(boot.load(sun_shading("")), describe(boot.service.report()));
		boot.command("sun", Value::of_bool(true));
		boot.run();
		hc::rules::RuleDiagnostics d;
		REQUIRE(boot.service.engine().diagnostics(0, d));
		CHECK(d.last_error_code == hc::rules::LogCode::condition_no_value);
	}
	{
		Storage storage;
		Boot boot(storage);
		REQUIRE_MESSAGE(boot.load(sun_shading(R"(, "initial": true)")), describe(boot.service.report()));
		CHECK(boot.dp("sonnenschutz").value() == Value::of_bool(true));
		boot.command("sun", Value::of_bool(true));
		boot.run();
		hc::rules::RuleDiagnostics d;
		REQUIRE(boot.service.engine().diagnostics(0, d));
		CHECK(d.last_error_code != hc::rules::LogCode::condition_no_value);
		CHECK(d.firings == 1);
		CHECK(boot.dp("blinds").value() == Value::of_bool(true));
	}
}

TEST_CASE("InitialValue.APersistedValueWinsAfterARestartTheInitialValueOtherwise") {
	Storage storage;
	{
		Boot first(storage);
		REQUIRE_MESSAGE(first.load(kRestart), describe(first.service.report()));
		first.run();
		// The rule's start reads the initial values.
		CHECK(first.dp("setting").value() == Value::of_bool(true));
		CHECK(first.dp("x").value() == Value::of_unsigned(100));
		CHECK(first.dp("y").value() == Value::of_unsigned(5));
		first.command("setting", Value::of_bool(false));
		first.command("plain", Value::of_unsigned(7));
		first.run();
		first.service.flush();
		CHECK(first.service.value_store().pending() == 0);
	}
	Boot second(storage);
	REQUIRE(second.service.loaded());
	second.run();
	// The persisted value wins; the one not persisted starts with its initial value again.
	CHECK(second.dp("setting").value() == Value::of_bool(false));
	CHECK(second.dp("setting").quality() == Quality::good);
	CHECK(second.dp("x").value() == Value::of_unsigned(80));
	CHECK(second.dp("plain").value() == Value::of_unsigned(5));
	CHECK(second.dp("plain").quality() == Quality::good);
	CHECK(second.dp("y").value() == Value::of_unsigned(5));
}

TEST_CASE("InitialValue.ASwapKeepsTheValueANewDatapointTakesItsInitialValue") {
	Storage storage;
	Boot boot(storage);
	REQUIRE(boot.load(R"({"image": {"schemaVersion": "1.9", "projectId": "swap", "revision": 1},
"datapoints": [{"id": "a", "type": "signed", "writable": true, "virtual": {}, "initial": 1}]})"));
	CHECK(boot.dp("a").value() == Value::of_signed(1));
	boot.command("a", Value::of_signed(3));
	REQUIRE(boot.load(R"({"image": {"schemaVersion": "1.9", "projectId": "swap", "revision": 2},
"datapoints": [{"id": "a", "type": "signed", "writable": true, "virtual": {}, "initial": 2},
 {"id": "b", "type": "real", "virtual": {}, "initial": -0.5}]})"));
	// The value kept across the swap is the current one; the new datapoint has none and takes its own.
	CHECK(boot.dp("a").value() == Value::of_signed(3));
	CHECK(boot.dp("b").value() == Value::of_real(-0.5));
	CHECK(boot.dp("b").quality() == Quality::good);
	// A changed type carries no value: the datapoint starts with its initial value.
	REQUIRE(boot.load(R"({"image": {"schemaVersion": "1.9", "projectId": "swap", "revision": 3},
"datapoints": [{"id": "a", "type": "unsigned", "writable": true, "virtual": {}, "initial": 4}]})"));
	CHECK(boot.dp("a").value() == Value::of_unsigned(4));
}

TEST_CASE("InitialValue.TheDeviceRefusesAMalformedInitialValue") {
	const char* source = R"({"image": {"schemaVersion": "1.9", "projectId": "refuse", "revision": 1},
"datapoints": [
 {"id": "a", "type": "boolean", "virtual": {}, "initial": true},
 {"id": "b", "type": "signed", "virtual": {}, "initial": -15},
 {"id": "c", "type": "real", "virtual": {}, "initial": 2.5},
 {"id": "k", "type": "boolean", "knx": {"dpt": "1.001", "listen": ["1/1/1"]}}]})";
	struct Mutation {
		const char* what;
		void (*apply)(hc_test::CompiledImage& image);
		const char* path;
		const char* reason;
	};
	const Mutation mutations[] = {
		{"an unknown bit of init",
			[](hc_test::CompiledImage& image) { record(image, 0).init = 0x03; },
			"datapoints[0].init", "malformed: unknown bits 0x2 in init"},
		{"an initial value of a KNX datapoint",
			[](hc_test::CompiledImage& image) {
				record(image, 3).init = 0x01;
				record(image, 3).initial = record(image, 0).initial;
			},
			"datapoints[3].init", "malformed: an initial value of a datapoint without virtual"},
		{"a constant that does not exist",
			[](hc_test::CompiledImage& image) { record(image, 0).initial = 999; },
			"datapoints[0].initial", "malformed: constant 999 does not exist (3 constants)"},
		{"a constant of another type",
			[](hc_test::CompiledImage& image) { record(image, 1).initial = record(image, 2).initial; },
			"datapoints[1].initial", "malformed: constant 2 is a real, the signed datapoint takes integer"},
		{"a value outside the datapoint's range",
			[](hc_test::CompiledImage& image) { record(image, 1).type = 2; }, // signed → unsigned, initial -15
			"datapoints[1].initial", "malformed: constant 1 does not fit the unsigned datapoint"},
	};
	for (const Mutation& m : mutations) {
		CAPTURE(m.what);
		hc_test::CompiledImage image;
		REQUIRE(image.compile(source));
		m.apply(image);
		fix_crc(image.bytes);
		hc::image::Report report;
		CHECK_FALSE(hc::image::check_structure(image.bytes.data(), report));
		bool found = false;
		for (size_t i = 0; i < report.count; ++i) {
			found = found || (std::string(report.refusals[i].path) == m.path && std::strstr(report.refusals[i].reason, m.reason));
		}
		CHECK_MESSAGE(found, describe(report));
	}
	// Without the bit the field is not read: an image of format 1.1 and before loads as it did.
	hc_test::CompiledImage image;
	REQUIRE(image.compile(source));
	record(image, 0).init = 0;
	record(image, 0).initial = 999;
	fix_crc(image.bytes);
	Storage storage;
	Boot boot(storage);
	REQUIRE_MESSAGE(hc_test::deploy_bytes(boot.service, image.bytes), describe(boot.service.report()));
	CHECK(boot.dp("a").quality() == Quality::unknown);
	CHECK(boot.dp("b").value() == Value::of_signed(-15));
}
