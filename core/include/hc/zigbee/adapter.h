/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ZIGBEE_ADAPTER_H
#define HC_ZIGBEE_ADAPTER_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/event_loop.h"
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
///   learnt stays uninterviewed and is tried again when it is heard after kInterviewRetryMs. Missing
///   names or zone types do not fail it. On success the database is written, the reporting of the
///   mapped attributes is configured, their clusters bound to the coordinator, the datapoints loaded.
/// - **Datapoints.** Until the runtime image of issue #8 exists, the adapter loads the model itself
///   (load refused while the model holds datapoints of another owner, like the demo's) and reloads it
///   when a device is interviewed or removed; values survive the reload.
/// - **Stale.** Every kCheckPeriodMs the adapter compares each device's silence with its stale
///   timeout: kStaleFactor times the largest maximum reporting interval of its datapoints (a device
///   without configured reporting, a pure button, never goes stale). Silence counts from the start
///   for a device not heard since. Any frame of a stale device ends it; its datapoints are read
///   again.
/// - **Reads.** After start() and after a silence, the server attributes of each device are read,
///   one device every kRefreshSpacingMs.
///
/// Memory: start(), configure() and the reload allocate the point table, the device database is
/// static (DeviceDatabase::kMaxDevices); reports, commands and the stale check allocate nothing.
/// Threading: the event loop's thread; the platform posts its events there.
class ZigbeeAdapter : public Technology {
public:
	static constexpr uint32_t kInterviewTimeoutMs = 10000;
	static constexpr uint8_t kInterviewAttempts = 3;
	static constexpr uint32_t kInterviewRetryMs = 60000;
	static constexpr uint32_t kCheckPeriodMs = 10000;
	static constexpr uint32_t kStaleFactor = 3;
	static constexpr uint32_t kRefreshSpacingMs = 500;
	/// Transition of the level and colour temperature commands, in tenths of a second.
	static constexpr uint16_t kTransitionDs = 5;
	static constexpr uint16_t kIdentifySeconds = 10;
	/// Longest permit-join time the stack accepts (255 would mean "forever" in Zigbee 2006).
	static constexpr uint32_t kMaxPermitJoinS = 254;
	/// Endpoints of one device the interview asks for (it keeps ZigbeeDevice::kMaxEndpoints).
	static constexpr size_t kMaxQueriedEndpoints = 8;
	static constexpr size_t kMaxOverrides = 512;

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

	/// Replaces the runtime image's device bindings (copied). Checked before anything changes: every
	/// binding has a name and maps a known function (override_point()), no name and no
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
	/// The device's stale timeout in seconds (0: never stale).
	uint32_t stale_timeout_s(Ieee ieee) const;
	/// The device being interviewed, 0 if none.
	Ieee interviewing() const { return interview_.active ? interview_.ieee : 0; }
	/// Whether the datapoints are in the model (start() or the last reload succeeded).
	bool loaded() const { return loaded_; }

	/// {"network": {...}, "devices": [{ieee, short, manufacturer, model, label, interviewed, stale,
	/// last_seen, datapoints: [names]}]} for the console, the API and E-Home's pairing view
	/// (spec/zigbee.md).
	void write_json(JsonWriter& json) const;

	// Technology
	const char* name() const override { return "zigbee"; }
	bool command(const Datapoint& datapoint, const Value& value) override;

private:
	enum class Step : uint8_t { active_endpoints, simple_descriptor, basic, zone };

	struct Interview {
		bool active = false;
		Ieee ieee = 0;
		Step step = Step::active_endpoints;
		uint8_t attempts = 0;
		uint8_t index = 0;
		uint8_t queried[kMaxQueriedEndpoints] = {};
		uint8_t queried_count = 0;
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
	void fail_interview(const char* reason);
	void end_interview();
	void start_pending_interview();
	void setup_reporting(const ZigbeeDevice& device);
	bool rebuild(const char** error);
	bool save_database();
	void check_stale();
	void refresh_due();
	void schedule_refresh();
	void report_point(const ZigbeeDevice& device, uint8_t endpoint, Function function, const Value& value);
	long find_point(Ieee ieee, uint8_t endpoint, Function function) const;
	bool device_has_overrides(Ieee ieee) const;
	void free_overrides();
	const char* device_text(const ZigbeeDevice& device, char* buffer, size_t size) const;

	EventLoop& loop_;
	const Clock& clock_;
	DatapointModel& model_;
	ZigbeeDriver& driver_;
	DeviceDatabase database_;
	NetworkStatus network_;
	Interview interview_;
	ZigbeePoint* points_ = nullptr;
	size_t point_count_ = 0;
	ZigbeePoint* overrides_ = nullptr; ///< The image's bindings, names in override_names_.
	size_t override_count_ = 0;
	char* override_names_ = nullptr;
	bool persistent_;
	bool started_ = false;
	bool loaded_ = false;
	uint64_t start_ms_ = 0;
	TimerHandle check_timer_;
	TimerHandle refresh_timer_;
};

} // namespace zigbee
} // namespace hc

#endif // HC_ZIGBEE_ADAPTER_H
