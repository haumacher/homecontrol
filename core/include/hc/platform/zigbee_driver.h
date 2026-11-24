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

/// What the coordinator's radio heard of one short address and sent to it, counted below the stack
/// in the 802.15.4 driver's events (homecontrol #117: why a sleepy child loses the coordinator as its
/// parent). Counted since the last read with reset, or since the start.
struct ZigbeePolls {
	/// Number of gaps between polls the platform keeps, the newest first.
	static constexpr size_t kGaps = 6;
	/// MAC Data Requests (the polls of a sleepy child) the radio received from the address and
	/// acknowledged.
	uint32_t polls = 0;
	/// Of those, the polls whose acknowledgement said a frame is pending for the child.
	uint32_t pending = 0;
	/// Milliseconds since the last poll; UINT32_MAX without one.
	uint32_t since_poll_ms = UINT32_MAX;
	/// The longest time between two polls, in milliseconds.
	uint32_t longest_gap_ms = 0;
	/// The last gaps between polls in milliseconds, the newest first; 0 where there is none.
	uint32_t gaps_ms[kGaps] = {};
	/// Polls answered with frame pending whose frame reached the child (sent and acknowledged before its
	/// next poll), and those whose frame did not (the child polled again first).
	uint32_t delivered = 0;
	uint32_t undelivered = 0;
	/// Other frames the radio received from the address (its data and commands, not its polls).
	uint32_t received = 0;
	/// One radio event of the address (ZigbeePolls::events).
	enum class Event : uint8_t {
		none = 0,
		poll,          ///< a poll, acknowledged without frame pending
		poll_pending,  ///< a poll, acknowledged with frame pending
		received,      ///< another frame from the address
		sent,          ///< a frame to it, acknowledged
		sent_no_ack,   ///< a frame to it without acknowledgement
		sent_coexist,  ///< a frame to it the WiFi coexistence refused
		sent_failed,   ///< a frame to it that failed otherwise
	};
	struct Timed {
		Event event = Event::none;
		/// The frame's kind where the radio can tell: the MAC frame type (1 data, 3 command) and, for a
		/// data frame, the NWK frame type (0 data, 1 command) and whether the NWK layer secured it.
		uint8_t mac_type = 0;
		uint8_t nwk_type = 0;
		bool nwk_secured = false;
		/// Milliseconds before the read.
		uint32_t ago_ms = 0;
	};
	/// Number of events the platform keeps, the newest first.
	static constexpr size_t kEvents = 12;
	Timed events[kEvents];
	/// Frames the radio sent to the address (each attempt counts), and of those the ones without an
	/// acknowledgement, refused by the WiFi coexistence, or failed otherwise (channel busy, abort).
	uint32_t sent = 0;
	uint32_t no_ack = 0;
	uint32_t coexist = 0;
	uint32_t failed = 0;
	/// The same failures of every frame the radio sent since the start.
	uint32_t all_sent = 0;
	uint32_t all_no_ack = 0;
	uint32_t all_coexist = 0;
	uint32_t all_failed = 0;
};

