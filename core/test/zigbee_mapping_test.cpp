/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The generic cluster-to-datapoint rules, the value conversions and the image's bindings.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/zigbee/cluster_mapping.h"

using hc::DatapointFlags;
using hc::Value;
using hc::ValueType;
using hc::zigbee::Function;
using hc::zigbee::ZclValue;
using hc::zigbee::ZigbeeDevice;
using hc::zigbee::ZigbeeOverride;
using hc::zigbee::ZigbeePoint;
namespace cluster = hc::zigbee::cluster;
namespace zcl_type = hc::zigbee::zcl_type;

namespace {

ZclValue number(uint8_t type, int64_t n) {
	ZclValue v;
	v.type = type;
	v.number = n;
	return v;
}

std::optional<Value> convert(Function f, int64_t n) {
	return hc::zigbee::attribute_value(f, number(hc::zigbee::function_info(f).zcl_type, n));
}

ZigbeeDevice device(std::vector<uint16_t> in, std::vector<uint16_t> out, uint16_t zone_type = 0xFFFF) {
	ZigbeeDevice d;
	d.ieee = 0x00124b0001020304ull;
	d.short_addr = 0x1234;
	d.interviewed = true;
	d.endpoint_count = 1;
	d.endpoints[0].id = 1;
	d.endpoints[0].profile = hc::zigbee::kHomeAutomationProfile;
	d.endpoints[0].set_clusters(in.data(), in.size(), out.data(), out.size());
	d.endpoints[0].zone_type = zone_type;
	return d;
}

std::vector<std::string> names(const ZigbeeDevice& d) {
	ZigbeePoint points[16];
	size_t count = hc::zigbee::map_device(d, points, 16);
	std::vector<std::string> result;
	for (size_t i = 0; i < count; ++i) {
		char name[64];
		result.push_back(hc::zigbee::point_name(points[i], name, sizeof(name)));
	}
	return result;
}

} // namespace

TEST_CASE("ZigbeeMapping: attribute values convert to datapoint values") {
	CHECK(*convert(Function::on_off, 1) == Value::of_bool(true));
	CHECK(*convert(Function::on_off, 0) == Value::of_bool(false));
	CHECK(*convert(Function::level, 254) == Value::of_real(100.0));
	CHECK(*convert(Function::level, 127) == Value::of_real(50.0));
	CHECK(*convert(Function::level, 1) == Value::of_real(0.4));
	CHECK_FALSE(convert(Function::level, 255)); // invalid
	CHECK(*convert(Function::color_temperature, 370) == Value::of_unsigned(2703));
	CHECK(*convert(Function::color_temperature, 153) == Value::of_unsigned(6536));
	CHECK_FALSE(convert(Function::color_temperature, 0));
	CHECK_FALSE(convert(Function::color_temperature, 0xFFFF));
	CHECK(*convert(Function::temperature, 2150) == Value::of_real(21.5));
	CHECK(*convert(Function::temperature, -500) == Value::of_real(-5.0));
	CHECK_FALSE(convert(Function::temperature, -0x8000)); // invalid
	CHECK(*convert(Function::humidity, 4567) == Value::of_real(45.67));
	CHECK_FALSE(convert(Function::humidity, 0xFFFF));
	CHECK(*convert(Function::illuminance, 0) == Value::of_real(0.0));
	CHECK(*convert(Function::illuminance, 1) == Value::of_real(1.0));
	CHECK(*convert(Function::illuminance, 30001) == Value::of_real(1000.0));
	CHECK_FALSE(convert(Function::illuminance, 0xFFFF));
	CHECK(*convert(Function::occupancy, 0x01) == Value::of_bool(true));
	CHECK(*convert(Function::occupancy, 0x02) == Value::of_bool(false));
	CHECK(*convert(Function::ias_zone, 0x0021) == Value::of_bool(true)); // alarm 1 plus battery low
	CHECK(*convert(Function::ias_zone, 0x0020) == Value::of_bool(false));
	CHECK(*convert(Function::battery, 200) == Value::of_real(100.0));
	CHECK(*convert(Function::battery, 87) == Value::of_real(43.5));
	CHECK_FALSE(convert(Function::battery, 0xFF));
	CHECK(*convert(Function::multistate, 2) == Value::of_unsigned(2));
	ZclValue text;
	text.type = zcl_type::kCharString;
	CHECK_FALSE(hc::zigbee::attribute_value(Function::on_off, text));
}

