/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ZIGBEE_ADAPTER_H
#define HC_ZIGBEE_ADAPTER_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/image/dynamic_specs.h"
#include "hc/json.h"
#include "hc/platform/blob_store.h"
#include "hc/platform/clock.h"
#include "hc/platform/zigbee_driver.h"
#include "hc/technology.h"
#include "hc/zigbee/cluster_mapping.h"
#include "hc/zigbee/device_database.h"
#include "hc/zigbee/zcl.h"

namespace hc {
namespace zigbee {

/// A simple descriptor as the platform received it (the cluster arrays are only valid during the
/// call).
struct SimpleDescriptor {
	uint8_t endpoint = 0;
	uint16_t profile = 0;
	uint16_t device = 0;
	const uint16_t* in = nullptr;
	size_t in_count = 0;
	const uint16_t* out = nullptr;
	size_t out_count = 0;
};

/// One attribute of a report or a read response, in its ZCL encoding (`data` valid during the call).
struct AttributeRecord {
	uint16_t attribute = 0;
	uint8_t status = zcl_status::kSuccess; ///< A report's records are always successes.
	uint8_t type = 0;
	const uint8_t* data = nullptr;
	size_t length = 0;
};

/// What the platform reported about the network.
struct NetworkStatus {
	bool up = false;
	uint16_t pan_id = 0;
	uint8_t channel = 0;
	Ieee extended_pan = 0;
	/// Monotonic time until which joining is permitted (0: closed).
	uint64_t permit_until_ms = 0;
};

/// One frame the adapter handled, for the monitor stream (hc::MonitorFeed).
struct ZigbeeTraffic {
	enum class Kind : uint8_t {
		report,  ///< An attribute report or read response record (`code` the attribute).
		action,  ///< A cluster command the device sent (`code` the command id).
		command, ///< A command the adapter sent for a datapoint (`code` the ZCL command id).
	};
	static constexpr uint16_t kNoDatapoint = 0xFFFF;