/// One entry of a neighbour table: the coordinator's own as the stack keeps it
/// (ZigbeeDriver::read_neighbors()), or a router's from its Mgmt_Lqi_rsp
/// (ZigbeeAdapter::neighbor_table()).
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
	/// The neighbour is the coordinator (only in a router's table).
	bool coordinator = false;
	/// Whether the neighbour's receiver is on while it is idle (a sleepy end device's is off;
	/// homecontrol #108): the table's own field, unknown where it has none.
	zigbee::Receiver receiver = zigbee::Receiver::unknown;
	Relationship relationship = Relationship::none;
	/// The end-device timeout of a child in seconds: how long the coordinator keeps it in its child
	/// table without hearing from it (negotiated with an End Device Timeout Request, else the stack's
	/// default).
	uint32_t timeout_s = 0;
	/// The stack's aging counter of a child in milliseconds: how long it stays in the child table
	/// unless it is heard (each poll of a sleepy child restarts it).
	uint32_t expires_ms = 0;
	/// The table owner's link quality of the neighbour (0..255; a router's table).
	uint8_t lqi = 0;
	/// The neighbour's depth in the network (0: the coordinator; a router's table).
	uint8_t depth = 0;
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

	/// ZDO Management Leave Request with rejoin set (homecontrol #99): the device leaves the network
	/// and rejoins it at once, keeping the network key; a sleepy end device takes the request from
	/// its parent's queue when it next polls. The device's answer arrives as
	/// ZigbeeAdapter::leave_answered().
	virtual bool leave_and_rejoin(zigbee::Ieee ieee, uint16_t short_addr) = 0;

	/// Sets the trust center's rejoin policy (homecontrol #99, doc/zigbee.md "Rejoin policy"):
	/// `trust_center_rejoin` whether it answers trust-center rejoins at all (the unsecured rejoin of a
	/// device whose network key is missing or outdated), `without_link_key` whether it accepts one from
	/// a device it holds no link key for (true would hand the network key to any device that asks).
	/// False when the stack does not run.
	virtual bool set_rejoin_policy(bool trust_center_rejoin, bool without_link_key) = 0;

	/// What trust_device() found.
	enum class Trust : uint8_t {
		kept,     ///< The trust center holds a link key for the device (a verified or provisional one).
		restored, ///< It held none; the well-known key the device joined with is its key again.
		refused,  ///< It held none and the stack did not take the key (the stack does not run, a full table).
	};

	/// Makes the trust center hold a link key for a device the adapter knows (homecontrol #99), so that
	/// it accepts the device's trust-center rejoin while the rejoin policy refuses devices without one:
	/// a device without a key (a legacy device, whose provisional key the stack deletes after its
	/// authorization; any device the stack forgot when it left) gets the well-known key it joined
	/// with, as the stack itself gives a joining device. `stack_answer` is the stack's answer to the
	/// write (0 when nothing was written), for the log; the result comes from the key table
	/// (hc/zigbee/key_table.h), not from that answer.
	virtual Trust trust_device(zigbee::Ieee ieee, int& stack_answer) = 0;

	/// Drops every link key the trust center holds for a device that is no longer known (removed, or
	/// left without rejoin; homecontrol #99), so that it refuses the device's trust-center rejoin,
	/// whatever became of the device's leave. True when the trust center holds no verified or
	/// provisional key for it afterwards; false when the stack did not drop it (it does not run, or the
	/// key is still there).
	virtual bool forget_device(zigbee::Ieee ieee) = 0;

	/// Reads the coordinator's neighbour table (homecontrol #98): calls `visitor` once per entry before
	/// it returns, on the calling thread, while the platform holds the stack (the visitor only copies,
	/// it must not call the driver). False when the table could not be read now (stack not running, or
	/// busy longer than the platform waits for it); the visitor was not called then.
	virtual bool read_neighbors(ZigbeeNeighborVisitor& visitor) = 0;

	/// ZDO Mgmt_Lqi_req to a router: its neighbour table from entry `start_index` (homecontrol #98).
	/// The answer arrives as ZigbeeAdapter::neighbor_table().
	virtual bool request_neighbor_table(zigbee::Ieee ieee, uint16_t short_addr, uint8_t start_index) = 0;

	/// What the radio heard of `short_addr` and sent to it (ZigbeePolls); `reset` starts the address's
	/// counts again after the read (the totals of every frame stay). False where the platform does not
	/// count (the default: the host and the test fakes).
	virtual bool read_polls(uint16_t short_addr, bool reset, ZigbeePolls& out) {
		(void) short_addr;
		(void) reset;
		(void) out;
		return false;
	}

protected:
	~ZigbeeDriver() = default;
};

} // namespace hc

#endif // HC_PLATFORM_ZIGBEE_DRIVER_H
