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
	// The Basic cluster never answers: interviewed without names after three attempts.
	f.run_for(3 * ZigbeeAdapter::kInterviewTimeoutMs);
	CHECK(f.driver.count("read 0x3c4d 1 0x0000 4,5") == 3);
	CHECK(capture.sink.contains("no manufacturer and model from the Basic cluster"));
	const hc::zigbee::ZigbeeDevice* device = f.adapter.devices().find(kSensor);
	CHECK(device->interviewed);
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
