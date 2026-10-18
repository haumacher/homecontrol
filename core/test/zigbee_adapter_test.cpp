/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The Zigbee adapter under the fake clock: join, interview, datapoints, reports, commands, stale
// devices, the persisted database, the image's bindings and the refusals.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/zigbee/adapter.h"
#include "zigbee_test_support.h"

using hc::DatapointFlags;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::Quality;
using hc::Value;
using hc::ValueType;
using hc::zigbee::ZigbeeAdapter;
using hc::zigbee::ZigbeeOverride;
using hc_test::kBulb;
using hc_test::kBulbShort;
using hc_test::kButton;
using hc_test::kButtonShort;
using hc_test::kContact;
using hc_test::kContactShort;
using hc_test::kSensor;
using hc_test::kSensorShort;
using hc_test::ZigbeeFixture;
namespace cluster = hc::zigbee::cluster;
namespace attribute = hc::zigbee::attribute;
namespace zcl_type = hc::zigbee::zcl_type;

namespace {

const char* const kBulbOn = "zigbee.00124b0001020304.11.on";
const char* const kBulbLevel = "zigbee.00124b0001020304.11.level";
const char* const kBulbCt = "zigbee.00124b0001020304.11.color_temperature";
const char* const kSensorTemperature = "zigbee.00158d0005060708.1.temperature";
const char* const kSensorHumidity = "zigbee.00158d0005060708.1.humidity";
const char* const kSensorBattery = "zigbee.00158d0005060708.1.battery";
const char* const kContactPoint = "zigbee.00158d00090a0b0c.1.contact";
const char* const kButtonAction = "zigbee.000b57fffe112233.1.action";

} // namespace

TEST_CASE("ZigbeeAdapter: a bulb joins, is interviewed and becomes datapoints") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	REQUIRE(f.adapter.permit_join(60));
	CHECK(f.driver.has("permit 60"));
	f.adapter.permit_join_status(60);
	CHECK(capture.sink.contains("network open for joining for 60 s"));

	f.adapter.device_announced(ZigbeeFixture::node(kBulb, kBulbShort));
	CHECK(f.adapter.interviewing() == kBulb);
	CHECK(f.driver.calls.back() == "active 0x1a2b");
	uint8_t endpoints[] = {11, 242};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kBulbShort), 0, endpoints, 2);
	CHECK(f.driver.calls.back() == "simple 0x1a2b 11");
	f.descriptor(kBulbShort, 11, hc::zigbee::kHomeAutomationProfile, 0x010C,
		{0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0008, 0x0300, 0x1000}, {0x0019});
	CHECK(f.driver.calls.back() == "simple 0x1a2b 242");
	f.descriptor(kBulbShort, 242, 0xA1E0, 0x0061, {}, {0x0021});
	CHECK(capture.sink.contains("endpoint 242 has profile 0xa1e0: not a device function, ignored"));
	CHECK(f.driver.calls.back() == "read 0x1a2b 11 0x0000 4,5");
	CHECK(f.model.size() == 0);
	f.basic(kBulbShort, 11, "IKEA of Sweden", "TRADFRI bulb E27 WS 806lm");

	CHECK(f.adapter.interviewing() == 0);
	CHECK(capture.sink.contains(
		"device 00124b0001020304 interviewed: \"IKEA of Sweden\" \"TRADFRI bulb E27 WS 806lm\", 1 endpoint(s), 3 datapoint(s)"));
	// The datapoints.
	REQUIRE(f.model.size() == 3);
	const hc::Datapoint* on = f.dp(kBulbOn);
	REQUIRE(on != nullptr);
	CHECK(on->type() == ValueType::boolean);
	CHECK(on->writable());
	CHECK(on->transmits());
	CHECK(on->technology() == &f.adapter);
	CHECK(f.dp(kBulbLevel)->type() == ValueType::real);
	CHECK(f.dp(kBulbCt)->type() == ValueType::unsigned_int);
	// Reporting configured and bound to the coordinator.
	CHECK(f.driver.has("bind 0x1a2b 11 0x0006"));
	CHECK(f.driver.has("bind 0x1a2b 11 0x0008"));
	CHECK(f.driver.has("bind 0x1a2b 11 0x0300"));
	CHECK(f.driver.has("configure 0x1a2b 11 0x0006 0x0000 0..300 0"));
	CHECK(f.driver.has("configure 0x1a2b 11 0x0008 0x0000 1..300 1"));
	CHECK(f.driver.has("configure 0x1a2b 11 0x0300 0x0007 1..300 10"));
	// The database is written.
	CHECK(f.store.writes_of["zigbee-devices.json"] == 1);
	const hc::zigbee::ZigbeeDevice* device = f.adapter.devices().find(kBulb);
	REQUIRE(device != nullptr);
	CHECK(device->interviewed);
	CHECK(device->endpoint_count == 1);
	CHECK(std::string(device->model) == "TRADFRI bulb E27 WS 806lm");
	// The values are read right after.
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.has("read 0x1a2b 11 0x0006 0"));
	CHECK(f.driver.has("read 0x1a2b 11 0x0008 0"));
	CHECK(f.driver.has("read 0x1a2b 11 0x0300 7"));
}

TEST_CASE("ZigbeeAdapter: reports update the values") {
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	f.join_sensor();
	f.report(kBulbShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 1);
	CHECK(f.dp(kBulbOn)->value() == Value::of_bool(true));
	CHECK(f.dp(kBulbOn)->quality() == Quality::good);
	f.report(kBulbShort, 11, cluster::kLevelControl, attribute::kCurrentLevel, zcl_type::kUint8, 127);
	CHECK(f.dp(kBulbLevel)->value() == Value::of_real(50.0));
	f.report(kBulbShort, 11, cluster::kColorControl, attribute::kColorTemperature, zcl_type::kUint16, 250);
	CHECK(f.dp(kBulbCt)->value() == Value::of_unsigned(4000));
	f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 2150);
	CHECK(f.dp(kSensorTemperature)->value() == Value::of_real(21.5));
	f.report(kSensorShort, 1, cluster::kRelativeHumidity, attribute::kMeasuredValue, zcl_type::kUint16, 4810);
	CHECK(f.dp(kSensorHumidity)->value() == Value::of_real(48.1));
	f.report(kSensorShort, 1, cluster::kPowerConfiguration, attribute::kBatteryPercentage, zcl_type::kUint8, 180);
	CHECK(f.dp(kSensorBattery)->value() == Value::of_real(90.0));
	// The invalid code leaves the value.
	f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, -0x8000);
	CHECK(f.dp(kSensorTemperature)->value() == Value::of_real(21.5));
	// A report of an unmapped attribute or endpoint changes nothing.
	f.report(kSensorShort, 2, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 0);
	CHECK(f.dp(kSensorTemperature)->value() == Value::of_real(21.5));
	// A read response with a failed record and a good one.
	uint8_t on = 0;
	hc::zigbee::AttributeRecord records[2];
	records[0].attribute = 0x4003;
	records[0].status = hc::zigbee::zcl_status::kUnsupportedAttribute;
	records[1].attribute = attribute::kOnOff;
	records[1].type = zcl_type::kBool;
	records[1].data = &on;
	records[1].length = 1;
	f.adapter.attributes(ZigbeeFixture::by_short(kBulbShort), 11, cluster::kOnOff, records, 2);
	CHECK(f.dp(kBulbOn)->value() == Value::of_bool(false));
}

