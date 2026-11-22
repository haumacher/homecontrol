/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ZIGBEE_DEVICE_DATABASE_H
#define HC_ZIGBEE_DEVICE_DATABASE_H

#include <cstddef>
#include <cstdint>

#include "hc/json.h"
#include "hc/platform/blob_store.h"
#include "hc/zigbee/zcl.h"

namespace hc {
namespace zigbee {

/// One application endpoint of a device as its simple descriptor describes it: the input
/// (server) and output (client) clusters. Only endpoints of the Home Automation and Light Link
/// profiles are kept (a Green Power proxy endpoint 242 is not a function of the device).
struct ZigbeeEndpoint {
	/// Clusters kept per direction. A descriptor with more keeps the ones the mapping or the
	/// interview uses first, then the others in their order while room is left.
	static constexpr size_t kMaxClusters = 12;

	/// "Not read yet" for zone_type.
	static constexpr uint16_t kNoZoneType = 0xFFFF;

	uint8_t id = 0;
	uint16_t profile = 0;
	uint16_t device = 0; ///< The application device id (0x0100 on/off light, 0x0302 temperature sensor, ...).
	uint8_t in_count = 0;
	uint8_t out_count = 0;
	uint16_t in[kMaxClusters] = {};
	uint16_t out[kMaxClusters] = {};
	/// The IAS zone type (0x0015 contact switch, 0x000D motion sensor, ...) of an endpoint with an
	/// IAS Zone server, read during the interview.
	uint16_t zone_type = kNoZoneType;

	bool has_in(uint16_t cluster) const;
	bool has_out(uint16_t cluster) const;

	/// Sets the cluster lists from a descriptor (the selection rule above). Returns the number of
	/// clusters dropped for lack of room.
	size_t set_clusters(const uint16_t* in_clusters, size_t in_total, const uint16_t* out_clusters, size_t out_total);
};

/// A device of the network: identity, the interview result and what the controller adds (label,
/// last seen). The network key and the stack's own tables (neighbours, addresses, bindings) are kept
/// by the Zigbee stack in its zb_storage partition, not here.
struct ZigbeeDevice {
	static constexpr size_t kMaxEndpoints = 4;
	/// Longest manufacturer, model and label text in bytes.
	static constexpr size_t kMaxText = 32;
	/// Endpoints an incomplete interview records as unanswered (the interview asks for at most 8).
	static constexpr size_t kMaxMissing = 8;

	Ieee ieee = 0;
	uint16_t short_addr = kNoShortAddress;
	char manufacturer[kMaxText + 1] = {};
	char model[kMaxText + 1] = {};
	/// A name for people (console `zigbee label`); empty: none. Datapoint names do not use it, so
	/// renaming a device never renames its datapoints.
	char label[kMaxText + 1] = {};
	uint8_t endpoint_count = 0;
	ZigbeeEndpoint endpoints[kMaxEndpoints];
	/// The interview completed: endpoints, clusters and the Basic cluster's names are known.
	bool interviewed = false;
	/// What the last interview asked for without an answer when it ended incomplete (a sleepy device
	/// fell asleep): the endpoints whose simple descriptor stayed unanswered, and whether the Basic
	/// cluster (manufacturer, model) did. Kept only while the device is incomplete(); persisted.
	uint8_t missing[kMaxMissing] = {};
	uint8_t missing_count = 0;
	bool missing_names = false;
	/// Whether its receiver is on while idle (unknown until an announce or a neighbour table told;
	/// homecontrol #108). Persisted; it takes a byte of the padding before `last_seen_s`.
	Receiver receiver = Receiver::unknown;
	/// Wall-clock seconds of the last frame heard while the clock was synchronized (0: never);
	/// persisted with the next write of the database, not on every frame, and at the latest
	/// ZigbeeAdapter::kSeenSaveS after it moved on (so it is at most that old after a restart).
	int64_t last_seen_s = 0;