	Kind kind = Kind::report;
	Ieee ieee = 0;
	uint16_t short_addr = kNoShortAddress;
	uint8_t endpoint = 0;
	uint16_t cluster = 0;
	uint16_t code = 0;
	/// The datapoint reported or commanded; kNoDatapoint when the frame maps to none.
	uint16_t datapoint = kNoDatapoint;
	/// The datapoint's value (or the attribute's decoded value when unmapped); nullptr if none.
	const Value* value = nullptr;
};

/// Told about every frame the adapter handled (ZigbeeAdapter::set_traffic_listener()). Called on the
/// event loop's thread after the datapoint was reported.
class ZigbeeTrafficListener {
public:
	virtual void zigbee_traffic(const ZigbeeTraffic& traffic) = 0;

protected:
	~ZigbeeTrafficListener() = default;
};

/// A change of the coordinator's state that a pairing view shows (the WebSocket's `zigbee` message,
/// spec/api.md "Zigbee").
struct ZigbeeEvent {
	enum class Kind : uint8_t {
		network,          ///< The network came up or went down.
		permit,           ///< The stack reported the permit-join state (`permit_join_s`, 0: closed).
		joined,           ///< A new device joined, or a known one joined afresh after a reset.
		interviewing,     ///< The device's interview started.
		interviewed,      ///< The interview finished; its datapoints are in the model.
		interview_failed, ///< The interview failed (it is tried again when the device is heard).
		/// The interview ended incomplete: what answered is stored and in the model, the device is not
		/// interviewed, and the rest is asked for again when the device is heard (ZigbeeDevice::missing).
		interview_incomplete,
		left,             ///< The device left the network (it stays in the database).
		removed,          ///< The device was removed from the database.
		changed,          ///< Something else about the device changed (label, stale, heard again).
	};
	Kind kind = Kind::changed;
	Ieee ieee = 0;              ///< The device; 0 for network and permit.
	uint32_t permit_join_s = 0; ///< For permit: how long the network is open.
};

/// "network", "permit", "joined", "interviewing", "interviewed", "interview_failed",
/// "interview_incomplete", "left", "removed", "changed".
const char* zigbee_event_name(ZigbeeEvent::Kind kind);

/// Told about every ZigbeeEvent (ZigbeeAdapter::set_event_listener()), on the event loop's thread,
/// while the adapter is in the middle of the change: it may read the adapter's state but must not
/// call its operations.
class ZigbeeEventListener {
public:
	virtual void zigbee_event(const ZigbeeEvent& event) = 0;

protected:
	~ZigbeeEventListener() = default;
};

/// The Zigbee technology adapter (doc/zigbee.md): keeps the device database, interviews joining
/// devices, maps their clusters to datapoints (generic rules or the runtime image's bindings),
/// configures their reporting, turns attribute reports and button commands into datapoint reports,
/// sends the datapoints' commands through the ZigbeeDriver and marks a silent device's datapoints
/// stale.
///
/// - **Interview.** A device announce of an unknown (or rejoining after leave) device starts the
///   interview, one device at a time: active endpoints, the simple descriptor of each, the Basic
///   cluster's manufacturer and model, the IAS zone type and the IAS enrollment of a zone endpoint.
///   Each step has kInterviewTimeoutMs and kInterviewAttempts; a device whose endpoints cannot be
///   learnt stays uninterviewed and is tried again when it is heard after kInterviewRetryMs. An
///   endpoint whose simple descriptor or a Basic cluster that stays unanswered (a sleepy device fell
///   asleep) ends the interview *incomplete*: the endpoints learnt are stored and mapped like a
///   finished interview's, the device is not interviewed and records what is missing; when it is
///   heard again, at least kInterviewRetryMs after that end, a retry asks for the missing parts
///   only. A zone type that stays unanswered does not make it incomplete (the zone is named
///   "alarm", doc/zigbee.md). At the end (finished or incomplete) the database is written, the
///   reporting of the mapped attributes is configured, their clusters bound to the coordinator, the
///   datapoints loaded.
/// - **Datapoints.** Registered with the image service (a DynamicSpecProvider with a listener), the
///   adapter does not load the model: it states the generic datapoints of the devices the image does
///   not bind (dynamic_specs()), notifies the service when that set changes (a device interviewed or
///   removed) and binds its datapoints by name when the service has loaded the model
///   (model_loaded()): the generic ones and the image's datapoints with a `zigbee` binding. Alone
///   (no image service, the tests and a build without images), it loads the model itself (refused
///   while the model holds datapoints of another owner) and reloads it when a device is interviewed
///   or removed; values survive the reload.
/// - **Reporting.** Each point carries its effective reporting (the function table's default, or
///   the binding's override, spec/zigbee.md "Reporting overrides"); the interview configures it,
///   and bindings that change it configure the interviewed devices concerned again
///   (reconfigure_reporting()), except at their first application (the start).
/// - **Stale.** Every kCheckPeriodMs the adapter compares each device's silence with its stale
///   timeout: kStaleFactor times the shortest effective maximum reporting interval of its datapoints
///   (every frame of the device counts as heard, so the most frequent promise is the one to check; a
///   device without a periodic report, a pure button or one whose overrides turn it off with max 0
///   or 65535, never goes stale). Silence counts from the start
///   for a device not heard since. Any frame of a stale device ends it; its datapoints are read
///   again.
/// - **Commands.** A commandable point's command goes to the driver at once. While the network is
///   not up it is held (homecontrol #72): checked as if it were sent, stored by the model, the
///   datapoint marked pending; a later command replaces it. When the network comes up the held
///   commands are sent once, in datapoint order, one every kHeldSpacingMs. A report drops the held
///   value (the device's is newer). Refused: a point without a command, a value out of range, a
///   device that has not joined, and the stack's refusal while the network is up.
/// - **Radio facts** (homecontrol #98). The platform hands the link quality and signal strength of
///   each frame (radio()); every kCheckPeriodMs the adapter reads the coordinator's neighbour table
///   (ZigbeeDriver::read_neighbors()) and keeps, per device, whether it is a child of the coordinator
///   with its end-device timeout and the remaining time of its entry, and whether it is a router.
///   A device that enters or leaves the child table is logged at info, as is every join, rejoin and
///   leave the trust center reports (device_updated()), every authorization (device_authorized()),
///   every association (device_associated()) and every announce with a new short address (a rejoin
///   with a new address, whether the trust center reported it or not), so that a device's loss and
///   its return show in the log with their time and kind.
/// - **Topology** (homecontrol #98). The parent of a device that is not the coordinator's child is
///   found in the routers' neighbour tables: a pass asks each router (ZDO Mgmt_Lqi_req, paged, one
///   request in flight, kTopologyTimeoutMs each, at most kTopologyMaxPages pages per router) every
///   kTopologyPeriodMs and kTopologyDelayMs after a device announced itself or turned stale. A
///   router's child entry gives the device's parent, depth and the router's link quality of it; its
///   entry of the coordinator gives the router's link quality of the coordinator. A change of parent
///   is logged; a device the parent router no longer lists (the router answered completely) loses it.
/// - **Rejoin** (homecontrol #99, doc/zigbee.md "Rejoin policy"). When the network comes up the
///   adapter sets the trust center's rejoin policy (trust-center rejoins answered, refused for a
///   device without a link key) and makes the trust center hold a link key for every known device
///   (in the database and not left): ZigbeeDriver::trust_device(). It does so again for a device
///   whose key the stack drops: after a legacy device's authorization, and after a leave the trust
///   center hears of (a router's Update Device "left", the coordinator's child leaving to rejoin).
///   A device that stops being known (remove(), a leave without rejoin) has its keys dropped
///   (ZigbeeDriver::forget_device()), whatever became of its leave.
///   So a known device that lost its parent comes back by any rejoin while the network is closed,
///   and an unknown one is not given the network key. request_rejoin() forces a device's loss
///   (ZDO Mgmt_Leave with rejoin): one request at a time, sent again when the device is heard while
///   it has not taken it (a sleepy device listens right after its own frames), at most kRejoinSends
///   times; the device's announce ends it ("came back"), kRejoinWaitMs without one ends it at warn.
/// - **Reads.** After start() and after a silence, the server attributes of each device are read,
///   one device every kRefreshSpacingMs. A read the application asks for (read(), a rule's `read`
///   action) sends a Read Attributes of the datapoint's attribute at once; refused for a point the
///   adapter does not read on start either (a client command of a button, a multistate input's
///   events) and while the device has not joined.
///
/// Memory: start(), configure() and the reload allocate the point table, the device database is
/// static (DeviceDatabase::kMaxDevices); reports, commands and the stale check allocate nothing.
/// Threading: the event loop's thread; the platform posts its events there.
class ZigbeeAdapter : public Technology, public image::DynamicSpecProvider {
public:
	/// A point whose datapoint is not in the model (point_id()).
	static constexpr uint16_t kNoId = 0xFFFF;
	/// The MAC capability bit of a router (a full-function device) in a device announce.
	static constexpr uint8_t kCapabilityRouter = 0x02;
	/// Update Device status values (device_updated()).
	static constexpr uint8_t kUpdateSecuredRejoin = 0x00;
	static constexpr uint8_t kUpdateUnsecuredJoin = 0x01;
	static constexpr uint8_t kUpdateLeft = 0x02;
	static constexpr uint8_t kUpdateTrustCenterRejoin = 0x03;
	/// The trust center's decision on an Update Device (device_updated()): it took the device.
	static constexpr uint8_t kTcAuthorize = 0x00;
	/// The authorization type of a legacy device (device_authorized()), one that never asks for a
	/// trust-center link key of its own.
	static constexpr uint8_t kAuthorizationLegacy = 0x00;