TEST_CASE("ZigbeeAdapter: a value that does not decode is logged and ignored") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	f.report(kBulbShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 1);
	uint8_t data[2] = {1, 0};
	hc::zigbee::AttributeRecord record;
	record.attribute = attribute::kOnOff;
	record.type = zcl_type::kBool;
	record.data = data;
	record.length = 2;
	f.adapter.attributes(ZigbeeFixture::by_short(kBulbShort), 11, cluster::kOnOff, &record, 1);
	CHECK(capture.sink.contains("00124b0001020304 endpoint 11 cluster 0x0006 attribute 0x0000 (type 0x10) not decoded: "
		"length does not match the ZCL data type"));
	CHECK(f.dp(kBulbOn)->value() == Value::of_bool(true));
}

TEST_CASE("ZigbeeAdapter: commands go through the driver") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	CHECK(f.model.command(f.dp(kBulbOn)->id(), Value::of_bool(true)) == DatapointStatus::ok);
	CHECK(f.driver.calls.back() == "onoff 0x1a2b 11 on");
	CHECK(f.model.command(f.dp(kBulbOn)->id(), Value::of_bool(false)) == DatapointStatus::ok);
	CHECK(f.driver.calls.back() == "onoff 0x1a2b 11 off");
	CHECK(f.model.command(f.dp(kBulbLevel)->id(), Value::of_real(50.0)) == DatapointStatus::ok);
	CHECK(f.driver.calls.back() == "level 0x1a2b 11 127 5");
	CHECK(f.model.command(f.dp(kBulbLevel)->id(), Value::of_unsigned(100)) == DatapointStatus::ok);
	CHECK(f.driver.calls.back() == "level 0x1a2b 11 254 5");
	CHECK(f.model.command(f.dp(kBulbCt)->id(), Value::of_unsigned(2700)) == DatapointStatus::ok);
	CHECK(f.driver.calls.back() == "ct 0x1a2b 11 370 5");
	// The model keeps the commanded value until the report confirms or corrects it.
	CHECK(f.dp(kBulbLevel)->value() == Value::of_real(100.0));
	f.report(kBulbShort, 11, cluster::kLevelControl, attribute::kCurrentLevel, zcl_type::kUint8, 200);
	CHECK(f.dp(kBulbLevel)->value() == Value::of_real(78.7));

	// Refusals.
	size_t calls = f.driver.calls.size();
	CHECK(f.model.command(f.dp(kBulbLevel)->id(), Value::of_real(101.0)) == DatapointStatus::refused);
	CHECK(capture.sink.contains("a level takes 0..100 %"));
	CHECK(f.model.command(f.dp(kBulbCt)->id(), Value::of_unsigned(10)) == DatapointStatus::refused);
	CHECK(capture.sink.contains("a colour temperature takes 16..1000000 K"));
	CHECK(f.driver.calls.size() == calls);
	f.driver.accept = false;
	CHECK(f.model.command(f.dp(kBulbOn)->id(), Value::of_bool(true)) == DatapointStatus::refused);
	CHECK(capture.sink.contains("command of 'zigbee.00124b0001020304.11.on' refused: the Zigbee stack did not take it"));
	CHECK(f.dp(kBulbOn)->value() == Value::of_bool(false));
}

TEST_CASE("ZigbeeAdapter: a sensor's datapoints are read-only") {
	ZigbeeFixture f;
	f.start_network();
	f.join_sensor();
	REQUIRE(f.model.size() == 3);
	CHECK_FALSE(f.dp(kSensorTemperature)->writable());
	CHECK(f.model.command(f.dp(kSensorTemperature)->id(), Value::of_real(20.0)) == DatapointStatus::not_writable);
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 30..600 10"));
	CHECK(f.driver.has("configure 0x3c4d 1 0x0405 0x0000 30..600 100"));
	CHECK(f.driver.has("configure 0x3c4d 1 0x0001 0x0021 3600..21600 2"));
	CHECK(f.adapter.stale_timeout_s(kSensor) == 3 * 21600);
}

TEST_CASE("ZigbeeAdapter: a contact sensor is enrolled and its zone notifications are reported") {
	ZigbeeFixture f;
	f.start_network();
	f.adapter.device_announced(ZigbeeFixture::node(kContact, kContactShort));
	uint8_t endpoints[] = {1};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kContactShort), 0, endpoints, 1);
	f.descriptor(kContactShort, 1, hc::zigbee::kHomeAutomationProfile, 0x0402, {0x0000, 0x0001, 0x0500}, {});
	f.basic(kContactShort, 1, "SONOFF", "DS01");
	CHECK(f.driver.calls.back() == "read 0x5e6f 1 0x0500 1");
	f.report(kContactShort, 1, cluster::kIasZone, attribute::kZoneType, zcl_type::kEnum16, 0x0015);
	CHECK(f.driver.has("enroll 0x5e6f 1"));
	REQUIRE(f.dp(kContactPoint) != nullptr);
	CHECK(f.adapter.devices().find(kContact)->endpoints[0].zone_type == 0x0015);
	CHECK_FALSE(f.driver.has("bind 0x5e6f 1 0x0500"));
	uint8_t open[] = {0x01, 0x00, 0x00, 0x01, 0x00, 0x00};
	f.adapter.command_received(ZigbeeFixture::by_short(kContactShort), 1, cluster::kIasZone, 0x00, open, sizeof(open));
	CHECK(f.dp(kContactPoint)->value() == Value::of_bool(true));
	uint8_t closed[] = {0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
	f.adapter.command_received(ZigbeeFixture::by_short(kContactShort), 1, cluster::kIasZone, 0x00, closed,
		sizeof(closed));
	CHECK(f.dp(kContactPoint)->value() == Value::of_bool(false));
	// The ZoneStatus attribute reports the same.
	f.report(kContactShort, 1, cluster::kIasZone, attribute::kZoneStatus, zcl_type::kBitmap16, 0x0001);
	CHECK(f.dp(kContactPoint)->value() == Value::of_bool(true));
}

TEST_CASE("ZigbeeAdapter: a button's commands are action events") {
	struct Recorder : hc::DatapointListener {
		void datapoint_updated(const hc::DatapointUpdate& update) override {
			values.push_back(update.datapoint.value().as_enum());
		}
		std::vector<uint32_t> values;
	} recorder;
	ZigbeeFixture f;
	f.start_network();
	f.join_button();
	REQUIRE(f.dp(kButtonAction) != nullptr);
	CHECK(f.driver.has("bind 0x7a8b 1 0x0006"));
	CHECK(f.driver.has("bind 0x7a8b 1 0x0008"));
	CHECK(f.adapter.stale_timeout_s(kButton) == 0);
	REQUIRE(f.model.add_listener(recorder) == DatapointStatus::ok);
	hc::zigbee::NodeAddress button = ZigbeeFixture::by_short(kButtonShort);
	f.adapter.command_received(button, 1, cluster::kOnOff, 0x01, nullptr, 0);
	f.adapter.command_received(button, 1, cluster::kOnOff, 0x01, nullptr, 0); // a repeat is an event too
	f.adapter.command_received(button, 1, cluster::kOnOff, 0x00, nullptr, 0);
	f.adapter.command_received(button, 1, cluster::kOnOff, 0x02, nullptr, 0);
	uint8_t up[] = {0x00, 0x53};
	uint8_t down[] = {0x01, 0x53};
	f.adapter.command_received(button, 1, cluster::kLevelControl, 0x05, up, 2);
	f.adapter.command_received(button, 1, cluster::kLevelControl, 0x07, nullptr, 0);
	f.adapter.command_received(button, 1, cluster::kLevelControl, 0x02, down, 2);
	f.adapter.command_received(button, 1, cluster::kOnOff, 0x99, nullptr, 0); // unknown: ignored
	CHECK(recorder.values == std::vector<uint32_t>{1, 1, 0, 2, 3, 5, 4});
	f.model.remove_listener(recorder);
}