TEST_CASE("ZigbeeMapping: the generic rules per device class") {
	// A colour-temperature bulb.
	CHECK(names(device({0x0000, 0x0003, 0x0006, 0x0008, 0x0300}, {0x0019})) ==
		std::vector<std::string>{"zigbee.00124b0001020304.1.on", "zigbee.00124b0001020304.1.level",
			"zigbee.00124b0001020304.1.color_temperature"});
	// A temperature/humidity sensor with a battery.
	CHECK(names(device({0x0000, 0x0001, 0x0402, 0x0405}, {})) ==
		std::vector<std::string>{"zigbee.00124b0001020304.1.temperature", "zigbee.00124b0001020304.1.humidity",
			"zigbee.00124b0001020304.1.battery"});
	// IAS zones are named after their type.
	CHECK(names(device({0x0500}, {}, 0x0015)) == std::vector<std::string>{"zigbee.00124b0001020304.1.contact"});
	CHECK(names(device({0x0500}, {}, 0x000D)) == std::vector<std::string>{"zigbee.00124b0001020304.1.motion"});
	CHECK(names(device({0x0500}, {}, 0x0028)) == std::vector<std::string>{"zigbee.00124b0001020304.1.smoke"});
	CHECK(names(device({0x0500}, {}, 0x002A)) == std::vector<std::string>{"zigbee.00124b0001020304.1.water"});
	CHECK(names(device({0x0500}, {})) == std::vector<std::string>{"zigbee.00124b0001020304.1.alarm"});
	// Occupancy, illuminance, a multistate button, an On/Off remote.
	CHECK(names(device({0x0406, 0x0400}, {})) ==
		std::vector<std::string>{"zigbee.00124b0001020304.1.illuminance", "zigbee.00124b0001020304.1.occupancy"});
	CHECK(names(device({0x0012}, {})) == std::vector<std::string>{"zigbee.00124b0001020304.1.multistate"});
	CHECK(names(device({0x0000}, {0x0006, 0x0008})) == std::vector<std::string>{"zigbee.00124b0001020304.1.action"});
	// Not interviewed: nothing.
	ZigbeeDevice fresh = device({0x0006}, {});
	fresh.interviewed = false;
	CHECK(hc::zigbee::map_device(fresh, nullptr, 0) == 0);
	// The count is returned even without room.
	CHECK(hc::zigbee::map_device(device({0x0006, 0x0008}, {}), nullptr, 0) == 2);
}

TEST_CASE("ZigbeeMapping: types and flags of the points") {
	ZigbeePoint points[8];
	size_t count = hc::zigbee::map_device(device({0x0006, 0x0402, 0x0012}, {0x0006}), points, 8);
	REQUIRE(count == 4);
	CHECK(points[0].function == Function::on_off);
	CHECK(points[0].type() == ValueType::boolean);
	CHECK(points[0].flags() == (DatapointFlags::kWritable | DatapointFlags::kTransmit | DatapointFlags::kReadOnStart));
	CHECK(points[1].function == Function::temperature);
	CHECK(points[1].type() == ValueType::real);
	CHECK(points[1].flags() == DatapointFlags::kReadOnStart);
	CHECK(points[2].function == Function::multistate);
	CHECK(points[2].flags() == 0);
	CHECK(points[3].function == Function::action);
	CHECK(points[3].type() == ValueType::enumeration);
	CHECK(points[3].flags() == 0);
}

