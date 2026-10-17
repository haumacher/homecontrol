/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ZIGBEE_ZCL_H
#define HC_ZIGBEE_ZCL_H

#include <cstddef>
#include <cstdint>

namespace hc {
namespace zigbee {

/// A device's IEEE (EUI-64) address: the identity that survives a rejoin (the 16-bit network address
/// may change). Written as 16 lowercase hex digits, most significant octet first
/// ("00124b001f2a3b4c"), the form `zigbee devices` prints and spec/zigbee.md uses.
using Ieee = uint64_t;

/// Characters of the text form including the NUL.
constexpr size_t kIeeeText = 17;

/// "No short address known" (the broadcast address is never a device's).
constexpr uint16_t kNoShortAddress = 0xFFFF;

/// Writes the 16 hex digits of `ieee` into `out`.
void format_ieee(Ieee ieee, char out[kIeeeText]);

/// Reads 16 hex digits, optionally as 8 colon-separated octets ("00:12:4b:..."). Refuses other
/// lengths, other characters and the reserved addresses 0 and ffffffffffffffff.
bool parse_ieee(const char* text, Ieee& out);

/// The address of a node as the platform knows it from a frame: the IEEE address when the frame or
/// the stack's address table has it (0 otherwise), the short address when the frame has it.
struct NodeAddress {
	Ieee ieee = 0;
	uint16_t short_addr = kNoShortAddress;
};

/// Application profiles whose endpoints the interview keeps.
constexpr uint16_t kHomeAutomationProfile = 0x0104;
constexpr uint16_t kLightLinkProfile = 0xC05E;

/// The cluster ids the mapping knows (ZCL specification, chapter numbers in doc/zigbee.md).
namespace cluster {
constexpr uint16_t kBasic = 0x0000;
constexpr uint16_t kPowerConfiguration = 0x0001;
constexpr uint16_t kIdentify = 0x0003;
constexpr uint16_t kOnOff = 0x0006;
constexpr uint16_t kLevelControl = 0x0008;
constexpr uint16_t kMultistateInput = 0x0012;
constexpr uint16_t kColorControl = 0x0300;
constexpr uint16_t kIlluminanceMeasurement = 0x0400;
constexpr uint16_t kTemperatureMeasurement = 0x0402;
constexpr uint16_t kRelativeHumidity = 0x0405;
constexpr uint16_t kOccupancySensing = 0x0406;
constexpr uint16_t kIasZone = 0x0500;
/// "No cluster" (a reserved id: manufacturer-specific clusters start at 0xFC00, 0xFFFF is unused).
constexpr uint16_t kNone = 0xFFFF;
} // namespace cluster

/// The attribute ids the mapping and the interview use.
namespace attribute {
constexpr uint16_t kManufacturerName = 0x0004; ///< Basic
constexpr uint16_t kModelIdentifier = 0x0005;  ///< Basic
constexpr uint16_t kBatteryPercentage = 0x0021; ///< Power Configuration, half percent
constexpr uint16_t kOnOff = 0x0000;            ///< On/Off
constexpr uint16_t kCurrentLevel = 0x0000;     ///< Level Control, 0..254
constexpr uint16_t kPresentValue = 0x0055;     ///< Multistate Input
constexpr uint16_t kColorTemperature = 0x0007; ///< Color Control, mireds
constexpr uint16_t kMeasuredValue = 0x0000;    ///< the measurement clusters
constexpr uint16_t kOccupancy = 0x0000;        ///< Occupancy Sensing, bitmap8
constexpr uint16_t kZoneType = 0x0001;         ///< IAS Zone, enum16
constexpr uint16_t kZoneStatus = 0x0002;       ///< IAS Zone, bitmap16
} // namespace attribute

/// ZCL data type ids (ZCL specification table 2-10) the decoder supports.
namespace zcl_type {
constexpr uint8_t kBool = 0x10;
constexpr uint8_t kBitmap8 = 0x18;
constexpr uint8_t kBitmap16 = 0x19;
constexpr uint8_t kUint8 = 0x20;
constexpr uint8_t kUint16 = 0x21;
constexpr uint8_t kUint24 = 0x22;
constexpr uint8_t kUint32 = 0x23;
constexpr uint8_t kInt8 = 0x28;
constexpr uint8_t kInt16 = 0x29;
constexpr uint8_t kInt24 = 0x2A;
constexpr uint8_t kInt32 = 0x2B;
constexpr uint8_t kEnum8 = 0x30;
constexpr uint8_t kEnum16 = 0x31;
constexpr uint8_t kCharString = 0x42;
} // namespace zcl_type

/// ZCL status codes the adapter distinguishes.
namespace zcl_status {
constexpr uint8_t kSuccess = 0x00;
constexpr uint8_t kUnsupportedAttribute = 0x86;
} // namespace zcl_status

/// A decoded ZCL attribute value: a number for the integer, boolean, bitmap and enumeration types,
/// a text for a character string.
struct ZclValue {
	/// Longest character string kept (the Basic cluster's names are at most 32 characters).
	static constexpr size_t kMaxString = 32;

	uint8_t type = 0;
	int64_t number = 0;
	char text[kMaxString + 1] = {};

	bool is_string() const { return type == zcl_type::kCharString; }
};

/// Octets of a fixed-size type; 0 for a character string or an unsupported type.
size_t zcl_type_size(uint8_t type);

/// Decodes a value of `type` from its ZCL encoding (little endian; a character string as its
/// length octet followed by the characters, 0xFF meaning "invalid", read as empty). Refuses (false,
/// the reason in `error`) an unsupported type, a length that does not match, and a string longer than
/// ZclValue::kMaxString.
bool decode_zcl(uint8_t type, const uint8_t* data, size_t length, ZclValue& out, const char** error = nullptr);

} // namespace zigbee
} // namespace hc

#endif // HC_ZIGBEE_ZCL_H