TEST_CASE("ZigbeeAdapter: a silent device becomes stale and recovers when heard") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	f.join_button();
	f.report(kBulbShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 1);
	CHECK(f.adapter.stale_timeout_s(kBulb) == 900);
	f.run_for(890 * 1000);
	CHECK(f.dp(kBulbOn)->quality() == Quality::good);
	f.run_for(20 * 1000);
	CHECK(f.dp(kBulbOn)->quality() == Quality::stale);
	CHECK(f.dp(kBulbOn)->value() == Value::of_bool(true));
	CHECK(f.adapter.devices().find(kBulb)->stale);
	CHECK(capture.sink.contains("00124b0001020304 (TRADFRI bulb E27 WS 806lm) silent for 900 s"));
	// The button configures no reporting: never stale.
	CHECK_FALSE(f.adapter.devices().find(kButton)->stale);

	// Any frame ends it; the datapoints are read again.
	f.driver.calls.clear();
	f.report(kBulbShort, 11, cluster::kLevelControl, attribute::kCurrentLevel, zcl_type::kUint8, 254);
	CHECK_FALSE(f.adapter.devices().find(kBulb)->stale);
	CHECK(f.dp(kBulbLevel)->quality() == Quality::good);
	CHECK(f.dp(kBulbOn)->quality() == Quality::stale); // until read
	CHECK(capture.sink.contains("heard again: reading its datapoints"));
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.has("read 0x1a2b 11 0x0006 0"));
	f.report(kBulbShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 1);
	CHECK(f.dp(kBulbOn)->quality() == Quality::good);
}

TEST_CASE("ZigbeeAdapter: the database survives a restart and the devices are read") {
	hc_test::FakeBlobStore store;
	{
		ZigbeeFixture f;
		f.start_network();
		f.join_bulb();
		f.join_sensor();
		REQUIRE(f.adapter.set_label(kBulb, "Kitchen"));
		store = f.store;
	}
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.store = store;
	REQUIRE(f.adapter.start());
	CHECK(capture.sink.contains("2 device(s) in the database"));
	CHECK(f.model.size() == 6);
	REQUIRE(f.dp(kBulbOn) != nullptr);
	CHECK(f.dp(kBulbOn)->quality() == Quality::unknown);
	CHECK(std::string(f.adapter.devices().find(kBulb)->label) == "Kitchen");
	// The reads, one device per spacing interval.
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.count("read 0x1a2b") == 3);
	CHECK(f.driver.count("read 0x3c4d") == 0);
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.count("read 0x3c4d") == 3);
	// No interview: the devices are known.
	CHECK(f.driver.count("active") == 0);
	f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 1990);
	CHECK(f.dp(kSensorTemperature)->value() == Value::of_real(19.9));
	// A device not heard since the start goes stale after its timeout counted from the start.
	f.run_for(900 * 1000);
	CHECK(f.adapter.devices().find(kBulb)->stale);
}

TEST_CASE("ZigbeeAdapter: an unreadable database starts empty and devices are learnt again") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	std::string garbage = "{\"format\":7}";
	f.store.blobs["zigbee-devices.json"] = std::vector<uint8_t>(garbage.begin(), garbage.end());
	CHECK(f.adapter.start());
	CHECK(capture.sink.contains("device database zigbee-devices.json unreadable (the database format is not 1)"));
	CHECK(f.adapter.devices().size() == 0);
	// A frame of an unknown device with an IEEE address adds it and interviews it.
	hc::zigbee::NodeAddress address = ZigbeeFixture::node(kBulb, kBulbShort);
	f.adapter.command_received(address, 11, cluster::kOnOff, 0x01, nullptr, 0);
	CHECK(f.adapter.interviewing() == kBulb);
}

TEST_CASE("ZigbeeAdapter: interview timeouts, retries and a later retry") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	f.adapter.device_announced(ZigbeeFixture::node(kSensor, kSensorShort));
	CHECK(f.driver.count("active 0x3c4d") == 1);
	f.run_for(ZigbeeAdapter::kInterviewTimeoutMs);
	CHECK(f.driver.count("active 0x3c4d") == 2);
	f.run_for(ZigbeeAdapter::kInterviewTimeoutMs);
	CHECK(f.driver.count("active 0x3c4d") == 3);
	f.run_for(ZigbeeAdapter::kInterviewTimeoutMs);
	CHECK(f.adapter.interviewing() == 0);
	CHECK(capture.sink.contains("interview of 00158d0005060708 failed at step active endpoints: no answer to the "
		"active endpoint request"));
	CHECK_FALSE(f.adapter.devices().find(kSensor)->interviewed);
	// Heard again within the retry delay: no new interview.
	f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 2000);
	CHECK(f.adapter.interviewing() == 0);
	// After it: interviewed again.
	f.run_for(ZigbeeAdapter::kInterviewRetryMs);
	f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 2000);
	CHECK(f.adapter.interviewing() == kSensor);
	uint8_t endpoints[] = {1};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSensorShort), 0, endpoints, 1);
	f.descriptor(kSensorShort, 1, hc::zigbee::kHomeAutomationProfile, 0x0302, {0x0000, 0x0402}, {});
	// The Basic cluster never answers: after three attempts the interview ends incomplete, the
	// endpoint's functions are in use (issue #47).
	f.run_for(3 * ZigbeeAdapter::kInterviewTimeoutMs);
	CHECK(f.driver.count("read 0x3c4d 1 0x0000 4,5") == 3);
	CHECK(capture.sink.contains("no manufacturer and model from the Basic cluster: the interview stays incomplete"));
	const hc::zigbee::ZigbeeDevice* device = f.adapter.devices().find(kSensor);
	CHECK_FALSE(device->interviewed);
	CHECK(device->incomplete());
	CHECK(device->missing_names);
	CHECK(std::string(device->model).empty());
	CHECK(f.dp(kSensorTemperature) != nullptr);
}

TEST_CASE("ZigbeeAdapter: interviews run one at a time") {
	ZigbeeFixture f;
	f.start_network();
	f.adapter.device_announced(ZigbeeFixture::node(kBulb, kBulbShort));
	f.adapter.device_announced(ZigbeeFixture::node(kSensor, kSensorShort));
	CHECK(f.adapter.interviewing() == kBulb);
	CHECK(f.driver.count("active 0x3c4d") == 0);
	uint8_t endpoints[] = {11};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kBulbShort), 0, endpoints, 1);
	f.descriptor(kBulbShort, 11, hc::zigbee::kHomeAutomationProfile, 0x0100, {0x0000, 0x0006}, {});
	f.basic(kBulbShort, 11, "A", "B");
	CHECK(f.adapter.interviewing() == kSensor);
	CHECK(f.driver.count("active 0x3c4d") == 1);
	// Answers of another device do not advance the interview.
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kBulbShort), 0, endpoints, 1);
	CHECK(f.driver.count("simple 0x3c4d") == 0);
}

TEST_CASE("ZigbeeAdapter: a device without a Home Automation endpoint fails the interview") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	f.adapter.device_announced(ZigbeeFixture::node(kBulb, kBulbShort));
	uint8_t endpoints[] = {242};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kBulbShort), 0, endpoints, 1);
	f.descriptor(kBulbShort, 242, 0xA1E0, 0x0061, {}, {0x0021});
	CHECK(capture.sink.contains("no endpoint of the Home Automation or Light Link profile"));
	CHECK(f.adapter.interviewing() == 0);
	uint8_t none[1] = {0};
	f.adapter.device_announced(ZigbeeFixture::node(kSensor, kSensorShort));
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSensorShort), 0, none, 0);
	CHECK(capture.sink.contains("the device has no active endpoint"));
}

