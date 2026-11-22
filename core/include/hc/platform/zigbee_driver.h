/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_ZIGBEE_DRIVER_H
#define HC_PLATFORM_ZIGBEE_DRIVER_H

#include <cstddef>
#include <cstdint>

#include "hc/zigbee/zcl.h"

namespace hc {

/// The commands of the On/Off cluster the adapter sends.
enum class OnOffCommand : uint8_t {
	off = 0x00,
	on = 0x01,
	toggle = 0x02,
};

/// One attribute's reporting configuration (ZCL Configure Reporting): the device reports when the
/// value changed by `change` (analog types; ignored for discrete ones), at most every `min_s` and at
/// least every `max_s` seconds.
struct ReportingConfig {
	uint16_t attribute;
	uint8_t type; ///< ZCL data type of the attribute (hc::zigbee::zcl_type).
	uint16_t min_s;
	uint16_t max_s;
	uint32_t change;
};

/// One entry of the coordinator's neighbour table as the stack keeps it (ZigbeeDriver::read_neighbors()).
struct ZigbeeNeighbor {
	/// The neighbour's relationship to the coordinator (Zigbee nwkNeighborTable).
	enum class Relationship : uint8_t {
		parent = 0,
		child = 1,
		sibling = 2,
		none = 3,
		previous_child = 4,
		unauthenticated_child = 5,
	};
	zigbee::Ieee ieee = 0;
	uint16_t short_addr = zigbee::kNoShortAddress;
	/// The neighbour is a router (a full-function device that routes; an end device is not).
	bool router = false;
	Relationship relationship = Relationship::none;
	/// The end-device timeout of a child in seconds: how long the coordinator keeps it in its child
	/// table without hearing from it (negotiated with an End Device Timeout Request, else the stack's
	/// default).
	uint32_t timeout_s = 0;
	/// The stack's aging counter of a child in milliseconds: how long it stays in the child table
	/// unless it is heard (each poll of a sleepy child restarts it).
	uint32_t expires_ms = 0;
};

/// Told about every entry of the neighbour table (ZigbeeDriver::read_neighbors()).
class ZigbeeNeighborVisitor {
public:
	virtual void neighbor(const ZigbeeNeighbor& neighbor) = 0;

protected:
	~ZigbeeNeighborVisitor() = default;
};

/// The Zigbee coordinator as the core sees it (doc/zigbee.md): requests to the network, sent by the
/// platform's stack. Implemented by the ESP component hc_zigbee over the esp-zigbee-lib, and by a
/// recording fake in the tests.
///
/// Every request is asynchronous: true means the stack accepted it for sending, false refuses it
/// (stack not running, no buffer); the platform logs why. Answers and frames arrive later as calls of
/// the ZigbeeAdapter on the event loop (hc/zigbee/adapter.h). A device is addressed by its IEEE and
/// short address; the platform uses the short address and may fall back to the IEEE address.
///
/// Threading: called on the event loop's thread; the platform hands the request to the stack's task.
class ZigbeeDriver {
public:
	/// Opens the network for joining for `seconds` (1..254), or closes it (0).
	virtual bool permit_join(uint8_t seconds) = 0;

	/// ZDO Active Endpoint Request.
	virtual bool request_active_endpoints(zigbee::Ieee ieee, uint16_t short_addr) = 0;

	/// ZDO Simple Descriptor Request for one endpoint.
	virtual bool request_simple_descriptor(zigbee::Ieee ieee, uint16_t short_addr, uint8_t endpoint) = 0;

	/// ZCL Read Attributes of up to 8 attributes of one cluster.
	virtual bool read_attributes(zigbee::Ieee ieee, uint16_t short_addr, uint8_t endpoint, uint16_t cluster,
		const uint16_t* attributes, size_t count) = 0;

	/// ZCL Configure Reporting of one attribute, reports to the coordinator.
	virtual bool configure_reporting(zigbee::Ieee ieee, uint16_t short_addr, uint8_t endpoint, uint16_t cluster,
		const ReportingConfig& config) = 0;

	/// ZDO Bind Request: the device's `cluster` on `endpoint` is bound to the coordinator's endpoint,
	/// so its reports and (for a client cluster) its commands come to the coordinator.
	virtual bool bind(zigbee::Ieee ieee, uint16_t short_addr, uint8_t endpoint, uint16_t cluster) = 0;

	/// IAS enrollment: writes the coordinator's IEEE address to the zone's IAS_CIE_Address; the
	/// platform answers the zone's Zone Enroll Request.
	virtual bool ias_enroll(zigbee::Ieee ieee, uint16_t short_addr, uint8_t endpoint) = 0;

	/// On/Off cluster command.
	virtual bool on_off(zigbee::Ieee ieee, uint16_t short_addr, uint8_t endpoint, OnOffCommand command) = 0;

	/// Level Control "move to level (with on/off)": level 0..254, transition in tenths of a second.
	virtual bool move_to_level(zigbee::Ieee ieee, uint16_t short_addr, uint8_t endpoint, uint8_t level,
		uint16_t transition_ds) = 0;

	/// Color Control "move to color temperature" in mireds, transition in tenths of a second.
	virtual bool move_to_color_temperature(zigbee::Ieee ieee, uint16_t short_addr, uint8_t endpoint, uint16_t mireds,
		uint16_t transition_ds) = 0;

	/// Identify cluster "identify" for `seconds` (the device blinks).
	virtual bool identify(zigbee::Ieee ieee, uint16_t short_addr, uint8_t endpoint, uint16_t seconds) = 0;

	/// ZDO Management Leave Request: the device leaves the network (without rejoin).
	virtual bool leave(zigbee::Ieee ieee, uint16_t short_addr) = 0;

	/// Reads the coordinator's neighbour table (homecontrol #98): calls `visitor` once per entry before
	/// it returns, on the calling thread, while the platform holds the stack (the visitor only copies,
	/// it must not call the driver). False when the table could not be read now (stack not running, or
	/// busy longer than the platform waits for it); the visitor was not called then.
	virtual bool read_neighbors(ZigbeeNeighborVisitor& visitor) = 0;

protected:
	~ZigbeeDriver() = default;
};

} // namespace hc

#endif // HC_PLATFORM_ZIGBEE_DRIVER_H
