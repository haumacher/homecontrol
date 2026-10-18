/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Reporting overrides of the Zigbee bindings (spec/zigbee.md "Reporting overrides", haumacher/ehome
// #64): the conversion of a change in the datapoint's unit to the attribute's raw step, the checks
// of resolve_reporting() and configure(), the effective values at the interview, after a change of
// the bindings and in the export, and the stale rule without a periodic report.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/zigbee/adapter.h"
#include "hc/zigbee/cluster_mapping.h"
#include "zigbee_test_support.h"

using hc::Quality;
using hc::zigbee::Function;
using hc::zigbee::Reporting;
using hc::zigbee::ReportingMember;
using hc::zigbee::ReportingOverride;
using hc::zigbee::ZigbeeOverride;
using hc_test::kBulb;
using hc_test::kBulbShort;
using hc_test::kSensor;
using hc_test::kSensorShort;
using hc_test::ZigbeeFixture;
namespace cluster = hc::zigbee::cluster;
namespace attribute = hc::zigbee::attribute;
namespace zcl_type = hc::zigbee::zcl_type;

namespace {

const char* const kSensorTemperature = "zigbee.00158d0005060708.1.temperature";
const char* const kSensorHumidity = "zigbee.00158d0005060708.1.humidity";
const char* const kSensorBattery = "zigbee.00158d0005060708.1.battery";
const char* const kBulbOn = "zigbee.00124b0001020304.11.on";

ZigbeeOverride temperature_binding(const char* name) {
	ZigbeeOverride binding;
	binding.name = name;
	binding.ieee = kSensor;
	binding.endpoint = 1;
	binding.cluster = cluster::kTemperatureMeasurement;
	binding.attribute = attribute::kMeasuredValue;
	return binding;
}

ZigbeeOverride with_reporting(ZigbeeOverride binding, uint8_t set, uint16_t min_s, uint16_t max_s, double change) {
	binding.reporting.set = set;
	binding.reporting.min_s = min_s;
	binding.reporting.max_s = max_s;
	binding.reporting.change = change;
	return binding;
}

std::string export_of(const hc::zigbee::ZigbeeAdapter& adapter, hc::zigbee::Ieee ieee) {
	for (size_t i = 0; i < adapter.devices().size(); ++i) {
		if (adapter.devices().at(i).ieee == ieee) {
			char buffer[8192];
			hc::JsonWriter json(buffer, sizeof(buffer));
			adapter.write_device_export_json(json, i);
			REQUIRE_FALSE(json.overflowed());
			return std::string(json.data(), json.length());
		}
	}
	return "";
}

bool contains(const std::string& text, const std::string& part) {
	return text.find(part) != std::string::npos;
}

/// The model's datapoint names, in order.
std::vector<std::string> names(const hc::DatapointModel& model) {
	std::vector<std::string> out;
	for (size_t i = 0; i < model.size(); ++i) {
		out.push_back(model.get(static_cast<uint16_t>(i))->name());
	}
	return out;
}

} // namespace

TEST_CASE("ZigbeeReporting: a change in the datapoint's unit maps to the attribute's step") {
	uint16_t raw = 0;
	CHECK(hc::zigbee::change_to_raw(Function::temperature, 0.5, raw));
	CHECK(raw == 50);
	CHECK(hc::zigbee::change_to_raw(Function::temperature, 327.67, raw));
	CHECK(raw == 32767);
	CHECK(hc::zigbee::change_to_raw(Function::humidity, 2.5, raw));
	CHECK(raw == 250);
	CHECK(hc::zigbee::change_to_raw(Function::battery, 1.5, raw));
	CHECK(raw == 3);
	CHECK(hc::zigbee::change_to_raw(Function::level, 0.4, raw));
	CHECK(raw == 1);
	CHECK(hc::zigbee::change_to_raw(Function::level, 100, raw));
	CHECK(raw == 254);
	CHECK(hc::zigbee::change_to_raw(Function::temperature, 0, raw));
	CHECK(raw == 0);
	const char* error = nullptr;
	CHECK_FALSE(hc::zigbee::change_to_raw(Function::temperature, 327.68, raw, &error));
	CHECK(std::string(error) == "change must be 0..327.67 (\xc2\xb0" "C)");
	CHECK_FALSE(hc::zigbee::change_to_raw(Function::humidity, -0.1, raw, &error));
	CHECK(std::string(error) == "change must be 0..100 (%)");
	CHECK_FALSE(hc::zigbee::change_to_raw(Function::occupancy, 1, raw, &error));
	CHECK(std::string(error) == "the function occupancy has no reportable change (a discrete value reports every change)");
	CHECK_FALSE(hc::zigbee::change_to_raw(Function::color_temperature, 100, raw, &error));
	CHECK(std::string(error).find("the function color_temperature has no reportable change in K") == 0);

	// Back into the unit: the defaults as the export shows them, and a round trip through it.
	double change = 0;
	CHECK(hc::zigbee::change_to_unit(Function::temperature, 10, change));
	CHECK(change == doctest::Approx(0.1));
	CHECK(hc::zigbee::change_to_unit(Function::level, 1, change));
	CHECK(change == doctest::Approx(0.4));
	for (uint16_t step = 0; step <= 254; ++step) {
		REQUIRE(hc::zigbee::change_to_unit(Function::level, step, change));
		REQUIRE(hc::zigbee::change_to_raw(Function::level, change, raw));
		CHECK(raw == step);
	}
	CHECK_FALSE(hc::zigbee::change_to_unit(Function::illuminance, 500, change));
	CHECK_FALSE(hc::zigbee::change_to_unit(Function::on_off, 0, change));
}

TEST_CASE("ZigbeeReporting: the effective values and the refusals of resolve_reporting") {
	Reporting out;
	ReportingOverride none;
	REQUIRE(hc::zigbee::resolve_reporting(Function::temperature, none, out));
	CHECK(out == hc::zigbee::default_reporting(Function::temperature));
	CHECK(out.min_s == 30);
	CHECK(out.max_s == 600);
	CHECK(out.change == 10);

	ReportingOverride only_max;
	only_max.set = ReportingOverride::kMax;
	only_max.max_s = 900;
	REQUIRE(hc::zigbee::resolve_reporting(Function::temperature, only_max, out));
	CHECK(out.min_s == 30);
	CHECK(out.max_s == 900);
	CHECK(out.change == 10);
	CHECK(out.periodic());
	// ZCL's special maxima: 0 (on change only) and 65535 (no reports) are below any min and not periodic.
	only_max.max_s = Reporting::kNoReports;
	REQUIRE(hc::zigbee::resolve_reporting(Function::battery, only_max, out));
	CHECK_FALSE(out.periodic());
	only_max.max_s = Reporting::kNoPeriodic;
	REQUIRE(hc::zigbee::resolve_reporting(Function::battery, only_max, out));
	CHECK(out.min_s == 3600);
	CHECK_FALSE(out.periodic());

	ReportingMember member = ReportingMember::whole;
	const char* error = nullptr;
	ReportingOverride crossed;
	crossed.set = ReportingOverride::kMin | ReportingOverride::kMax;
	crossed.min_s = 900;
	crossed.max_s = 30;
	CHECK_FALSE(hc::zigbee::resolve_reporting(Function::temperature, crossed, out, &member, &error));
	CHECK(member == ReportingMember::max);
	CHECK(std::string(error) == "max must not be below min (an absent min is the function's default); 0: no periodic "
		"reports, 65535: no reports");
	ReportingOverride only_min;
	only_min.set = ReportingOverride::kMin;
	only_min.min_s = 700;
	CHECK_FALSE(hc::zigbee::resolve_reporting(Function::temperature, only_min, out, &member, &error));
	CHECK(member == ReportingMember::min);
	only_min.min_s = 0xFFFF;
	CHECK_FALSE(hc::zigbee::resolve_reporting(Function::temperature, only_min, out, &member, &error));
	CHECK(std::string(error) == "min must be 0..65534");
	ReportingOverride change;
	change.set = ReportingOverride::kChange;
	change.change = 1;
	CHECK_FALSE(hc::zigbee::resolve_reporting(Function::on_off, change, out, &member, &error));
	CHECK(member == ReportingMember::change);
	CHECK(std::string(error) == "the function on_off has no reportable change (a discrete value reports every change)");
	CHECK_FALSE(hc::zigbee::resolve_reporting(Function::multistate, only_max, out, &member, &error));
	CHECK(member == ReportingMember::whole);
	CHECK(std::string(error) == "the function multistate configures no reporting (its reports are events)");
	CHECK_FALSE(hc::zigbee::resolve_reporting(Function::ias_zone, only_max, out, &member, &error));
	CHECK(std::string(error) == "the function ias_zone configures no reporting (zone status change notifications)");
}

TEST_CASE("ZigbeeReporting: configure() refuses a reporting override with the loader's reasons") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	const char* error = nullptr;
	ZigbeeOverride crossed = with_reporting(temperature_binding("t"), ReportingOverride::kMin | ReportingOverride::kMax,
		900, 30, 0);
	CHECK_FALSE(f.adapter.configure(&crossed, 1, &error));
	CHECK(std::string(error) == "max must not be below min (an absent min is the function's default); 0: no periodic "
		"reports, 65535: no reports");
	CHECK(capture.sink.contains("bindings refused: binding 0 ('t'): max must not be below min"));
	ZigbeeOverride on;
	on.name = "on";
	on.ieee = kBulb;
	on.endpoint = 11;
	on.cluster = cluster::kOnOff;
	on.attribute = attribute::kOnOff;
	on = with_reporting(on, ReportingOverride::kChange, 0, 0, 1);
	CHECK_FALSE(f.adapter.configure(&on, 1, &error));
	CHECK(std::string(error) == "the function on_off has no reportable change (a discrete value reports every change)");
	ZigbeeOverride button;
	button.name = "button";
	button.ieee = hc_test::kButton;
	button.endpoint = 1;
	button.cluster = cluster::kOnOff;
	button.client = true;
	button = with_reporting(button, ReportingOverride::kMax, 0, 600, 0);
	CHECK_FALSE(f.adapter.configure(&button, 1, &error));
	CHECK(std::string(error) == "the function action (a client binding, button commands) configures no reporting");
	ZigbeeOverride negative = with_reporting(temperature_binding("t"), ReportingOverride::kChange, 0, 0, -0.5);
	CHECK_FALSE(f.adapter.configure(&negative, 1, &error));
	CHECK(std::string(error) == "change must be 0..327.67 (\xc2\xb0" "C)");
	// Nothing changed: no bindings, the generic rules.
	f.join_sensor();
	CHECK(f.dp(kSensorTemperature) != nullptr);
}