TEST_CASE("ZigbeeAdapter: values survive the reload when another device joins") {
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	f.report(kBulbShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 1);
	f.report(kBulbShort, 11, cluster::kLevelControl, attribute::kCurrentLevel, zcl_type::kUint8, 127);
	f.run_for(900 * 1000 + ZigbeeAdapter::kCheckPeriodMs);
	f.report(kBulbShort, 11, cluster::kLevelControl, attribute::kCurrentLevel, zcl_type::kUint8, 127);
	// On is stale (not read back yet), level good.
	REQUIRE(f.dp(kBulbOn)->quality() == Quality::stale);
	f.join_sensor();
	CHECK(f.model.size() == 6);
	CHECK(f.dp(kBulbOn)->value() == Value::of_bool(true));
	CHECK(f.dp(kBulbOn)->quality() == Quality::stale);
	CHECK(f.dp(kBulbLevel)->value() == Value::of_real(50.0));
	CHECK(f.dp(kBulbLevel)->quality() == Quality::good);
	CHECK(f.dp(kSensorTemperature)->quality() == Quality::unknown);
}

TEST_CASE("ZigbeeAdapter: the runtime image's bindings replace the generic rules of a device") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	f.join_sensor();
	ZigbeeOverride bindings[2];
	bindings[0].name = "Kitchen light";
	bindings[0].ieee = kBulb;
	bindings[0].endpoint = 11;
	bindings[0].cluster = cluster::kOnOff;
	bindings[0].attribute = attribute::kOnOff;
	bindings[0].command_cluster = cluster::kLevelControl;
	bindings[1].name = "Hall button";
	bindings[1].ieee = kButton;
	bindings[1].endpoint = 1;
	bindings[1].cluster = cluster::kOnOff;
	bindings[1].client = true;
	REQUIRE(f.adapter.configure(bindings, 2));
	// The bulb has only the bound datapoint; the sensor keeps its generic ones; the button's binding
	// exists before the button joins.
	CHECK(f.model.size() == 5);
	CHECK(f.dp(kBulbOn) == nullptr);
	CHECK(f.dp(kBulbLevel) == nullptr);
	REQUIRE(f.dp("Kitchen light") != nullptr);
	REQUIRE(f.dp("Hall button") != nullptr);
	CHECK(f.dp(kSensorTemperature) != nullptr);
	CHECK(f.model.command(f.dp("Kitchen light")->id(), Value::of_bool(true)) == DatapointStatus::ok);
	CHECK(f.driver.calls.back() == "level 0x1a2b 11 254 5");
	f.report(kBulbShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 0);
	CHECK(f.dp("Kitchen light")->value() == Value::of_bool(false));
	// A command to a bound device that has not joined is refused.
	f.join_button();
	f.adapter.command_received(ZigbeeFixture::by_short(kButtonShort), 1, cluster::kOnOff, 0x02, nullptr, 0);
	CHECK(f.dp("Hall button")->value() == Value::of_enum(2));

	// Refusals keep the bindings.
	ZigbeeOverride twice[2] = {bindings[0], bindings[0]};
	const char* error = nullptr;
	CHECK_FALSE(f.adapter.configure(twice, 2, &error));
	CHECK(std::string(error) == "a datapoint name is bound twice");
	twice[1].name = "Other";
	CHECK_FALSE(f.adapter.configure(twice, 2, &error));
	CHECK(std::string(error) == "a function of an endpoint is bound twice");
	ZigbeeOverride unknown = bindings[0];
	unknown.cluster = 0x0702;
	CHECK_FALSE(f.adapter.configure(&unknown, 1, &error));
	CHECK(capture.sink.contains("bindings refused: binding 0 ('Kitchen light'): no known function"));
	// A name that clashes with a generic name: the model refuses, the previous bindings stay.
	ZigbeeOverride clash = bindings[0];
	clash.ieee = kButton;
	clash.name = kSensorTemperature;
	CHECK_FALSE(f.adapter.configure(&clash, 1, &error));
	CHECK(f.dp("Kitchen light") != nullptr);
	CHECK(f.model.size() == 5);
	// Removing the bindings brings back the generic rules.
	REQUIRE(f.adapter.configure(nullptr, 0));
	CHECK(f.dp(kBulbOn) != nullptr);
	CHECK(f.dp(kButtonAction) != nullptr);
}

TEST_CASE("ZigbeeAdapter: the model holding other datapoints refuses the Zigbee datapoints") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	DatapointSpec spec{"knx.light", ValueType::boolean, 0};
	REQUIRE(f.model.load(&spec, 1) == DatapointStatus::ok);
	const char* error = nullptr;
	CHECK_FALSE(f.adapter.start(&error));
	CHECK(std::string(error) == "the model holds datapoints of another owner");
	CHECK(capture.sink.contains("the model holds 'knx.light', which is not Zigbee's"));
	CHECK_FALSE(f.adapter.loaded());
	CHECK(f.model.size() == 1);
	// The adapter keeps working: the interview runs, the datapoints wait.
	f.adapter.network_state(true, 1, 15, 2);
	f.join_bulb();
	CHECK(f.adapter.devices().find(kBulb)->interviewed);
	CHECK(f.model.find("knx.light") != nullptr);
	CHECK_FALSE(f.adapter.start(&error));
	CHECK(std::string(error) == "the Zigbee adapter is started already");
}

TEST_CASE("ZigbeeAdapter: operations and their refusals") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	REQUIRE(f.adapter.start());
	const char* error = nullptr;
	CHECK_FALSE(f.adapter.permit_join(60, &error));
	CHECK(std::string(error) == "the Zigbee network is not up");
	f.adapter.network_state(true, 0x1a62, 20, 0x1122334455667788);
	CHECK(capture.sink.contains("network up: PAN id 0x1a62, channel 20, extended PAN id 1122334455667788"));
	CHECK_FALSE(f.adapter.permit_join(255, &error));
	CHECK(std::string(error) == "permit join takes 0..254 seconds");
	CHECK(f.adapter.permit_join(0));
	CHECK(f.driver.calls.back() == "permit 0");
	f.driver.accept = false;
	CHECK_FALSE(f.adapter.permit_join(30, &error));
	CHECK(std::string(error) == "the Zigbee stack refused the permit join");
	f.driver.accept = true;

	CHECK_FALSE(f.adapter.identify(kBulb, &error));
	CHECK(std::string(error) == "no such device");
	CHECK_FALSE(f.adapter.remove(kBulb, &error));
	CHECK(std::string(error) == "no such device");
	CHECK_FALSE(f.adapter.set_label(kBulb, "x", &error));
	CHECK(std::string(error) == "no such device");
	CHECK_FALSE(f.adapter.interview(kBulb, &error));
	CHECK(std::string(error) == "no such device");

	f.join_bulb();
	CHECK(f.adapter.identify(kBulb));
	CHECK(f.driver.calls.back() == "identify 0x1a2b 11 10");
	CHECK_FALSE(f.adapter.set_label(kBulb, "a label that is longer than 32 bytes", &error));
	CHECK(std::string(error) == "a label has at most 32 bytes");
	CHECK(f.adapter.set_label(kBulb, "Kitchen"));
	CHECK(std::string(f.adapter.devices().find(kBulb)->label) == "Kitchen");
	f.store.fail_writes = true;
	CHECK_FALSE(f.adapter.set_label(kBulb, "Hall", &error));
	CHECK(std::string(error) == "the device database could not be written");
	CHECK(std::string(f.adapter.devices().find(kBulb)->label) == "Kitchen");
	f.store.fail_writes = false;

	// A manual interview asks again.
	size_t actives = f.driver.count("active 0x1a2b");
	CHECK(f.adapter.interview(kBulb));
	CHECK(f.driver.count("active 0x1a2b") == actives + 1);
	CHECK(f.adapter.interviewing() == kBulb);

	// Remove: the device is asked to leave and forgotten with its datapoints.
	CHECK(f.adapter.remove(kBulb));
	CHECK(f.driver.calls.back() == "leave 0x1a2b");
	CHECK(f.adapter.interviewing() == 0);
	CHECK(f.adapter.devices().find(kBulb) == nullptr);
	CHECK(f.model.size() == 0);
	CHECK(capture.sink.contains("00124b0001020304 (Kitchen) removed"));
}

