/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ZIGBEE_CLUSTER_MAPPING_H
#define HC_ZIGBEE_CLUSTER_MAPPING_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "hc/datapoint.h"
#include "hc/zigbee/device_database.h"
#include "hc/zigbee/zcl.h"

namespace hc {
namespace zigbee {

/// What a datapoint of a Zigbee device stands for: one known attribute of a server cluster, or the
/// commands a device's client cluster sends (a button). The table in doc/zigbee.md lists them.
enum class Function : uint8_t {
	on_off,            ///< On/Off 0x0006, OnOff: boolean; commands on/off.
	level,             ///< Level Control 0x0008, CurrentLevel 0..254: real percent; commands move to level.
	color_temperature, ///< Color Control 0x0300, ColorTemperatureMireds: unsigned kelvin; commands move to color temperature.
	temperature,       ///< Temperature Measurement 0x0402: real °C.
	humidity,          ///< Relative Humidity 0x0405: real %.
	illuminance,       ///< Illuminance Measurement 0x0400: real lux.
	occupancy,         ///< Occupancy Sensing 0x0406, bit 0: boolean.
	ias_zone,          ///< IAS Zone 0x0500, ZoneStatus bit 0 (alarm 1): boolean, named after the zone type.
	battery,           ///< Power Configuration 0x0001, BatteryPercentageRemaining: real %.
	multistate,        ///< Multistate Input 0x0012, PresentValue: unsigned, every report an event.
	action,            ///< On/Off and Level Control commands a button sends (client clusters): enumeration.
};

constexpr size_t kFunctionCount = 11;

/// The elements of an `action` datapoint.
namespace action {
constexpr uint32_t kOff = 0;
constexpr uint32_t kOn = 1;
constexpr uint32_t kToggle = 2;
constexpr uint32_t kDimUp = 3;   ///< Level Control move or step up.
constexpr uint32_t kDimDown = 4; ///< Level Control move or step down.
constexpr uint32_t kDimStop = 5; ///< Level Control stop.
} // namespace action

/// The fixed facts of a function.
struct FunctionInfo {
	Function function;
	/// The key in spec/zigbee.md and the console ("on_off", "level", ...).
	const char* key;
	/// The datapoint name's last part (an ias_zone point is named after its zone type instead).
	const char* suffix;
	uint16_t cluster;
	/// The attribute (server functions); action has none (0xFFFF).
	uint16_t attribute;
	/// The attribute's ZCL type, which also defines its "invalid" code.
	uint8_t zcl_type;
	ValueType value_type;
	/// Whether the function lives on the device's server (input) cluster; action is a client cluster.
	bool server;
	/// The cluster whose command sets the value by default (cluster::kNone: read-only).
	uint16_t command_cluster;
	/// Reporting the adapter configures after the interview: minimum and maximum interval in seconds
	/// and the reportable change in the attribute's unit; max_s 0: none configured (events,
	/// notifications).
	uint16_t min_s;
	uint16_t max_s;
	uint32_t change;
};

const FunctionInfo& function_info(Function function);

/// The function of a server attribute; nullptr if the mapping does not know it.
const FunctionInfo* function_of(uint16_t cluster, uint16_t attribute);

/// The function by its key; nullptr if unknown.
const FunctionInfo* function_by_key(const char* key);

/// The name part of an IAS zone point: "contact" (zone type 0x0015), "motion" (0x000D), "smoke"
/// (0x0028), "water" (0x002A), "alarm" otherwise.
const char* zone_suffix(uint16_t zone_type);

/// The datapoint value of an attribute value of `function`; empty for the type's "invalid" code
/// (0xFF of a uint8, 0x8000 of an int16, 0xFFFF of a uint16, mireds 0) and for a string.
///
/// level: percent = level * 100 / 254, one decimal; color temperature: kelvin = 1e6 / mireds,
/// rounded; temperature and humidity: value / 100; illuminance: 10^((value - 1) / 10000) lux, one
/// decimal (0: 0 lux); battery: value / 2; occupancy and IAS zone: bit 0.
std::optional<Value> attribute_value(Function function, const ZclValue& value);

/// Whether `command_cluster` can set a datapoint of `function`: its own command cluster, and
/// additionally Level Control for on_off (move to level with on/off: 254 or 0).
bool can_command(Function function, uint16_t command_cluster);

/// One datapoint the adapter owns: where its value comes from and where its commands go.
struct ZigbeePoint {
	Ieee ieee = 0;
	uint8_t endpoint = 0;
	Function function = Function::on_off;
	/// The command cluster (cluster::kNone: read-only).
	uint16_t command_cluster = cluster::kNone;
	/// The name given by an override; nullptr: the generic name (point_name()). Only the adapter's
	/// copy of the overrides carries it; its loaded points do not (the model has the names).
	const char* name = nullptr;
	/// Zone type of an ias_zone point at mapping time (for the generic name).
	uint16_t zone_type = ZigbeeEndpoint::kNoZoneType;

	ValueType type() const { return function_info(function).value_type; }
	uint16_t cluster() const { return function_info(function).cluster; }
	uint16_t attribute() const { return function_info(function).attribute; }
	bool commandable() const { return command_cluster != cluster::kNone; }
	/// The datapoint flags: commandable points kWritable | kTransmit, server attributes
	/// kReadOnStart (the adapter reads them when it starts and after a device's silence).
	uint8_t flags() const;
};

/// The generic datapoint name "zigbee.<ieee>.<endpoint>.<suffix>" (or the override's name) into
/// `buffer` (at least Datapoint::kMaxName + 1 bytes). Returns `buffer`.
const char* point_name(const ZigbeePoint& point, char* buffer, size_t size);

/// The generic rules: the points of every known server attribute on every endpoint of the device,
/// and an action point per endpoint whose client clusters include On/Off or Level Control. Writes up
/// to `capacity` points to `out` (which may be nullptr) and returns the number of points the device
/// has. A device that is not interviewed has none.
size_t map_device(const ZigbeeDevice& device, ZigbeePoint* out, size_t capacity);

/// A device binding supplied by the runtime image (the `zigbee` binding of a datapoint,
/// spec/zigbee.md). When the image binds a device, only its bindings become datapoints of that
/// device; the generic rules apply to the devices it does not mention.
struct ZigbeeOverride {
	/// The datapoint name (1..Datapoint::kMaxName printable bytes); must outlive configure().
	const char* name = nullptr;
	Ieee ieee = 0;
	uint8_t endpoint = 0;
	uint16_t cluster = 0;
	/// The attribute; ignored for the client clusters of an action binding.
	uint16_t attribute = 0;
	/// Whether the binding is a client cluster's commands (an action point) rather than an attribute.
	bool client = false;
	/// The cluster commands go to; cluster::kNone: read-only.
	uint16_t command_cluster = cluster::kNone;
};

/// The point of an override; empty with `error` when the cluster/attribute is no known function,
/// the endpoint is 0 or above 240, the name is missing, or the command cluster cannot command it.
std::optional<ZigbeePoint> override_point(const ZigbeeOverride& binding, const char** error = nullptr);

} // namespace zigbee
} // namespace hc

#endif // HC_ZIGBEE_CLUSTER_MAPPING_H