	static constexpr uint32_t kInterviewTimeoutMs = 10000;
	static constexpr uint8_t kInterviewAttempts = 3;
	static constexpr uint32_t kInterviewRetryMs = 60000;
	/// An announce heard this shortly before the trust center's join notice belongs to that join.
	static constexpr uint64_t kAnnounceBeforeJoinMs = 5000;
	static constexpr uint32_t kCheckPeriodMs = 10000;
	/// The routers' neighbour tables are read every kTopologyPeriodMs, and kTopologyDelayMs after a
	/// device announced itself or turned stale (the router has the rejoined child in its table then).
	static constexpr uint32_t kTopologyPeriodMs = 300000;
	static constexpr uint32_t kTopologyDelayMs = 3000;
	/// How long a router has to answer one Mgmt_Lqi_req.
	static constexpr uint32_t kTopologyTimeoutMs = 5000;
	/// Pages of one router's table read in a pass (a Mgmt_Lqi_rsp carries about three entries).
	static constexpr uint8_t kTopologyMaxPages = 16;
	/// An announce this shortly after the trust center's notice of the device belongs to it.
	static constexpr uint64_t kNoticeBeforeAnnounceMs = 10000;
	static constexpr uint32_t kStaleFactor = 3;
	/// A device's `last_seen` is written at the latest this long after it moved past the stored one
	/// (by the check every kCheckPeriodMs; one write covers every device): after a restart it is at
	/// most an hour behind the last frame, and the flash sees at most about one write an hour.
	static constexpr int64_t kSeenSaveS = 3600;
	static constexpr uint32_t kRefreshSpacingMs = 500;
	/// Spacing of the commands held while the network was not up, once it is (the first goes at once).
	static constexpr uint32_t kHeldSpacingMs = 100;
	/// Transition of the level and colour temperature commands, in tenths of a second.
	static constexpr uint16_t kTransitionDs = 5;
	static constexpr uint16_t kIdentifySeconds = 10;
	/// Longest permit-join time the stack accepts (255 would mean "forever" in Zigbee 2006).
	static constexpr uint32_t kMaxPermitJoinS = 254;
	/// A requested rejoin (request_rejoin()) is sent again when the device is heard, at the earliest
	/// kRejoinResendMs after the last send, at most kRejoinSends times in all; it ends kRejoinWaitMs
	/// after the request when the device has not announced itself by then.
	static constexpr uint32_t kRejoinResendMs = 30000;
	static constexpr uint8_t kRejoinSends = 5;
	static constexpr uint32_t kRejoinWaitMs = 3600000;
	/// Mgmt_Leave_rsp status: the device takes the request.
	static constexpr uint8_t kLeaveSuccess = 0x00;
	/// Endpoints of one device the interview asks for (it keeps ZigbeeDevice::kMaxEndpoints).
	static constexpr size_t kMaxQueriedEndpoints = 8;
	static constexpr size_t kMaxOverrides = 512;
	/// Points of one device the reporting set-up considers (every endpoint with every function).
	static constexpr size_t kMaxDevicePoints = ZigbeeDevice::kMaxEndpoints * (kFunctionCount + 1);