TEST_CASE("ZigbeeAdapter: a device that left is stale and interviewed again when it rejoins") {
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	f.report(kBulbShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 1);
	f.adapter.device_left(ZigbeeFixture::node(kBulb, kBulbShort));
	CHECK(f.dp(kBulbOn)->quality() == Quality::stale);
	CHECK(f.adapter.devices().find(kBulb) != nullptr);
	// It rejoins with a new short address: interviewed again, the datapoints stay meanwhile.
	f.adapter.device_announced(ZigbeeFixture::node(kBulb, 0x4444));
	CHECK(f.adapter.interviewing() == kBulb);
	CHECK(f.driver.calls.back() == "active 0x4444");
	CHECK(f.dp(kBulbOn) != nullptr);
	CHECK(f.adapter.devices().find(kBulb)->short_addr == 0x4444);
	CHECK(f.model.command(f.dp(kBulbOn)->id(), Value::of_bool(false)) == DatapointStatus::ok);
	CHECK(f.driver.calls.back() == "onoff 0x4444 11 off");
}

TEST_CASE("ZigbeeAdapter: a known device that joins afresh was reset and is interviewed again") {
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	size_t calls = f.driver.calls.size();
	// A secured rejoin (a power cycle) announces only: nothing to do, the reports continue.
	f.adapter.device_announced(ZigbeeFixture::node(kBulb, kBulbShort));
	CHECK(f.adapter.interviewing() == 0);
	CHECK(f.driver.calls.size() == calls);
	f.run_for(10 * 1000);
	// The trust center reports an unsecured join: the bindings and the reporting are gone with the
	// factory reset; the interview waits for the announce, which proves the device holds the key.
	f.adapter.device_joined(ZigbeeFixture::node(kBulb, kBulbShort));
	CHECK(f.adapter.interviewing() == 0);
	f.adapter.device_announced(ZigbeeFixture::node(kBulb, kBulbShort));
	CHECK(f.adapter.interviewing() == kBulb);
	CHECK(f.driver.calls.back() == "active 0x1a2b");
	// The other order (the announce arrived first, within seconds) interviews right away.
	ZigbeeFixture g;
	g.start_network();
	g.join_bulb();
	g.adapter.device_announced(ZigbeeFixture::node(kBulb, kBulbShort));
	g.adapter.device_joined(ZigbeeFixture::node(kBulb, kBulbShort));
	CHECK(g.adapter.interviewing() == kBulb);
	CHECK(g.driver.calls.back() == "active 0x1a2b");
	// An unknown device's join notice alone adds nothing: its announce does.
	g.adapter.device_joined(ZigbeeFixture::node(0x00124b00aaaaaaaaULL, 0x5555));
	CHECK(g.adapter.devices().find(0x00124b00aaaaaaaaULL) == nullptr);
}

TEST_CASE("ZigbeeAdapter: the JSON view for the console and E-Home") {
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	f.adapter.permit_join_status(120);
	f.run_for(20 * 1000);
	char buffer[2048];
	hc::JsonWriter json(buffer, sizeof(buffer));
	f.adapter.write_json(json);
	REQUIRE_FALSE(json.overflowed());
	std::string text(json.data(), json.length());
	CHECK(text.find("\"network\":{\"up\":true,\"pan_id\":6754,\"channel\":15,\"extended_pan_id\":\"dddddddddddddddd\","
		"\"permit_join_s\":100}") != std::string::npos);
	CHECK(text.find("\"ieee\":\"00124b0001020304\",\"short\":6699,\"manufacturer\":\"IKEA of Sweden\"") !=
		std::string::npos);
	CHECK(text.find("\"datapoints\":[\"zigbee.00124b0001020304.11.on\",\"zigbee.00124b0001020304.11.level\","
		"\"zigbee.00124b0001020304.11.color_temperature\"]") != std::string::npos);
}

TEST_CASE("ZigbeeAdapter: without a blob store the database lives in memory") {
	hc_test::ScopedCapture capture;
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc::DatapointModel model{clock};
	hc_test::FakeZigbeeDriver driver;
	ZigbeeAdapter adapter(loop, clock, model, driver, nullptr);
	REQUIRE(adapter.start());
	CHECK(capture.sink.contains("no blob store: the device database is kept in memory only"));
}

namespace {

/// Stands in for the image service: counts the notifications.
struct SpecListener : hc::image::DynamicSpecListener {
	void dynamic_specs_changed(hc::image::DynamicSpecProvider&) override { ++calls; }
	int calls = 0;
};

/// The image service's part: the image's specs, then the provider's, the provider's bound to their
/// technology, then model_loaded().
void compose(ZigbeeFixture& f, const std::vector<DatapointSpec>& image, const hc::image::ImageBindings& bindings) {
	const hc::image::DynamicSpec* specs = nullptr;
	size_t count = 0;
	const char* error = nullptr;
	REQUIRE(f.adapter.dynamic_specs(bindings, specs, count, &error));
	std::vector<DatapointSpec> all = image;
	for (size_t i = 0; i < count; ++i) {
		all.push_back(specs[i].spec);
	}
	REQUIRE(f.model.load(all.data(), all.size(), true) == DatapointStatus::ok);
	for (size_t i = 0; i < count; ++i) {
		REQUIRE(specs[i].technology == &f.adapter);
		REQUIRE(f.model.bind(static_cast<uint16_t>(image.size() + i), *specs[i].technology) == DatapointStatus::ok);
	}
	f.adapter.model_loaded(bindings);
}

} // namespace