TEST_CASE("ZigbeeReporting: the interview configures the override, a binding change reconfigures the device") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	ZigbeeOverride binding = with_reporting(temperature_binding("bath.temperature"),
		ReportingOverride::kMin | ReportingOverride::kMax | ReportingOverride::kChange, 30, 900, 0.5);
	REQUIRE(f.adapter.configure(&binding, 1));
	f.join_sensor();
	// Only the bound function is configured, with the override.
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 30..900 50"));
	CHECK(f.driver.count("configure 0x3c4d") == 1);
	CHECK(f.adapter.stale_timeout_s(kSensor) == 3 * 900);
	std::string view = export_of(f.adapter, kSensor);
	CHECK(contains(view, "\"function\":\"temperature\",\"endpoint\":1,\"cluster\":1026,\"attribute\":0,\"type\":\"real\","
		"\"unit\":\"\xc2\xb0" "C\",\"writable\":false,\"reporting\":{\"min\":30,\"max\":900,\"change\":0.5},"
		"\"datapoint\":\"bath.temperature\""));
	// The unbound functions of the bound device: listed with the defaults and without a datapoint.
	CHECK(contains(view, "\"function\":\"humidity\",\"endpoint\":1,\"cluster\":1029,\"attribute\":0,\"type\":\"real\","
		"\"unit\":\"%\",\"writable\":false,\"reporting\":{\"min\":30,\"max\":600,\"change\":1}}"));
	CHECK(contains(view, "\"reporting\":{\"min\":3600,\"max\":21600,\"change\":1}}"));

	// Another override: the changed function is configured again, at once.
	f.driver.calls.clear();
	binding = with_reporting(temperature_binding("bath.temperature"), ReportingOverride::kMax, 0, 1200, 0);
	REQUIRE(f.adapter.configure(&binding, 1));
	CHECK(f.driver.calls == std::vector<std::string>{"bind 0x3c4d 1 0x0402", "configure 0x3c4d 1 0x0402 0x0000 30..1200 10"});
	CHECK(capture.sink.contains("the new bindings change the reporting of 1 attribute(s): configured again"));
	CHECK(f.adapter.stale_timeout_s(kSensor) == 3 * 1200);
	// The same bindings again: nothing to do.
	f.driver.calls.clear();
	REQUIRE(f.adapter.configure(&binding, 1));
	CHECK(f.driver.count("configure") == 0);
	// No bindings: the generic rules' defaults for the temperature, and the humidity and battery the
	// bound device never had configured.
	REQUIRE(f.adapter.configure(nullptr, 0));
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 30..600 10"));
	CHECK(f.driver.has("configure 0x3c4d 1 0x0405 0x0000 30..600 100"));
	CHECK(f.driver.has("configure 0x3c4d 1 0x0001 0x0021 3600..21600 2"));
	CHECK(f.driver.has("bind 0x3c4d 1 0x0405"));
	CHECK(f.driver.count("configure") == 3);
}

