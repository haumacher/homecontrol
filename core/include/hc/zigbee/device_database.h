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
	/// Wall-clock seconds of the last frame heard while the clock was synchronized (0: never);
	/// persisted with the next write of the database, not on every frame.
	int64_t last_seen_s = 0;

	// The adapter's state of this boot, not persisted.

	/// Monotonic time of the last frame heard since boot (0: none yet).
	uint64_t seen_ms = 0;
	/// Silent for longer than its stale timeout: its datapoints are stale.
	bool stale = false;
	/// The device announced that it left the network (it may rejoin).
	bool left = false;
	/// Its reads are due (after the start and after a silence).
	bool refresh = false;
	/// An interview is due as soon as none is running.
	bool interview_pending = false;
	/// Monotonic time the last interview of it started (0: none since boot).
	uint64_t interview_ms = 0;

	/// The endpoint with `id`, nullptr if there is none.
	const ZigbeeEndpoint* endpoint(uint8_t id) const;
	ZigbeeEndpoint* endpoint(uint8_t id);
};

/// The joined devices, persisted as JSON in the BlobStore blob kBlobName (format in spec/zigbee.md).
/// Statically sized (kMaxDevices); reading and writing the blob allocate one temporary buffer of
/// exactly the blob's size (a save measures the JSON first; a load checks the text in a first pass
/// and parses it into the table in a second, without a temporary table), which happens at start and
/// when a device joins, is interviewed, renamed or removed, never per frame.
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

/// Copies `text` into a field of `size` bytes, cutting at a UTF-8 character boundary.
void copy_text(char* field, size_t size, const char* text);

} // namespace zigbee
} // namespace hc

#endif // HC_ZIGBEE_DEVICE_DATABASE_H
