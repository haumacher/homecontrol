/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/zigbee/cluster_mapping.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace hc {
namespace zigbee {

namespace {

constexpr uint16_t kNoAttribute = 0xFFFF;

// In the order of Function. Reporting: lights report a change at once (min 1 s) and at least every
// 5 min; sensors every 30 s at most and every 10 min at least; the battery every 1 h at most and
// every 6 h at least. A device is stale after three maximum intervals of silence (doc/zigbee.md).
const FunctionInfo kFunctions[kFunctionCount] = {
	{Function::on_off, "on_off", "on", cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, ValueType::boolean, true,
		cluster::kOnOff, 0, 300, 0},
	{Function::level, "level", "level", cluster::kLevelControl, attribute::kCurrentLevel, zcl_type::kUint8,
		ValueType::real, true, cluster::kLevelControl, 1, 300, 1},
	{Function::color_temperature, "color_temperature", "color_temperature", cluster::kColorControl,
		attribute::kColorTemperature, zcl_type::kUint16, ValueType::unsigned_int, true, cluster::kColorControl, 1, 300,
		10},
	{Function::temperature, "temperature", "temperature", cluster::kTemperatureMeasurement, attribute::kMeasuredValue,
		zcl_type::kInt16, ValueType::real, true, cluster::kNone, 30, 600, 10},
	{Function::humidity, "humidity", "humidity", cluster::kRelativeHumidity, attribute::kMeasuredValue,
		zcl_type::kUint16, ValueType::real, true, cluster::kNone, 30, 600, 100},
	{Function::illuminance, "illuminance", "illuminance", cluster::kIlluminanceMeasurement, attribute::kMeasuredValue,
		zcl_type::kUint16, ValueType::real, true, cluster::kNone, 10, 600, 500},
	{Function::occupancy, "occupancy", "occupancy", cluster::kOccupancySensing, attribute::kOccupancy,
		zcl_type::kBitmap8, ValueType::boolean, true, cluster::kNone, 0, 600, 0},
	{Function::ias_zone, "ias_zone", "alarm", cluster::kIasZone, attribute::kZoneStatus, zcl_type::kBitmap16,
		ValueType::boolean, true, cluster::kNone, 0, 0, 0},
	{Function::battery, "battery", "battery", cluster::kPowerConfiguration, attribute::kBatteryPercentage,
		zcl_type::kUint8, ValueType::real, true, cluster::kNone, 3600, 21600, 2},
	{Function::multistate, "multistate", "multistate", cluster::kMultistateInput, attribute::kPresentValue,
		zcl_type::kUint16, ValueType::unsigned_int, true, cluster::kNone, 0, 0, 0},
	{Function::action, "action", "action", cluster::kOnOff, kNoAttribute, 0, ValueType::enumeration, false,
		cluster::kNone, 0, 0, 0},
};

void fail(const char** error, const char* reason) {
	if (error != nullptr) {
		*error = reason;
	}
}

double round1(double value) {
	return std::round(value * 10.0) / 10.0;
}

} // namespace

const FunctionInfo& function_info(Function function) {
	return kFunctions[static_cast<size_t>(function)];
}

const FunctionInfo* function_of(uint16_t cluster, uint16_t attribute) {
	for (const FunctionInfo& info : kFunctions) {
		if (info.server && info.cluster == cluster && info.attribute == attribute) {
			return &info;
		}
	}
	return nullptr;
}

const FunctionInfo* function_by_key(const char* key) {
	if (key == nullptr) {
		return nullptr;
	}
	for (const FunctionInfo& info : kFunctions) {
		if (std::strcmp(info.key, key) == 0) {
			return &info;
		}
	}
	return nullptr;
}

const char* zone_suffix(uint16_t zone_type) {
	switch (zone_type) {
		case 0x0015: return "contact";
		case 0x000D: return "motion";
		case 0x0028: return "smoke";
		case 0x002A: return "water";
		default: return "alarm";
	}
}

std::optional<Value> attribute_value(Function function, const ZclValue& value) {
	if (value.is_string()) {
		return std::nullopt;
	}
	int64_t n = value.number;
	switch (function) {
		case Function::on_off:
			return Value::of_bool(n != 0);
		case Function::level:
			if (n < 0 || n > 254) {
				return std::nullopt; // 0xFF: invalid
			}
			return Value::of_real(round1(static_cast<double>(n) * 100.0 / 254.0));
		case Function::color_temperature:
			if (n <= 0 || n >= 0xFFFF) {
				return std::nullopt;
			}
			return Value::of_unsigned(static_cast<uint32_t>(std::lround(1e6 / static_cast<double>(n))));
		case Function::temperature:
			if (n == -0x8000 || n < -0x8000 || n > 0x7FFF) {
				return std::nullopt;
			}
			return Value::of_real(static_cast<double>(n) / 100.0);
		case Function::humidity:
			if (n < 0 || n >= 0xFFFF) {
				return std::nullopt;
			}
			return Value::of_real(static_cast<double>(n) / 100.0);
		case Function::illuminance:
			if (n < 0 || n >= 0xFFFF) {
				return std::nullopt;
			}
			if (n == 0) {
				return Value::of_real(0.0);
			}
			return Value::of_real(round1(std::pow(10.0, (static_cast<double>(n) - 1.0) / 10000.0)));
		case Function::occupancy:
		case Function::ias_zone:
			return Value::of_bool((n & 0x1) != 0);
		case Function::battery:
			if (n < 0 || n > 200) {
				return std::nullopt; // 0xFF: invalid; above 200 (100 %) is outside the range
			}
			return Value::of_real(static_cast<double>(n) / 2.0);
		case Function::multistate:
			if (n < 0 || n > 0xFFFF) {
				return std::nullopt;
			}
			return Value::of_unsigned(static_cast<uint32_t>(n));
		case Function::action:
			return std::nullopt;
	}
	return std::nullopt;
}

bool configures_reporting(Function function) {
	return function_info(function).max_s > 0;
}

Reporting default_reporting(Function function) {
	const FunctionInfo& info = function_info(function);
	Reporting reporting;
	if (info.max_s > 0) {
		reporting.min_s = info.min_s;
		reporting.max_s = info.max_s;
		reporting.change = static_cast<uint16_t>(info.change);
	}
	return reporting;
}

namespace {

/// How a function's reportable change in the datapoint's unit maps to the attribute: raw =
/// round(change * scale), at most max_raw. scale 0: no change in the unit.
struct ChangeScale {
	double scale;
	uint16_t max_raw;
	const char* refusal;
};

ChangeScale change_scale(Function function) {
	switch (function) {
		case Function::level: return {2.54, 254, "change must be 0..100 (%)"};
		case Function::temperature: return {100.0, 32767, "change must be 0..327.67 (\xc2\xb0" "C)"};
		case Function::humidity: return {100.0, 10000, "change must be 0..100 (%)"};
		case Function::battery: return {2.0, 200, "change must be 0..100 (%)"};
		case Function::on_off:
			return {0, 0, "the function on_off has no reportable change (a discrete value reports every change)"};
		case Function::occupancy:
			return {0, 0, "the function occupancy has no reportable change (a discrete value reports every change)"};
		case Function::illuminance:
			return {0, 0, "the function illuminance has no reportable change in lux (its attribute is logarithmic: a lux "
				"step has no fixed attribute step); min and max are allowed"};
		case Function::color_temperature:
			return {0, 0, "the function color_temperature has no reportable change in K (its attribute counts mireds, "
				"1000000 / K: a kelvin step has no fixed attribute step); min and max are allowed"};
		case Function::ias_zone:
		case Function::multistate:
		case Function::action:
			break;
	}
	return {0, 0, "the function has no reportable change"};
}

/// Why `function` takes no reporting override; nullptr if it does.
const char* no_reporting_reason(Function function) {
	switch (function) {
		case Function::action: return "the function action (a client binding, button commands) configures no reporting";
		case Function::multistate: return "the function multistate configures no reporting (its reports are events)";
		case Function::ias_zone:
			return "the function ias_zone configures no reporting (zone status change notifications)";
		default: return nullptr;
	}
}

} // namespace

bool change_to_raw(Function function, double change, uint16_t& raw, const char** error) {
	ChangeScale scale = change_scale(function);
	if (scale.scale == 0) {
		fail(error, scale.refusal);
		return false;
	}
	double steps = change * scale.scale;
	if (!(change >= 0) || std::round(steps) > static_cast<double>(scale.max_raw)) {
		fail(error, scale.refusal);
		return false;
	}
	raw = static_cast<uint16_t>(std::lround(steps));
	return true;
}

bool change_to_unit(Function function, uint16_t raw, double& change) {
	ChangeScale scale = change_scale(function);
	if (scale.scale == 0) {
		return false;
	}
	change = static_cast<double>(raw) / scale.scale;
	if (function == Function::level) {
		change = round1(change);
	}
	return true;
}

bool resolve_reporting(Function function, const ReportingOverride& override, Reporting& out, ReportingMember* member,
	const char** error) {
	auto refuse = [&](ReportingMember which, const char* reason) {
		if (member != nullptr) {
			*member = which;
		}
		fail(error, reason);
		return false;
	};
	out = default_reporting(function);
	if (override.set == 0) {
		return true;
	}
	if (!configures_reporting(function)) {
		const char* reason = no_reporting_reason(function);
		return refuse(ReportingMember::whole, reason != nullptr ? reason : "the function configures no reporting");
	}
	if ((override.set & ReportingOverride::kMin) != 0) {
		if (override.min_s > Reporting::kMaxMin) {
			return refuse(ReportingMember::min, "min must be 0..65534");
		}
		out.min_s = override.min_s;
	}
	if ((override.set & ReportingOverride::kMax) != 0) {
		out.max_s = override.max_s;
	}
	if (out.periodic() && out.max_s < out.min_s) {
		if ((override.set & ReportingOverride::kMax) != 0) {
			return refuse(ReportingMember::max, "max must not be below min (an absent min is the function's default); "
				"0: no periodic reports, 65535: no reports");
		}
		return refuse(ReportingMember::min, "min must not exceed max (an absent max is the function's default); "
			"max 0: no periodic reports, max 65535: no reports");
	}
	if ((override.set & ReportingOverride::kChange) != 0) {
		const char* why = nullptr;
		if (!change_to_raw(function, override.change, out.change, &why)) {
			return refuse(ReportingMember::change, why);
		}
	}
	return true;
}

bool can_command(Function function, uint16_t command_cluster) {
	if (command_cluster == cluster::kNone) {
		return true;
	}
	const FunctionInfo& info = function_info(function);
	if (info.command_cluster != cluster::kNone && info.command_cluster == command_cluster) {
		return true;
	}
	return function == Function::on_off && command_cluster == cluster::kLevelControl;
}

uint8_t ZigbeePoint::flags() const {
	uint8_t flags = 0;
	if (commandable()) {
		flags |= DatapointFlags::kWritable | DatapointFlags::kTransmit;
	}
	const FunctionInfo& info = function_info(function);
	if (info.server && function != Function::multistate) {
		flags |= DatapointFlags::kReadOnStart;
	}
	return flags;
}

const char* point_name(const ZigbeePoint& point, char* buffer, size_t size) {
	if (size == 0) {
		return buffer;
	}
	if (point.name != nullptr) {
		std::snprintf(buffer, size, "%s", point.name);
		return buffer;
	}
	char ieee[kIeeeText];
	format_ieee(point.ieee, ieee);
	const char* suffix =
		point.function == Function::ias_zone ? zone_suffix(point.zone_type) : function_info(point.function).suffix;
	std::snprintf(buffer, size, "zigbee.%s.%u.%s", ieee, static_cast<unsigned>(point.endpoint), suffix);
	return buffer;
}

size_t map_device(const ZigbeeDevice& device, ZigbeePoint* out, size_t capacity) {
	if (!device.has_result()) {
		return 0;
	}
	size_t count = 0;
	auto emit = [&](const ZigbeeEndpoint& endpoint, Function function) {
		if (out != nullptr && count < capacity) {
			ZigbeePoint& point = out[count];
			point = ZigbeePoint();
			point.ieee = device.ieee;
			point.endpoint = endpoint.id;
			point.function = function;
			point.command_cluster = function_info(function).command_cluster;
			point.zone_type = endpoint.zone_type;
			point.reporting = default_reporting(function);
		}
		++count;
	};
	for (size_t e = 0; e < device.endpoint_count; ++e) {
		const ZigbeeEndpoint& endpoint = device.endpoints[e];
		for (const FunctionInfo& info : kFunctions) {
			if (info.server ? endpoint.has_in(info.cluster) : false) {
				emit(endpoint, info.function);
			}
		}
		if (endpoint.has_out(cluster::kOnOff) || endpoint.has_out(cluster::kLevelControl)) {
			emit(endpoint, Function::action);
		}
	}
	return count;
}

std::optional<ZigbeePoint> override_point(const ZigbeeOverride& binding, const char** error) {
	if (binding.name == nullptr || binding.name[0] == '\0') {
		fail(error, "binding without a datapoint name");
		return std::nullopt;
	}
	if (binding.ieee == 0 || binding.ieee == ~static_cast<Ieee>(0)) {
		fail(error, "binding with a reserved IEEE address");
		return std::nullopt;
	}
	if (binding.endpoint == 0 || binding.endpoint > 240) {
		fail(error, "binding endpoint outside 1..240");
		return std::nullopt;
	}
	ZigbeePoint point;
	point.ieee = binding.ieee;
	point.endpoint = binding.endpoint;
	point.name = binding.name;
	if (binding.client) {
		if (binding.cluster != cluster::kOnOff && binding.cluster != cluster::kLevelControl) {
			fail(error, "a client binding (button commands) needs cluster 6 (On/Off) or 8 (Level Control)");
			return std::nullopt;
		}
		if (binding.command_cluster != cluster::kNone) {
			fail(error, "a client binding (button commands) cannot be commanded");
			return std::nullopt;
		}
		if (binding.reporting.set != 0) {
			fail(error, no_reporting_reason(Function::action));
			return std::nullopt;
		}
		point.function = Function::action;
		return point;
	}
	const FunctionInfo* info = function_of(binding.cluster, binding.attribute);
	if (info == nullptr) {
		fail(error, "no known function for this cluster and attribute (doc/zigbee.md lists them)");
		return std::nullopt;
	}
	if (!can_command(info->function, binding.command_cluster)) {
		fail(error, "the command cluster cannot set this attribute");
		return std::nullopt;
	}
	point.function = info->function;
	point.command_cluster = binding.command_cluster;
	if (!resolve_reporting(info->function, binding.reporting, point.reporting, nullptr, error)) {
		return std::nullopt;
	}
	return point;
}

} // namespace zigbee
} // namespace hc