TEST_CASE("ZigbeeReporting: a device not yet interviewed is configured when its interview finishes") {
	ZigbeeFixture f;
	f.start_network();
	ZigbeeOverride binding = temperature_binding("bath.temperature");
	REQUIRE(f.adapter.configure(&binding, 1));
	// The device announces itself; its interview runs while the bindings change.
	f.adapter.device_announced(ZigbeeFixture::node(kSensor, kSensorShort));
	REQUIRE(f.adapter.interviewing() == kSensor);
	binding = with_reporting(binding, ReportingOverride::kMin | ReportingOverride::kChange, 60, 0, 0.25);
	REQUIRE(f.adapter.configure(&binding, 1));
	CHECK(f.driver.count("configure") == 0);
	uint8_t endpoints[] = {1};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSensorShort), 0, endpoints, 1);
	f.descriptor(kSensorShort, 1, hc::zigbee::kHomeAutomationProfile, 0x0302, {0x0000, 0x0001, 0x0003, 0x0402, 0x0405},
		{0x0019});
	f.basic(kSensorShort, 1, "LUMI", "lumi.weather");
	CHECK(f.adapter.interviewing() == 0);
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 60..600 25"));
	CHECK(f.driver.count("configure") == 1);
}

TEST_CASE("ZigbeeReporting: a device whose only function reports no periodic value never goes stale") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	ZigbeeOverride bindings[2];
	bindings[0] = with_reporting(temperature_binding("bath.temperature"), ReportingOverride::kMax, 0,
		Reporting::kNoReports, 0);
	bindings[1].name = "living.ceiling";
	bindings[1].ieee = kBulb;
	bindings[1].endpoint = 11;
	bindings[1].cluster = cluster::kOnOff;
	bindings[1].attribute = attribute::kOnOff;
	REQUIRE(f.adapter.configure(bindings, 2));
	f.join_sensor();
	f.join_bulb();
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 30..65535 10"));
	CHECK(f.adapter.stale_timeout_s(kSensor) == 0);
	CHECK(f.adapter.stale_timeout_s(kBulb) == 900);
	f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 2150);
	f.report(kBulbShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 1);
	// Past three times the temperature's default maximum (600 s), and far past the bulb's 900 s.
	f.run_for(3 * 600 * 1000 + 60 * 1000);
	CHECK(f.dp("bath.temperature")->quality() == Quality::good);
	CHECK_FALSE(f.adapter.devices().find(kSensor)->stale);
	CHECK(f.dp("living.ceiling")->quality() == Quality::stale);
	CHECK(f.adapter.devices().find(kBulb)->stale);
	// The export shows what the device was told.
	CHECK(contains(export_of(f.adapter, kSensor), "\"reporting\":{\"min\":30,\"max\":65535,\"change\":0.1}"));
}