	/// `store` holds the device database; nullptr keeps it in memory only.
	ZigbeeAdapter(EventLoop& loop, const Clock& clock, DatapointModel& model, ZigbeeDriver& driver, BlobStore* store);
	~ZigbeeAdapter();

	ZigbeeAdapter(const ZigbeeAdapter&) = delete;
	ZigbeeAdapter& operator=(const ZigbeeAdapter&) = delete;

	/// Reads the database (an unreadable one is logged and starts empty, the blob is kept until the
	/// next write), loads the datapoints and starts the stale check and the reads. Once; false (with
	/// `error`) when called again or the datapoints cannot be loaded (the adapter runs anyway and
	/// loads them with the next change).
	bool start(const char** error = nullptr);

	/// Alone (without an image service): replaces the runtime image's device bindings (copied); with
	/// an image service the bindings come with model_loaded() and this is refused. Checked before anything changes: every
	/// binding has a name and maps a known function with an acceptable reporting override
	/// (override_point()), no name and no
	/// (device, endpoint, function) appears twice, at most kMaxOverrides. Reloads the datapoints when
	/// started. A refusal (false, the reason in `error` and the log) keeps the previous bindings.
	bool configure(const ZigbeeOverride* overrides, size_t count, const char** error = nullptr);

	// ---- Operations (console, E-Home through the API) ----