TEST_CASE("ZigbeeAdapter: as a provider of the image service it states its datapoints and binds them by name") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	SpecListener listener;
	f.adapter.set_spec_listener(&listener);
	f.start_network();
	CHECK(listener.calls == 1); // nothing loaded yet
	CHECK(f.model.size() == 0); // the service loads the model, not the adapter
	f.join_sensor();
	CHECK(listener.calls == 2);
	CHECK(f.model.size() == 0);
	// Reporting is configured from the device's mapping, before its datapoints are in the model.
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 30..600 10"));
	CHECK(capture.sink.contains("1 endpoint(s), 3 datapoint(s)"));

	// The image binds the bulb's On/Off to "living.ceiling"; another technology's datapoint first.
	ZigbeeOverride ceiling;
	ceiling.name = "living.ceiling";
	ceiling.ieee = kBulb;
	ceiling.endpoint = 11;
	ceiling.cluster = cluster::kOnOff;
	ceiling.attribute = attribute::kOnOff;
	ceiling.command_cluster = cluster::kOnOff;
	hc::image::ImageBindings bindings;
	bindings.zigbee = &ceiling;
	bindings.zigbee_count = 1;
	std::vector<DatapointSpec> image = {
		{"knx.light", ValueType::boolean, 0},
		{"living.ceiling", ValueType::boolean, DatapointFlags::kWritable | DatapointFlags::kTransmit},
	};
	const hc::image::DynamicSpec* specs = nullptr;
	size_t count = 0;
	REQUIRE(f.adapter.dynamic_specs(bindings, specs, count, nullptr));
	REQUIRE(count == 3);
	CHECK(std::string(specs[0].spec.name) == kSensorTemperature);
	CHECK(std::string(specs[0].unit) == "\xc2\xb0" "C");
	CHECK(f.adapter.configure(&ceiling, 1) == false); // the service hands the bindings over
	compose(f, image, bindings);
	REQUIRE(f.model.size() == 5);
	CHECK(f.adapter.loaded());
	CHECK(f.dp("knx.light")->technology() == nullptr);
	CHECK(f.dp("living.ceiling")->technology() == &f.adapter);
	CHECK(f.dp(kSensorTemperature)->technology() == &f.adapter);
	CHECK(f.dp(kSensorTemperature)->id() == 2);
	f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 2150);
	CHECK(f.dp(kSensorTemperature)->value() == Value::of_real(21.5));

	// The bound bulb joins: its only datapoint is the image's, the generic set does not change.
	f.join_bulb();
	CHECK(listener.calls == 2);
	CHECK(f.dp(kBulbOn) == nullptr);
	f.report(kBulbShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 1);
	CHECK(f.dp("living.ceiling")->value() == Value::of_bool(true));
	CHECK(f.model.command(f.dp("living.ceiling")->id(), Value::of_bool(false)) == DatapointStatus::ok);
	CHECK(f.driver.calls.back() == "onoff 0x1a2b 11 off");
	// A labelled device keeps its datapoints; a removed one changes the set.
	REQUIRE(f.adapter.set_label(kSensor, "Bathroom"));
	CHECK(listener.calls == 2);
	REQUIRE(f.adapter.remove(kSensor));
	CHECK(listener.calls == 3);
	// The model without the adapter's datapoints: reports go nowhere.
	f.adapter.model_cleared();
	CHECK_FALSE(f.adapter.loaded());
	CHECK(f.adapter.point_id(0) == ZigbeeAdapter::kNoId);
	f.report(kBulbShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 0);
	CHECK(f.dp("living.ceiling")->value() == Value::of_bool(false)); // the command's value, unchanged
}

TEST_CASE("ZigbeeAdapter: a read asks the device for the attribute behind the datapoint") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	f.join_button();
	CHECK(f.model.read(f.dp(kBulbOn)->id()) == DatapointStatus::ok);
	CHECK(f.driver.calls.back() == "read 0x1a2b 11 0x0006 0");
	CHECK(f.model.read(f.dp(kBulbLevel)->id()) == DatapointStatus::ok);
	CHECK(f.driver.calls.back() == "read 0x1a2b 11 0x0008 0");
	// A button's action has no attribute.
	size_t calls = f.driver.calls.size();
	CHECK(f.model.read(f.dp(kButtonAction)->id()) == DatapointStatus::refused);
	CHECK(capture.sink.contains("read of 'zigbee.000b57fffe112233.1.action' refused:"));
	CHECK(f.driver.calls.size() == calls);
	// The stack refuses.
	f.driver.accept = false;
	CHECK(f.model.read(f.dp(kBulbOn)->id()) == DatapointStatus::refused);
	CHECK(capture.sink.contains("read of 'zigbee.00124b0001020304.11.on' refused: the Zigbee stack did not take it"));
	f.driver.accept = true;
	// A device that left.
	f.adapter.device_left(ZigbeeFixture::node(kBulb, kBulbShort));
	calls = f.driver.calls.size();
	CHECK(f.model.read(f.dp(kBulbOn)->id()) == DatapointStatus::refused);
	CHECK(capture.sink.contains("read of 'zigbee.00124b0001020304.11.on' refused: the device left the network"));
	CHECK(f.driver.calls.size() == calls);
}

// ---- Incomplete interviews (issue #47) --------------------------------------------------------

namespace {

const char* const kSleepyTemperature = "zigbee.00158d0005060708.1.temperature";
const char* const kSleepyOccupancy = "zigbee.00158d0005060708.2.occupancy";
const char* const kSleepyBattery = "zigbee.00158d0005060708.2.battery";

/// Records the names of the adapter's events.
struct EventRecorder : hc::zigbee::ZigbeeEventListener {
	std::vector<std::string> names;
	void zigbee_event(const hc::zigbee::ZigbeeEvent& event) override {
		names.push_back(hc::zigbee::zigbee_event_name(event.kind));
	}
	bool has(const std::string& name) const {
		for (const std::string& n : names) {
			if (n == name) {
				return true;
			}
		}
		return false;
	}
};

/// Endpoint 2 of the sleepy sensor: occupancy and battery.
void describe_sleepy_2(ZigbeeFixture& f) {
	f.descriptor(kSensorShort, 2, hc::zigbee::kHomeAutomationProfile, 0x0107, {0x0000, 0x0001, 0x0406}, {});
}

/// A sleepy sensor with active endpoints [2, 1] that falls asleep before the descriptor of endpoint
/// 2 is read: endpoint 1 (temperature) and the Basic cluster answer later.
void join_sleepy(ZigbeeFixture& f) {
	f.adapter.device_announced(ZigbeeFixture::node(kSensor, kSensorShort));
	uint8_t endpoints[] = {2, 1};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSensorShort), 0, endpoints, 2);
	REQUIRE(f.driver.calls.back() == "simple 0x3c4d 2");
	f.run_for(ZigbeeAdapter::kInterviewAttempts * ZigbeeAdapter::kInterviewTimeoutMs);
	REQUIRE(f.driver.count("simple 0x3c4d 2") == ZigbeeAdapter::kInterviewAttempts);
	REQUIRE(f.driver.calls.back() == "simple 0x3c4d 1");
	f.descriptor(kSensorShort, 1, hc::zigbee::kHomeAutomationProfile, 0x0302, {0x0000, 0x0402}, {});
	f.basic(kSensorShort, 1, "Sleepy Things", "ST-2");
	REQUIRE(f.adapter.interviewing() == 0);
}

std::string view_json(const ZigbeeFixture& f) {
	std::vector<char> buffer(8192);
	hc::JsonWriter json(buffer.data(), buffer.size());
	f.adapter.write_json(json);
	REQUIRE_FALSE(json.overflowed());
	return std::string(json.data(), json.length());
}

std::string export_json(const ZigbeeFixture& f, hc::zigbee::Ieee ieee) {
	std::vector<char> buffer(8192);
	hc::JsonWriter json(buffer.data(), buffer.size());
	const hc::zigbee::DeviceDatabase& devices = f.adapter.devices();
	for (size_t i = 0; i < devices.size(); ++i) {
		if (devices.at(i).ieee == ieee) {
			f.adapter.write_device_export_json(json, i);
		}
	}
	REQUIRE_FALSE(json.overflowed());
	return std::string(json.data(), json.length());
}

} // namespace