	// The adapter's state of this boot, not persisted.

	/// `last_seen_s` as the database holds it (loaded or last written).
	int64_t saved_seen_s = 0;

	/// Monotonic time of the last frame heard since boot (0: none yet).
	uint64_t seen_ms = 0;
	/// Silent for longer than its stale timeout: its datapoints are stale.
	bool stale = false;
	/// The device announced that it left the network (it may rejoin).
	bool left = false;
	/// Its reads (and the reporting configuration not in effect) are due (after the start and after a
	/// silence).
	bool refresh = false;
	/// An interview is due as soon as none is running.
	bool interview_pending = false;
	/// The due interview is the retry of an incomplete one: it asks only for what is missing.
	bool interview_retry = false;
	// homecontrol #108, in the three bytes of padding before `interview_ms`:
	/// The refresh waits for the device's next frame (a device that may sleep, after the start).
	bool refresh_on_frame = false;
	/// The refresh was asked for by the device's own frame: it goes before the others.
	bool refresh_awake = false;
	/// Its points' reporting watches are checked shortly after its last frame (ZigbeeAdapter).
	bool watch_check = false;
	/// Monotonic time the last interview of it started, or ended incomplete (0: none since boot); the
	/// next interview the device's own frames trigger waits kInterviewRetryMs from it.
	uint64_t interview_ms = 0;

	// Radio facts and topology of this boot (homecontrol #98, spec/zigbee.md "Adapter view"), not
	// persisted; packed into 16 bytes (the flags share one byte).

	/// The device's end-device timeout in the coordinator's child table, in seconds, and the
	/// remaining time of its entry at the last read of the table, in milliseconds (valid with kChild).
	uint32_t child_timeout_s = 0;
	uint32_t child_expires_ms = 0;
	/// Its parent router (kNoShortAddress: none known; 0x0000 the coordinator), from the trust
	/// center's notice of its join or rejoin, or from a router's neighbour table (kParentListed).
	uint16_t parent_short = kNoShortAddress;
	/// Link quality (0..255) and signal strength (dBm) of the device's last frame as the coordinator's
	/// neighbour table holds them (valid with kRadioKnown).
	uint8_t lqi = 0;
	int8_t rssi = 0;
	/// From the parent router's neighbour table (valid with kParentListed): the router's link quality
	/// of the device and the device's depth in the network.
	uint8_t parent_lqi = 0;
	uint8_t depth = 0;
	/// A router's link quality of the coordinator, from its own neighbour table (valid with
	/// kCoordinatorLink).
	uint8_t coordinator_lqi = 0;
	/// kRadioKnown, kRouter, ...
	uint8_t flags = 0;

	/// `lqi` and `rssi` hold what the stack gave with a frame of the device since the start.
	static constexpr uint8_t kRadioKnown = 0x01;
	/// The device is a router (its announce's capability, or a neighbour table): it has no parent.
	static constexpr uint8_t kRouter = 0x02;
	/// The device was in the coordinator's child table at the last read of the table.
	static constexpr uint8_t kChild = 0x04;
	/// `parent_short` comes from the router's neighbour table, with `parent_lqi` and `depth`.
	static constexpr uint8_t kParentListed = 0x08;
	/// `coordinator_lqi` is known.
	static constexpr uint8_t kCoordinatorLink = 0x10;
	/// The router did not answer the last neighbour table request (logged once until it answers).
	static constexpr uint8_t kTopologySilent = 0x20;
	/// The last complete read of its parent router's table did not list it: the next one that does not
	/// either drops the parent (homecontrol #114).
	static constexpr uint8_t kParentMissed = 0x40;

	bool has(uint8_t flag) const { return (flags & flag) != 0; }
	void set(uint8_t flag, bool on) { flags = static_cast<uint8_t>(on ? flags | flag : flags & ~flag); }

