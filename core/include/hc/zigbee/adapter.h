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
///   timeout: kStaleFactor times the largest effective maximum reporting interval of its datapoints
///   (a device without a periodic report, a pure button or one whose overrides turn it off with max
///   0 or 65535, never goes stale). Silence counts from the start
///   for a device not heard since. Any frame of a stale device ends it; its datapoints are read
///   again.
/// - **Commands.** A commandable point's command goes to the driver at once. While the network is
///   not up it is held (homecontrol #72): checked as if it were sent, stored by the model, the
///   datapoint marked pending; a later command replaces it. When the network comes up the held
///   commands are sent once, in datapoint order, one every kHeldSpacingMs. A report drops the held
///   value (the device's is newer). Refused: a point without a command, a value out of range, a
///   device that has not joined, and the stack's refusal while the network is up.
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

	static constexpr uint32_t kInterviewTimeoutMs = 10000;
	static constexpr uint8_t kInterviewAttempts = 3;
	static constexpr uint32_t kInterviewRetryMs = 60000;
	/// An announce heard this shortly before the trust center's join notice belongs to that join.
	static constexpr uint64_t kAnnounceBeforeJoinMs = 5000;
	static constexpr uint32_t kCheckPeriodMs = 10000;
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
	/// Asks the device to leave, deletes it from the database and reloads the datapoints.
	bool remove(Ieee ieee, const char** error = nullptr);
	/// Makes the device identify itself for kIdentifySeconds.
	bool identify(Ieee ieee, const char** error = nullptr);
	/// Sets the device's label (at most ZigbeeDevice::kMaxText bytes; empty clears it) and writes the
	/// database.
	bool set_label(Ieee ieee, const char* label, const char** error = nullptr);
	/// Interviews a known device again (after a firmware update changed its endpoints).
	bool interview(Ieee ieee, const char** error = nullptr);

	// ---- Platform events (on the event loop) ----

	void network_state(bool up, uint16_t pan_id, uint8_t channel, Ieee extended_pan);
	/// The stack's permit-join state: open for `seconds`, 0 closed.
	void permit_join_status(uint8_t seconds);
	/// The trust center accepted a fresh association of the device (an unsecured join, not a
	/// rejoin): a new device, or a known one after a factory reset, whose bindings and reporting
	/// configuration are gone; it is interviewed again with the announce that follows.
	void device_joined(NodeAddress address);
	void device_announced(NodeAddress address);
	void device_left(NodeAddress address);
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
	/// left, interviewing, last_seen, seen_ago_s, missing, incomplete, datapoints: [names]}]} for the
	/// console, the API and E-Home's pairing view (spec/zigbee.md; `missing` and `incomplete` only for
	/// an incomplete interview).
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