	/// Opens the network for `seconds` (1..kMaxPermitJoinS), 0 closes it.
	bool permit_join(uint32_t seconds, const char** error = nullptr);
	/// Drops the trust center's link keys for the device (refused, with nothing changed, when the stack
	/// cannot: a removed device must not come back by a trust-center rejoin), asks the device to leave,
	/// deletes it from the database and reloads the datapoints.
	bool remove(Ieee ieee, const char** error = nullptr);
	/// Makes the device identify itself for kIdentifySeconds.
	bool identify(Ieee ieee, const char** error = nullptr);
	/// Sets the device's label (at most ZigbeeDevice::kMaxText bytes; empty clears it) and writes the
	/// database.
	bool set_label(Ieee ieee, const char* label, const char** error = nullptr);
	/// Interviews a known device again (after a firmware update changed its endpoints).
	bool interview(Ieee ieee, const char** error = nullptr);
	/// Forces the device's loss (homecontrol #99): asks it to leave and rejoin at once (ZDO
	/// Mgmt_Leave with rejoin), so that its return shows whether the trust center takes it back.
	/// Refused while the network is not up, for an unknown or left device, one without a short
	/// address, and while another device's requested rejoin is pending.
	bool request_rejoin(Ieee ieee, const char** error = nullptr);
	/// The device whose requested rejoin is pending, 0 if none.
	Ieee rejoin_pending() const { return rejoin_.ieee; }

	// ---- Platform events (on the event loop) ----

	void network_state(bool up, uint16_t pan_id, uint8_t channel, Ieee extended_pan);
	/// The stack's permit-join state: open for `seconds`, 0 closed.
	void permit_join_status(uint8_t seconds);
	/// The trust center accepted a fresh association of the device (an unsecured join, not a
	/// rejoin): a new device, or a known one after a factory reset, whose bindings and reporting
	/// configuration are gone; it is interviewed again with the announce that follows.
	void device_joined(NodeAddress address);
	void device_announced(NodeAddress address);
	/// The announce with the device's MAC capability (bit 0x02: a router).
	void device_announced(NodeAddress address, uint8_t capability);
	/// The trust center's Update Device (a join, rejoin or leave through it or its routers): `status`
	/// as in Zigbee's Update Device command (UpdateDevice), `tc_action` what the trust center decided
	/// (TcAction), `parent_short` the parent the device joined through. Logged at info with the device;
	/// an unsecured join continues as device_joined().
	void device_updated(NodeAddress address, uint8_t status, uint8_t tc_action, uint16_t parent_short);
	/// The trust center authorized a joining or rejoining device, or failed to: `type` and `status`
	/// as the stack gives them (0 success). Logged at info, a failure at warn.
	void device_authorized(NodeAddress address, uint8_t type, uint8_t status);
	/// A device associated with the coordinator at the MAC level (a join during permit join). Logged at
	/// info.
	void device_associated(Ieee ieee);
	/// The link quality and signal strength the stack gives for the device's last frame; told before
	/// the frame's own event. Kept for the adapter view, not a sign of life by itself.
	void radio(NodeAddress address, uint8_t lqi, int8_t rssi);
	/// A router's answer to ZigbeeDriver::request_neighbor_table(): `status` (0 success), the
	/// router's `total` entries, the page from `start_index` (the entries valid during the call).
	void neighbor_table(NodeAddress router, uint8_t status, uint8_t total, uint8_t start_index,
		const ZigbeeNeighbor* entries, size_t count);
	void device_left(NodeAddress address);
	/// The coordinator's child left with rejoin set (it comes back at once): the stack forgot its
	/// link key, the adapter makes the trust center hold one again.
	void device_left_to_rejoin(NodeAddress address);
	/// A device's answer to a leave request (ZigbeeDriver::leave(), leave_and_rejoin()): `status` 0
	/// it takes it, anything else (the stack's timeout included) it did not.
	void leave_answered(NodeAddress address, uint8_t status);
	void active_endpoints(NodeAddress address, uint8_t status, const uint8_t* endpoints, size_t count);
	void simple_descriptor(NodeAddress address, uint8_t status, const SimpleDescriptor& descriptor);
	/// An attribute report or a read attributes response.
	void attributes(NodeAddress address, uint8_t endpoint, uint16_t cluster, const AttributeRecord* records,
		size_t count);
	/// A cluster command the device sent to the coordinator (a button's On/Off or Level Control
	/// command, an IAS Zone Status Change Notification).
	void command_received(NodeAddress address, uint8_t endpoint, uint16_t cluster, uint8_t command,
		const uint8_t* payload, size_t length);
	void reporting_configured(NodeAddress address, uint8_t endpoint, uint16_t cluster, uint8_t status);
	void bind_result(NodeAddress address, uint8_t status);