// ---- Probe: the override composed with the generic fallback ------------------------------------------

TEST_CASE("ZigbeeReporting probe: an override changes no datapoint, only the device's reporting") {
	ZigbeeFixture f;
	f.start_network();
	f.join_sensor();
	f.join_bulb();
	REQUIRE(f.dp(kSensorHumidity) != nullptr);
	ZigbeeOverride binding = temperature_binding("bath.temperature");
	REQUIRE(f.adapter.configure(&binding, 1));
	std::vector<std::string> plain = names(f.model);
	// The bound device has exactly its binding; the unbound bulb keeps its generic datapoints.
	CHECK(f.dp("bath.temperature") != nullptr);
	CHECK(f.dp(kSensorTemperature) == nullptr);
	CHECK(f.dp(kSensorHumidity) == nullptr);
	CHECK(f.dp(kSensorBattery) == nullptr);
	CHECK(f.dp(kBulbOn) != nullptr);
	std::string plain_export = export_of(f.adapter, kSensor);
	f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 2150);

	ZigbeeOverride tuned = with_reporting(binding, ReportingOverride::kMin | ReportingOverride::kChange, 10, 0, 0.3);
	REQUIRE(f.adapter.configure(&tuned, 1));
	CHECK(names(f.model) == plain);
	CHECK(f.dp("bath.temperature")->value() == hc::Value::of_real(21.5));
	CHECK(f.adapter.point_count() == plain.size());
	// The export lists the same functions; only the temperature's reporting differs.
	std::string tuned_export = export_of(f.adapter, kSensor);
	CHECK(contains(tuned_export, "\"reporting\":{\"min\":10,\"max\":600,\"change\":0.3}"));
	CHECK(contains(plain_export, "\"reporting\":{\"min\":30,\"max\":600,\"change\":0.1}"));
	std::string expected = plain_export;
	size_t at = expected.find("\"reporting\":{\"min\":30,\"max\":600,\"change\":0.1}");
	REQUIRE(at != std::string::npos);
	expected.replace(at, std::string("\"reporting\":{\"min\":30,\"max\":600,\"change\":0.1}").size(),
		"\"reporting\":{\"min\":10,\"max\":600,\"change\":0.3}");
	CHECK(tuned_export == expected);
	// The bulb, bound by nothing, was not asked anything.
	CHECK(f.driver.count("configure 0x1a2b") == 3);
}
