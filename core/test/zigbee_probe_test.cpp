/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the Zigbee stage one (issue #15): a bulb's life from join
// through reports, a command, silence, recovery and removal, and an image binding renaming a point.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/zigbee/adapter.h"
#include "hc/zigbee/cluster_mapping.h"
#include "test_support.h"
#include "zigbee_test_support.h"

using hc::Quality;
using hc::Value;
using hc::ValueType;
using hc::zigbee::ZigbeeAdapter;
using hc_test::kBulb;
using hc_test::kBulbShort;
using hc_test::ZigbeeFixture;

namespace {

void join_bulb(ZigbeeFixture& f) {
	f.adapter.device_announced(ZigbeeFixture::node(kBulb, kBulbShort));
	uint8_t endpoints[] = {11};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kBulbShort), 0, endpoints, 1);
	f.descriptor(kBulbShort, 11, hc::zigbee::kHomeAutomationProfile, 0x010C,
		{0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0008}, {0x0019});
	f.basic(kBulbShort, 11, "Probe Lights", "PL-1");
	REQUIRE(f.adapter.interviewing() == 0);
}

size_t calls_mentioning(const ZigbeeFixture& f, const std::string& text) {
	size_t n = 0;
	for (const std::string& c : f.driver.calls) {
		n += c.find(text) != std::string::npos ? 1 : 0;
	}
	return n;
}

} // namespace

TEST_CASE("ZigbeeProbe.BulbLifeCycleFromJoinToRemoval") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	REQUIRE(f.adapter.permit_join(60));
	f.adapter.permit_join_status(60);
	join_bulb(f);
	REQUIRE(f.model.size() == 2); // on/off and level
	const hc::Datapoint* on = nullptr;
	const hc::Datapoint* level = nullptr;
	for (uint16_t id = 0; id < 2; ++id) {
		const hc::Datapoint* d = f.model.get(id);
		if (d->type() == ValueType::boolean) {
			on = d;
		} else {
			level = d;
		}
	}
	REQUIRE(on != nullptr);
	REQUIRE(level != nullptr);
	CHECK(std::string(on->name()).rfind("zigbee.00124b0001020304.11.", 0) == 0);
	// Reports: on, then level 127 of 254 = 50 %.
	f.report(kBulbShort, 11, hc::zigbee::cluster::kOnOff, 0x0000, hc::zigbee::zcl_type::kBool, 1);
	CHECK(on->value() == Value::of_bool(true));
	CHECK(on->quality() == Quality::good);
	f.report(kBulbShort, 11, hc::zigbee::cluster::kLevelControl, 0x0000, hc::zigbee::zcl_type::kUint8, 127);
	CHECK(level->value().as_real() == doctest::Approx(50.0));
	// A command goes to the driver, addressed to the bulb; the value waits for the report.
	size_t before = calls_mentioning(f, "0x1a2b");
	REQUIRE(f.model.command(on->id(), Value::of_bool(false)) == hc::DatapointStatus::ok);
	CHECK(calls_mentioning(f, "0x1a2b") == before + 1);
	// Silence: after three reporting intervals the bulb is stale; a report revives it.
	f.run_for(3ull * 300 * 1000 + ZigbeeAdapter::kCheckPeriodMs + 1000);
	CHECK(on->quality() == Quality::stale);
	CHECK(level->quality() == Quality::stale);
	f.report(kBulbShort, 11, hc::zigbee::cluster::kOnOff, 0x0000, hc::zigbee::zcl_type::kBool, 0);
	CHECK(on->quality() == Quality::good);
	CHECK(on->value() == Value::of_bool(false));
	// Removal: the device leaves the database and the model, and the driver is told.
	const char* error = nullptr;
	REQUIRE(f.adapter.remove(kBulb, &error));
	CHECK(f.model.size() == 0);
	CHECK(f.adapter.devices().find(kBulb) == nullptr);
	CHECK(calls_mentioning(f, "leave") >= 1);
	CHECK_FALSE(f.adapter.remove(kBulb, &error)); // gone already
	CHECK(error != nullptr);
}

TEST_CASE("ZigbeeProbe.AnImageBindingNamesTheBulbAndSurvivesARejoin") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	hc::zigbee::ZigbeeOverride overrides[1];
	overrides[0].name = "living.bulb";
	overrides[0].ieee = kBulb;
	overrides[0].endpoint = 11;
	overrides[0].cluster = hc::zigbee::cluster::kOnOff;
	overrides[0].attribute = 0x0000;
	overrides[0].command_cluster = hc::zigbee::cluster::kOnOff;
	const char* error = nullptr;
	REQUIRE(f.adapter.configure(overrides, 1, &error)); // before the device is known
	REQUIRE(f.adapter.permit_join(60));
	join_bulb(f);
	// With a binding, the device gets only its bound datapoints, under the image's name.
	REQUIRE(f.model.size() == 1);
	const hc::Datapoint* bulb = f.dp("living.bulb");
	REQUIRE(bulb != nullptr);
	CHECK(bulb->writable());
	f.report(kBulbShort, 11, hc::zigbee::cluster::kOnOff, 0x0000, hc::zigbee::zcl_type::kBool, 1);
	CHECK(bulb->value() == Value::of_bool(true));
	// The device rejoins with another short address: the name and the value stay.
	f.adapter.device_announced(ZigbeeFixture::node(kBulb, 0x2b3c));
	f.run_for(1000);
	const hc::Datapoint* again = f.dp("living.bulb");
	REQUIRE(again != nullptr);
	CHECK(again->value() == Value::of_bool(true));
	// A binding for an unknown attribute of a known cluster is refused with a reason.
	hc::zigbee::ZigbeeOverride bad = overrides[0];
	bad.name = "living.nonsense";
	bad.attribute = 0x4242;
	CHECK_FALSE(f.adapter.configure(&bad, 1, &error));
	CHECK(error != nullptr);
	CHECK(f.dp("living.bulb") != nullptr); // the previous bindings stay
}