	// ---- State ----

	const DeviceDatabase& devices() const { return database_; }
	const NetworkStatus& network() const { return network_; }
	size_t point_count() const { return point_count_; }
	const ZigbeePoint& point(size_t index) const { return points_[index]; }
	/// The model id of point `index`; kNoId while it is not in the model.
	uint16_t point_id(size_t index) const { return loaded_ ? ids_[index] : kNoId; }
	/// The device's stale timeout in seconds (0: never stale, or unknown); only the functions of the
	/// endpoints it described count.
	uint32_t stale_timeout_s(Ieee ieee) const;
	/// The device being interviewed, 0 if none.
	Ieee interviewing() const { return interview_.active ? interview_.ieee : 0; }
	/// Whether the datapoints are in the model (start() or the last reload succeeded).
	bool loaded() const { return loaded_; }

	/// The one traffic listener (the monitor stream); nullptr: none. It must outlive the adapter or be
	/// removed first.
	void set_traffic_listener(ZigbeeTrafficListener* listener) { traffic_ = listener; }
	ZigbeeTrafficListener* traffic_listener() const { return traffic_; }

	/// The one event listener (the web API's live feed); nullptr: none. It must outlive the adapter or
	/// be removed first.
	void set_event_listener(ZigbeeEventListener* listener) { events_ = listener; }
	ZigbeeEventListener* event_listener() const { return events_; }
	/// Counts every ZigbeeEvent: a view that saw another generation reads the devices again.
	uint32_t generation() const { return generation_; }
	/// How long the network stays open for joining, in whole seconds (0: closed).
	uint32_t permit_join_s() const;

	/// {"network": {...}, "devices": [{ieee, short, manufacturer, model, label, interviewed, stale,
	/// left, interviewing, last_seen, seen_ago_s, lqi, rssi, parent, parent_label, timeout_s,
	/// expires_in_s, missing, incomplete, rejoin, datapoints: [names]}]} for the console, the API and
	/// E-Home's pairing view (spec/zigbee.md; `missing` and `incomplete` only for an incomplete
	/// interview, the radio facts only where known, `rejoin` only while a requested rejoin is pending).
	void write_json(JsonWriter& json) const;
	/// The members of the "network" object into an open object.
	void write_network_json(JsonWriter& json) const;
	/// Device `index` of devices() as an object.
	void write_device_json(JsonWriter& json, size_t index) const;
	/// The device as E-Home's importer needs it (spec/zigbee.md "Export"): identity, label, the
	/// interview result (endpoints with profile, device id, input and output clusters, zone type)
	/// and every function the mapping derives, with its generic datapoint name, cluster, attribute,
	/// value type, unit, whether it is commandable, and the name the loaded model gives it; `missing`
	/// for an incomplete interview.
	void write_device_export_json(JsonWriter& json, size_t index) const;

	// Technology
	const char* name() const override { return "zigbee"; }
	bool command(const Datapoint& datapoint, const Value& value) override;
	bool read(const Datapoint& datapoint) override;
	bool holds_commands() const override { return true; }

	/// Commands held while the network was not up and not yet sent (Datapoint::pending()).
	size_t commands_held() const;

	// image::DynamicSpecProvider (with the image service, doc/image.md "Dynamic datapoints")
	const char* provider_name() const override { return "zigbee"; }
	/// The generic datapoints of the interviewed devices that `bindings` does not mention. The
	/// bindings are taken over (copied) only with model_loaded().
	bool dynamic_specs(const image::ImageBindings& bindings, const image::DynamicSpec*& specs, size_t& count,
		const char** error) override;
	/// Takes over the bindings and the points of the last dynamic_specs() call and binds them by name.
	void model_loaded(const image::ImageBindings& bindings) override;
	void model_cleared() override;

private:
	enum class Step : uint8_t { active_endpoints, simple_descriptor, basic, zone };

	/// The tables of a dynamic_specs() call, adopted by model_loaded().
	struct Pending;