	/// The interview ended incomplete: its partial result (endpoints() is not empty) is kept and
	/// mapped, the device is not interviewed and is interviewed again when it is next heard.
	bool incomplete() const { return !interviewed && endpoint_count > 0 && (missing_count > 0 || missing_names); }
	/// An interview result is in use (interviewed or incomplete): its endpoints are mapped, read and
	/// configured.
	bool has_result() const { return interviewed || incomplete(); }

	/// The endpoint with `id`, nullptr if there is none.
	const ZigbeeEndpoint* endpoint(uint8_t id) const;
	ZigbeeEndpoint* endpoint(uint8_t id);

	/// Whether the device sleeps: its receiver is known to be off while idle.
	bool sleepy() const { return receiver == Receiver::off_when_idle; }
};

/// The joined devices, persisted as JSON in the BlobStore blob kBlobName (format in spec/zigbee.md).
/// Statically sized (kMaxDevices); reading and writing the blob allocate one temporary buffer of
/// exactly the blob's size (a save measures the JSON first; a load checks the text in a first pass
/// and parses it into the table in a second, without a temporary table), which happens at start and
/// when a device joins, is interviewed, renamed or removed, and (with the adapter's next check, at
/// most one write per check) when its short address changes or its receiver is learnt; never per
/// frame.
///
/// Threading: the event loop's thread.
class DeviceDatabase {
public:
	static constexpr size_t kMaxDevices = 32;

	/// The blob (BlobStore names are flat: no directory).
	static constexpr const char* kBlobName = "zigbee-devices.json";

	/// The format number written in the blob; a reader refuses another.
	static constexpr int kFormat = 1;

	/// `store` may be nullptr: the database then lives in memory only (logged by the adapter).
	explicit DeviceDatabase(BlobStore* store);

	DeviceDatabase(const DeviceDatabase&) = delete;
	DeviceDatabase& operator=(const DeviceDatabase&) = delete;

	/// Reads the blob. A missing blob is an empty database (true). A refusal (false, the reason in
	/// `error`) leaves the database as it was.
	bool load(const char** error = nullptr);

	/// Writes the blob (atomic replace by the store). False with `error` when the JSON does not fit
	/// BlobStore::kMaxBlobSize or the store refuses; the previous blob then stays.
	bool save(const char** error = nullptr);

	size_t size() const { return count_; }
	const ZigbeeDevice& at(size_t index) const { return devices_[index]; }
	ZigbeeDevice& at(size_t index) { return devices_[index]; }

	ZigbeeDevice* find(Ieee ieee);
	const ZigbeeDevice* find(Ieee ieee) const;
	ZigbeeDevice* find_short(uint16_t short_addr);

	/// Adds a device (not interviewed) or returns the existing one with the new short address.
	/// nullptr with `error` when the database is full or the address is reserved.
	ZigbeeDevice* add(Ieee ieee, uint16_t short_addr, const char** error = nullptr);

	/// Removes a device; false if it is unknown.
	bool remove(Ieee ieee);

	/// Removes all devices (not the blob).
	void clear() { count_ = 0; }

	/// Writes the database object (spec/zigbee.md) to `json`.
	void write_json(JsonWriter& json) const;

	/// Replaces the content by the database object in `text`. Every device is checked before
	/// anything changes; a refusal (false, the reason in `error`) keeps the current content.
	bool read_json(const char* text, size_t length, const char** error = nullptr);

private:
	BlobStore* store_;
	ZigbeeDevice devices_[kMaxDevices];
	size_t count_ = 0;
};

/// The member "missing": {"endpoints": [ids], "names": bool} of an incomplete() device into an open
/// object (the database, the adapter view and the export write the same, spec/zigbee.md).
void write_missing_json(JsonWriter& json, const ZigbeeDevice& device);

/// Copies `text` into a field of `size` bytes, cutting at a UTF-8 character boundary.
void copy_text(char* field, size_t size, const char* text);

} // namespace zigbee
} // namespace hc

#endif // HC_ZIGBEE_DEVICE_DATABASE_H