TEST_CASE("ZigbeeAdapter: an unanswered endpoint leaves the interview incomplete until a retry completes it") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	EventRecorder events;
	f.adapter.set_event_listener(&events);
	f.start_network();
	join_sleepy(f);
	CHECK(capture.sink.contains("no simple descriptor for endpoint 2: the interview stays incomplete"));
	CHECK(capture.sink.contains("interview of 00158d0005060708 incomplete (endpoint 2 did not answer)"));
	CHECK(events.has("interview_incomplete"));
	CHECK_FALSE(events.has("interviewed"));
	const hc::zigbee::ZigbeeDevice* device = f.adapter.devices().find(kSensor);
	REQUIRE(device != nullptr);
	CHECK_FALSE(device->interviewed);
	CHECK(device->incomplete());
	REQUIRE(device->missing_count == 1);
	CHECK(device->missing[0] == 2);
	CHECK_FALSE(device->missing_names);
	CHECK(std::string(device->model) == "ST-2");
	// The functions of endpoint 1 are datapoints, those of endpoint 2 are unknown yet.
	REQUIRE(f.dp(kSleepyTemperature) != nullptr);
	CHECK(f.dp(kSleepyOccupancy) == nullptr);
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 30..600 10"));
	// The view and the export say so.
	std::string view = view_json(f);
	CHECK(view.find("\"interviewed\":false") != std::string::npos);
	CHECK(view.find("\"missing\":{\"endpoints\":[2],\"names\":false},\"incomplete\":\"endpoint 2 did not answer\"") !=
		std::string::npos);
	std::string exported = export_json(f, kSensor);
	CHECK(exported.find("\"interviewed\":false,\"missing\":{\"endpoints\":[2],\"names\":false},\"endpoints\":[{\"id\":1,") !=
		std::string::npos);
	// The persisted database too.
	std::string blob(f.store.blobs["zigbee-devices.json"].begin(), f.store.blobs["zigbee-devices.json"].end());
	CHECK(blob.find("\"interviewed\":false") != std::string::npos);
	CHECK(blob.find("\"missing\":{\"endpoints\":[2],\"names\":false}") != std::string::npos);

	// A report right after: the value is taken, the retry waits for its delay.
	f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 2150);
	CHECK(f.dp(kSleepyTemperature)->value() == Value::of_real(21.5));
	CHECK(f.adapter.interviewing() == 0);
	// After the delay its next frame starts the retry, which asks only for what is missing.
	f.run_for(ZigbeeAdapter::kInterviewRetryMs);
	size_t actives = f.driver.count("active 0x3c4d");
	size_t basics = f.driver.count("read 0x3c4d 1 0x0000 4,5");
	f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 2150);
	CHECK(f.adapter.interviewing() == kSensor);
	CHECK(f.driver.calls.back() == "simple 0x3c4d 2");
	CHECK(capture.sink.contains("retrying its incomplete interview (endpoint 2 did not answer)"));
	describe_sleepy_2(f);
	CHECK(f.adapter.interviewing() == 0);
	CHECK(f.driver.count("active 0x3c4d") == actives);
	CHECK(f.driver.count("read 0x3c4d 1 0x0000 4,5") == basics); // the names are known
	CHECK(events.names.back() == "interviewed");
	device = f.adapter.devices().find(kSensor);
	CHECK(device->interviewed);
	CHECK_FALSE(device->incomplete());
	CHECK(device->missing_count == 0);
	REQUIRE(device->endpoint_count == 2);
	CHECK(device->endpoint(1) != nullptr);
	CHECK(device->endpoint(2) != nullptr);
	CHECK(std::string(device->manufacturer) == "Sleepy Things");
	// The model is rebuilt with both endpoints' datapoints; endpoint 1's value survived.
	REQUIRE(f.dp(kSleepyOccupancy) != nullptr);
	REQUIRE(f.dp(kSleepyBattery) != nullptr);
	CHECK(f.dp(kSleepyTemperature)->value() == Value::of_real(21.5));
	CHECK(f.dp(kSleepyTemperature)->quality() == Quality::good);
	CHECK(f.driver.has("configure 0x3c4d 2 0x0406 0x0000 0..600 0"));
	f.report(kSensorShort, 2, cluster::kOccupancySensing, 0x0000, zcl_type::kBitmap8, 1);
	CHECK(f.dp(kSleepyOccupancy)->value() == Value::of_bool(true));
	view = view_json(f);
	CHECK(view.find("\"missing\"") == std::string::npos);
	CHECK(view.find("\"incomplete\"") == std::string::npos);
	f.adapter.set_event_listener(nullptr);
}

TEST_CASE("ZigbeeAdapter: an endpoint that never answers is retried at most once per delay") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sleepy(f);
	auto heard = [&f](int16_t centi) {
		f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, centi);
	};
	// Frames before the delay start nothing.
	for (int i = 0; i < 5; ++i) {
		f.run_for(ZigbeeAdapter::kInterviewRetryMs / 6);
		heard(static_cast<int16_t>(2000 + i));
		CHECK(f.adapter.interviewing() == 0);
	}
	CHECK(f.driver.count("simple 0x3c4d 2") == 3);
	for (int round = 1; round <= 3; ++round) {
		f.run_for(ZigbeeAdapter::kInterviewRetryMs);
		heard(2100);
		REQUIRE(f.adapter.interviewing() == kSensor);
		// The retry times out again: incomplete again, the delay counts from its end.
		f.run_for(ZigbeeAdapter::kInterviewAttempts * ZigbeeAdapter::kInterviewTimeoutMs);
		CHECK(f.adapter.interviewing() == 0);
		CHECK(f.driver.count("simple 0x3c4d 2") == static_cast<size_t>(3 + 3 * round));
		heard(2200);
		CHECK(f.adapter.interviewing() == 0);
		f.run_for(ZigbeeAdapter::kInterviewRetryMs - 1000);
		heard(2300);
		CHECK(f.adapter.interviewing() == 0);
		// Endpoint 1 keeps working throughout.
		CHECK(f.dp(kSleepyTemperature)->value() == Value::of_real(23.0));
		CHECK(f.adapter.devices().find(kSensor)->incomplete());
	}
	CHECK(f.driver.count("active 0x3c4d") == 1);
	CHECK(capture.sink.count("retrying its incomplete interview") == 3);
}

TEST_CASE("ZigbeeAdapter: an unanswered Basic cluster leaves the interview incomplete and is read again") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	f.adapter.device_announced(ZigbeeFixture::node(kSensor, kSensorShort));
	uint8_t endpoints[] = {1};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSensorShort), 0, endpoints, 1);
	f.descriptor(kSensorShort, 1, hc::zigbee::kHomeAutomationProfile, 0x0302, {0x0000, 0x0001, 0x0402, 0x0405}, {});
	f.run_for(ZigbeeAdapter::kInterviewAttempts * ZigbeeAdapter::kInterviewTimeoutMs);
	CHECK(f.adapter.interviewing() == 0);
	const hc::zigbee::ZigbeeDevice* device = f.adapter.devices().find(kSensor);
	CHECK(device->incomplete());
	CHECK(device->missing_names);
	CHECK(device->missing_count == 0);
	CHECK(view_json(f).find("\"incomplete\":\"manufacturer and model did not answer\"") != std::string::npos);
	// Its functions work meanwhile.
	REQUIRE(f.dp(kSensorTemperature) != nullptr);
	REQUIRE(f.dp(kSensorHumidity) != nullptr);
	f.report(kSensorShort, 1, cluster::kRelativeHumidity, attribute::kMeasuredValue, zcl_type::kUint16, 4550);
	CHECK(f.dp(kSensorHumidity)->value() == Value::of_real(45.5));
	CHECK(f.adapter.interviewing() == 0);
	// The retry reads only the names.
	f.run_for(ZigbeeAdapter::kInterviewRetryMs);
	size_t simples = f.driver.count("simple 0x3c4d");
	f.report(kSensorShort, 1, cluster::kRelativeHumidity, attribute::kMeasuredValue, zcl_type::kUint16, 4600);
	CHECK(f.adapter.interviewing() == kSensor);
	CHECK(f.driver.calls.back() == "read 0x3c4d 1 0x0000 4,5");
	f.basic(kSensorShort, 1, "LUMI", "lumi.weather");
	CHECK(f.adapter.interviewing() == 0);
	CHECK(f.driver.count("simple 0x3c4d") == simples);
	device = f.adapter.devices().find(kSensor);
	CHECK(device->interviewed);
	CHECK_FALSE(device->missing_names);
	CHECK(std::string(device->model) == "lumi.weather");
	CHECK(f.dp(kSensorHumidity)->value() == Value::of_real(46.0));
}