	struct Interview {
		bool active = false;
		Ieee ieee = 0;
		Step step = Step::active_endpoints;
		uint8_t attempts = 0;
		uint8_t index = 0;
		uint8_t queried[kMaxQueriedEndpoints] = {};
		uint8_t queried_count = 0;
		/// Basic is read; false in the retry of an incomplete interview whose names are known (the
		/// retry starts `result` from the device's partial result, `queried` are its missing endpoints).
		bool names_wanted = true;
		/// The first endpoint of `result` this interview learnt (the zone types are read from there).
		uint8_t first_new = 0;
		/// What stayed unanswered (`result.missing`, `result.missing_names`) is collected in `result`.
		ZigbeeDevice result;
		TimerHandle timer;
	};

	ZigbeeDevice* resolve(NodeAddress address, bool add);
	void touch(ZigbeeDevice& device);
	void start_interview(ZigbeeDevice& device);
	void send_step();
	void step_timeout();
	void next_simple_descriptor();
	void start_basic();
	void start_zone(uint8_t from_index);
	void finish_interview();
	/// The English reason of an incomplete device ("endpoint 2 did not answer"), for the log and the
	/// adapter view.
	static const char* missing_text(const ZigbeeDevice& device, char* buffer, size_t size);
	void fail_interview(const char* reason);
	void end_interview();
	void start_pending_interview();
	void setup_reporting(const ZigbeeDevice& device);
	/// After the bindings changed from `old_table`: configures the reporting of every function of an
	/// interviewed device whose effective reporting the new bindings change (or that the old ones did
	/// not make a point), and binds its cluster.
	void reconfigure_reporting(const ZigbeePoint* old_table, size_t old_count);
	bool rebuild(const char** error);
	bool save_database();
	void check_stale();
	/// Reads the coordinator's neighbour table into the devices' radio facts and logs who entered or
	/// left the child table.
	void read_child_table();
	class ChildTableReader;
	/// A pass over the routers' neighbour tables (homecontrol #98).
	struct Topology {
		bool active = false;
		/// Another pass is due when this one ends.
		bool again = false;
		/// The router asked now (database index), its address and the page asked for.
		size_t router = 0;
		Ieee router_ieee = 0;
		uint16_t router_short = kNoShortAddress;
		uint8_t start = 0;
		uint8_t pages = 0;
		/// Database indices (bits) of the devices a router listed as its child in this pass, and of the
		/// routers whose table was read to its end.
		uint32_t listed = 0;
		uint32_t answered = 0;
		/// The delay before a pass, or the timeout of the request in flight.
		TimerHandle timer;
	};
	static_assert(DeviceDatabase::kMaxDevices <= 32, "Topology's bit sets hold one bit per device");
	Topology topology_;
	TimerHandle topology_period_;
	/// The trust center's last notice (device_updated()), for the announce that follows it.
	Ieee notice_ieee_ = 0;
	uint8_t notice_status_ = 0;
	uint64_t notice_ms_ = 0;
	/// The device's short address before that notice (kNoShortAddress: it was not known).
	uint16_t notice_old_short_ = kNoShortAddress;
	/// Schedules a pass in kTopologyDelayMs (or after the running one).
	void request_topology();
	void topology_tick();
	void topology_start();
	/// Asks the next router from database index `from`; ends the pass when there is none.
	void topology_next_router(size_t from);
	void topology_send();
	void topology_timeout();
	void topology_finish();
	/// Ends a running pass without its conclusions (the database indices changed).
	void topology_abort();
	/// Whether the child table was read since the start (the first read logs every child it finds).
	bool children_read_ = false;
	/// The requested rejoin (request_rejoin()); `ieee` 0: none pending.
	struct Rejoin {
		Ieee ieee = 0;
		/// The short address the request went to (a leave answer may come without the IEEE).
		uint16_t short_addr = kNoShortAddress;
		uint64_t since_ms = 0;
		uint64_t sent_ms = 0;
		uint8_t sends = 0;
		/// The device answered the request with success.
		bool accepted = false;
		/// Ends the wait kRejoinWaitMs after the request.
		TimerHandle timer;
	};
	Rejoin rejoin_;
	/// Sends the pending rejoin request again (logged).
	void resend_rejoin(ZigbeeDevice& device);
	/// The device announced itself: ends its pending rejoin ("came back").
	void rejoin_returned(const ZigbeeDevice& device);
	void rejoin_expired();
	void end_rejoin();
	/// Sets the rejoin policy and makes the trust center hold a key for every known device (network up).
	void trust_known_devices();
	/// Makes the trust center hold a key for the device unless it left (logged when the key was restored
	/// or refused; `why` names the occasion).
	ZigbeeDriver::Trust trust(const ZigbeeDevice& device, const char* why);
	void refresh_due();
	void schedule_refresh();
	/// Reports the value of the mapped point; the datapoint's id, kNoId when unmapped.
	uint16_t report_point(const ZigbeeDevice& device, uint8_t endpoint, Function function, const Value& value);
	void traffic(ZigbeeTraffic::Kind kind, const ZigbeeDevice& device, uint8_t endpoint, uint16_t cluster, uint16_t code,
		uint16_t datapoint, const Value* value);
	long find_point(Ieee ieee, uint8_t endpoint, Function function) const;
	bool device_has_overrides(Ieee ieee) const;
	void free_overrides();
	void free_pending();
	bool specs_differ() const;
	long index_of(uint16_t id) const;
	/// The points of one device on the endpoints it described: its bindings, or the generic mapping
	/// when the image binds none. A binding of an endpoint the device has not described (an incomplete
	/// interview's missing endpoint, or one the device lacks) is a datapoint but gets no coordinator
	/// binding, reporting configuration or read until an interview learns the endpoint.
	size_t device_points(const ZigbeeDevice& device, ZigbeePoint* out, size_t capacity) const;
	const char* device_text(const ZigbeeDevice& device, char* buffer, size_t size) const;
	/// The device's text, or its IEEE address while it is not in the database.
	const char* node_text(NodeAddress address, char* buffer, size_t size);
	/// "the coordinator", the router's device text, or "router 0x1234".
	const char* parent_text(uint16_t parent_short, char* buffer, size_t size);
	/// lqi, rssi, parent, parent_label, timeout_s, expires_in_s where known.
	void write_radio_json(JsonWriter& json, const ZigbeeDevice& device) const;