TEST_CASE("ZigbeeMapping: bindings of the runtime image") {
	ZigbeeOverride binding;
	binding.name = "Kitchen light";
	binding.ieee = 0x00124b0001020304ull;
	binding.endpoint = 11;
	binding.cluster = cluster::kOnOff;
	binding.attribute = 0x0000;
	binding.command_cluster = cluster::kOnOff;
	std::optional<ZigbeePoint> point = hc::zigbee::override_point(binding);
	REQUIRE(point);
	CHECK(point->function == Function::on_off);
	CHECK(point->commandable());
	char name[64];
	CHECK(std::string(hc::zigbee::point_name(*point, name, sizeof(name))) == "Kitchen light");

	// An on/off state commanded through Level Control (move to level with on/off).
	binding.command_cluster = cluster::kLevelControl;
	CHECK(hc::zigbee::override_point(binding));
	// Read-only.
	binding.command_cluster = cluster::kNone;
	point = hc::zigbee::override_point(binding);
	REQUIRE(point);
	CHECK_FALSE(point->commandable());
	CHECK(point->flags() == DatapointFlags::kReadOnStart);

	const char* error = nullptr;
	ZigbeeOverride wrong = binding;
	wrong.cluster = cluster::kTemperatureMeasurement;
	wrong.command_cluster = cluster::kOnOff;
	CHECK_FALSE(hc::zigbee::override_point(wrong, &error));
	CHECK(std::string(error) == "the command cluster cannot set this attribute");
	wrong = binding;
	wrong.cluster = 0x0702; // metering: not known
	CHECK_FALSE(hc::zigbee::override_point(wrong, &error));
	CHECK(std::string(error) == "no known function for this cluster and attribute (doc/zigbee.md lists them)");
	wrong = binding;
	wrong.endpoint = 0;
	CHECK_FALSE(hc::zigbee::override_point(wrong, &error));
	CHECK(std::string(error) == "binding endpoint outside 1..240");
	wrong = binding;
	wrong.name = "";
	CHECK_FALSE(hc::zigbee::override_point(wrong, &error));
	CHECK(std::string(error) == "binding without a datapoint name");
	wrong = binding;
	wrong.ieee = 0;
	CHECK_FALSE(hc::zigbee::override_point(wrong, &error));
	CHECK(std::string(error) == "binding with a reserved IEEE address");

	// A button's commands.
	ZigbeeOverride button = binding;
	button.client = true;
	button.cluster = cluster::kLevelControl;
	point = hc::zigbee::override_point(button);
	REQUIRE(point);
	CHECK(point->function == Function::action);
	button.command_cluster = cluster::kOnOff;
	CHECK_FALSE(hc::zigbee::override_point(button, &error));
	CHECK(std::string(error) == "a client binding (button commands) cannot be commanded");
	button.command_cluster = cluster::kNone;
	button.cluster = cluster::kTemperatureMeasurement;
	CHECK_FALSE(hc::zigbee::override_point(button, &error));
	CHECK(std::string(error) == "a client binding (button commands) needs cluster 6 (On/Off) or 8 (Level Control)");
}

TEST_CASE("ZigbeeMapping: functions by key and attribute") {
	CHECK(hc::zigbee::function_by_key("color_temperature")->function == Function::color_temperature);
	CHECK(hc::zigbee::function_by_key("nonsense") == nullptr);
	CHECK(hc::zigbee::function_of(cluster::kIasZone, 0x0002)->function == Function::ias_zone);
	CHECK(hc::zigbee::function_of(cluster::kIasZone, 0x0001) == nullptr);
	for (size_t i = 0; i < hc::zigbee::kFunctionCount; ++i) {
		CHECK(static_cast<size_t>(hc::zigbee::function_info(static_cast<Function>(i)).function) == i);
	}
}