TEST_CASE("ZigbeeAdapter: an incomplete device stays incomplete across a restart and is retried when heard") {
	hc_test::FakeBlobStore store;
	{
		ZigbeeFixture f;
		f.start_network();
		join_sleepy(f);
		store = f.store;
	}
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.store = store;
	f.start_network();
	const hc::zigbee::ZigbeeDevice* device = f.adapter.devices().find(kSensor);
	REQUIRE(device != nullptr);
	CHECK(device->incomplete());
	REQUIRE(device->missing_count == 1);
	CHECK(device->missing[0] == 2);
	REQUIRE(f.dp(kSleepyTemperature) != nullptr);
	// Its known datapoints are read like an interviewed device's.
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.has("read 0x3c4d 1 0x0402 0"));
	// The first frame after the restart retries at once (no interview since the boot).
	f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 1990);
	CHECK(f.adapter.interviewing() == kSensor);
	CHECK(f.driver.calls.back() == "simple 0x3c4d 2");
	describe_sleepy_2(f);
	CHECK(f.adapter.devices().find(kSensor)->interviewed);
	CHECK(f.dp(kSleepyOccupancy) != nullptr);
	CHECK(f.dp(kSleepyTemperature)->value() == Value::of_real(19.9));
}

TEST_CASE("ZigbeeAdapter: a reset or a manual interview of an incomplete device interviews it fully") {
	ZigbeeFixture f;
	f.start_network();
	join_sleepy(f);
	// A fresh join (factory reset): the full interview with the announce.
	f.adapter.device_joined(ZigbeeFixture::node(kSensor, kSensorShort));
	f.adapter.device_announced(ZigbeeFixture::node(kSensor, kSensorShort));
	CHECK(f.adapter.interviewing() == kSensor);
	CHECK(f.driver.calls.back() == "active 0x3c4d");
	// A full interview that misses endpoint 2 again keeps endpoint 1 and stays incomplete.
	uint8_t endpoints[] = {2, 1};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSensorShort), 0, endpoints, 2);
	f.run_for(ZigbeeAdapter::kInterviewAttempts * ZigbeeAdapter::kInterviewTimeoutMs);
	f.descriptor(kSensorShort, 1, hc::zigbee::kHomeAutomationProfile, 0x0302, {0x0000, 0x0402}, {});
	f.basic(kSensorShort, 1, "Sleepy Things", "ST-2");
	CHECK(f.adapter.devices().find(kSensor)->incomplete());
	// The console's or the UI's "interview again" asks for everything.
	REQUIRE(f.adapter.interview(kSensor));
	CHECK(f.driver.calls.back() == "active 0x3c4d");
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSensorShort), 0, endpoints, 2);
	describe_sleepy_2(f);
	f.descriptor(kSensorShort, 1, hc::zigbee::kHomeAutomationProfile, 0x0302, {0x0000, 0x0402}, {});
	f.basic(kSensorShort, 2, "Sleepy Things", "ST-2");
	const hc::zigbee::ZigbeeDevice* device = f.adapter.devices().find(kSensor);
	CHECK(device->interviewed);
	CHECK(device->endpoint_count == 2);
	// A later full interview of the interviewed device that misses endpoint 2 keeps its earlier
	// descriptor (and its datapoints) but is no longer complete.
	REQUIRE(f.adapter.interview(kSensor));
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSensorShort), 0, endpoints, 2);
	f.run_for(ZigbeeAdapter::kInterviewAttempts * ZigbeeAdapter::kInterviewTimeoutMs);
	f.descriptor(kSensorShort, 1, hc::zigbee::kHomeAutomationProfile, 0x0302, {0x0000, 0x0402}, {});
	f.basic(kSensorShort, 1, "Sleepy Things", "ST-2");
	device = f.adapter.devices().find(kSensor);
	CHECK_FALSE(device->interviewed);
	CHECK(device->incomplete());
	CHECK(device->endpoint_count == 2);
	CHECK(device->endpoint(2) != nullptr);
	CHECK(f.dp(kSleepyOccupancy) != nullptr);
}

// ---- Probe: an incomplete interview composed with a reporting override on the missing endpoint ------

TEST_CASE("ZigbeeAdapter probe: the override of a function on the unanswered endpoint is configured by the retry") {
	ZigbeeFixture f;
	f.start_network();
	hc::zigbee::ZigbeeOverride bindings[2];
	bindings[0].name = "hall.temperature";
	bindings[0].ieee = kSensor;
	bindings[0].endpoint = 1;
	bindings[0].cluster = cluster::kTemperatureMeasurement;
	bindings[0].attribute = attribute::kMeasuredValue;
	bindings[1].name = "hall.presence";
	bindings[1].ieee = kSensor;
	bindings[1].endpoint = 2;
	bindings[1].cluster = cluster::kOccupancySensing;
	bindings[1].attribute = 0x0000;
	bindings[1].reporting.set = hc::zigbee::ReportingOverride::kMax;
	bindings[1].reporting.max_s = 120;
	REQUIRE(f.adapter.configure(bindings, 2));
	join_sleepy(f);
	const hc::zigbee::ZigbeeDevice* device = f.adapter.devices().find(kSensor);
	REQUIRE(device != nullptr);
	CHECK(device->incomplete());
	// Both bound datapoints exist (bindings do not wait for the device); only endpoint 1 is configured.
	REQUIRE(f.dp("hall.temperature") != nullptr);
	REQUIRE(f.dp("hall.presence") != nullptr);
	CHECK(f.dp("hall.presence")->quality() == Quality::unknown);
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 30..600 10"));
	CHECK(f.driver.count("configure 0x3c4d 2") == 0);
	CHECK(f.adapter.stale_timeout_s(kSensor) == 3 * 600);
	std::string exported = export_json(f, kSensor);
	CHECK(exported.find("\"interviewed\":false") != std::string::npos);
	// The unanswered endpoint's function is not in the export yet: nothing is known about it.
	CHECK(exported.find("\"function\":\"occupancy\"") == std::string::npos);

	// The retry completes the interview: the missing endpoint is configured with the image's override.
	f.run_for(ZigbeeAdapter::kInterviewRetryMs);
	f.report(kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 2000);
	REQUIRE(f.adapter.interviewing() == kSensor);
	describe_sleepy_2(f);
	CHECK(f.adapter.interviewing() == 0);
	CHECK(f.driver.has("configure 0x3c4d 2 0x0406 0x0000 0..120 0"));
	CHECK(f.driver.count("configure 0x3c4d 2") == 1);
	CHECK(f.adapter.stale_timeout_s(kSensor) == 3 * 600); // the longer default of the temperature rules
	exported = export_json(f, kSensor);
	CHECK(exported.find("\"interviewed\":true") != std::string::npos);
	CHECK(exported.find("\"function\":\"occupancy\",\"endpoint\":2,\"cluster\":1030,\"attribute\":0,\"type\":\"boolean\","
		"\"writable\":false,\"reporting\":{\"min\":0,\"max\":120},\"datapoint\":\"hall.presence\"") != std::string::npos);
	f.report(kSensorShort, 2, cluster::kOccupancySensing, 0x0000, zcl_type::kBitmap8, 1);
	CHECK(f.dp("hall.presence")->value() == Value::of_bool(true));
	CHECK(f.dp("hall.temperature")->value() == Value::of_real(20.0));
}