	EventLoop& loop_;
	const Clock& clock_;
	DatapointModel& model_;
	ZigbeeDriver& driver_;
	DeviceDatabase database_;
	NetworkStatus network_;
	Interview interview_;
	ZigbeePoint* points_ = nullptr;
	uint16_t* ids_ = nullptr; ///< The model id of each point (kNoId: not in the model).
	size_t point_count_ = 0;
	Pending* pending_ = nullptr;
	ZigbeePoint* overrides_ = nullptr; ///< The image's bindings, names in override_names_.
	size_t override_count_ = 0;
	char* override_names_ = nullptr;
	bool persistent_;
	bool started_ = false;
	bool loaded_ = false;
	/// Whether bindings were applied before (configure() or model_loaded()): the first application,
	/// at the start, configures nothing (the devices kept what their interview configured).
	bool bindings_known_ = false;
	uint64_t start_ms_ = 0;
	TimerHandle check_timer_;
	TimerHandle refresh_timer_;
	TimerHandle held_timer_;
	uint16_t held_next_ = 0; ///< The model id from which the next held command is looked for.
	bool hold_logged_ = false;

	/// Whether the point has a command and `value` is in its range (logged when not).
	bool command_valid(const ZigbeePoint& point, const Datapoint& datapoint, const Value& value) const;
	/// Sends the command (command_valid() said yes); false (logged) when the stack does not take it.
	bool send_command(const ZigbeeDevice& device, const ZigbeePoint& point, const Datapoint& datapoint,
		const Value& value);
	/// Sends the next held command and schedules the one after it (kHeldSpacingMs).
	void send_held();
	ZigbeeTrafficListener* traffic_ = nullptr;
	ZigbeeEventListener* events_ = nullptr;
	uint32_t generation_ = 0;

	void notify(ZigbeeEvent::Kind kind, Ieee ieee, uint32_t permit_join_s = 0);
};

} // namespace zigbee
} // namespace hc

#endif // HC_ZIGBEE_ADAPTER_H
