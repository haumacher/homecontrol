/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_VALUE_STORE_H
#define HC_VALUE_STORE_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/platform/blob_store.h"
#include "hc/platform/clock.h"

namespace hc {

/// Helpers shared by the persisted formats of ValueStore and History (doc/persistence.md).
namespace persist {

/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) continued from `crc` (start with 0).
uint32_t crc32(uint32_t crc, const void* data, size_t size);

/// FNV-1a, 32 bit, of a NUL-terminated string.
uint32_t fnv1a(const char* text);

/// The blob name of a datapoint: `prefix` followed by the 8 lower-case hex digits of the FNV-1a hash
/// of its name ("v.1a2b3c4d"). `buffer` holds at least kBlobNameSize bytes; `prefix` is at most
/// 4 characters.
constexpr size_t kBlobNameSize = 13;
void blob_name(const char* prefix, const char* datapoint_name, char* buffer);

/// Decides which blobs of a prefix stay (delete_orphans()).
class BlobKeeper {
public:
	/// Whether the blob `name` (which starts with the prefix) belongs to the current image.
	virtual bool keep(const char* name) const = 0;

protected:
	~BlobKeeper() = default;
};

/// Deletes every blob whose name starts with `prefix` and that `keeper` does not keep; logs each
/// deletion under `tag`. Returns the number deleted.
size_t delete_orphans(BlobStore& store, const char* prefix, const BlobKeeper& keeper, const char* tag);

} // namespace persist

/// The rate limits of a ValueStore; the defaults bound the flash wear (doc/persistence.md).
struct ValueStoreLimits {
	/// A datapoint's record is written at most once per this interval; changes in between are
	/// coalesced into the next write (the latest value wins).
	uint32_t min_interval_s = 60;
	/// The global budget: writes per hour on average, also the burst (a token bucket that refills
	/// continuously). A write that finds the bucket empty waits.
	uint32_t writes_per_hour = 60;
	/// A repeated report of an unchanged value re-writes the record (for its time: "last known 3 h
	/// ago") only when the saved time is at least this old.
	uint32_t refresh_s = 3600;
};

/// Keeps the last good value of every datapoint flagged DatapointFlags::kPersist, with its
/// wall-clock time, across a reboot (doc/persistence.md):
///
/// - Listens to the model; a changed good value (report or command) makes the datapoint's record
///   pending. A pending record is written when the datapoint's last write is at least
///   `min_interval_s` old and the hourly budget has a token: at once (posted to the event loop) if
///   both hold, otherwise from the store's tick (every kTickMs). One blob per datapoint, named
///   "v." + 8 hex digits of the FNV-1a hash of its name.
/// - load(), called right after the model is loaded, restores each persisted datapoint's record
///   with DatapointModel::restore(): the value with quality stale (the bus has not confirmed it),
///   its saved wall-clock time available from wall_time(). Records of datapoints no longer
///   persisted are deleted.
/// - flush() writes everything pending at once (orderly shutdown).
///
/// The store keeps its own copy of each persisted datapoint's name and last value, so a pending
/// record of the previous image is still written by the next load().
///
/// Memory: load() allocates one block (the slots and the names); nothing in steady state.
/// Threading: the event loop's thread.
class ValueStore : public DatapointListener {
public:
	/// Period of the tick that writes the records whose interval has passed.
	static constexpr uint32_t kTickMs = 5000;

	/// Largest record in bytes.
	static constexpr size_t kMaxRecord = 128;

	/// Format generation of the record (the third byte); a reader refuses a higher one.
	static constexpr uint8_t kRecordVersion = 1;

	ValueStore(EventLoop& loop, const Clock& clock, DatapointModel& model, BlobStore& store,
		const ValueStoreLimits& limits = ValueStoreLimits());
	~ValueStore();

	ValueStore(const ValueStore&) = delete;
	ValueStore& operator=(const ValueStore&) = delete;

	/// Takes over the model's current datapoints: writes what is pending from the previous load,
	/// allocates a slot for every kPersist datapoint, restores their records, deletes orphaned
	/// records. The first call registers the store as a listener and starts its tick. Refused with
	/// busy (from a listener), capacity (no listener slot) or out_of_memory; the reason is logged.
	DatapointStatus load();

	/// Writes every pending record now, ignoring the interval and the budget (orderly shutdown,
	/// before an image swap).
	void flush();

	/// The wall-clock time (UTC seconds) of a persisted datapoint's current value: the saved time of
	/// a restored value, the time of the last update otherwise. Empty when unknown (never had a
	/// value, the clock was not synchronized) or the datapoint is not persisted.
	std::optional<int64_t> wall_time(uint16_t id) const;

	/// Number of persisted datapoints.
	size_t size() const { return count_; }

	/// Number of records waiting to be written.
	size_t pending() const;

	/// Records written since construction (successful writes).
	uint32_t writes() const { return writes_; }

	/// The decoded content of a record.
	struct Record {
		char name[Datapoint::kMaxName + 1];
		Value value;
		std::optional<int64_t> wall_s;
	};

	/// Encodes a record (doc/persistence.md) into `buffer`; returns its length, 0 if it does not fit.
	static size_t encode(const char* name, const Value& value, std::optional<int64_t> wall_s, uint8_t* buffer,
		size_t size);

	/// Decodes a record; false with `error` naming the problem for a malformed record or an unknown
	/// format generation. Unknown fields are skipped.
	static bool decode(const uint8_t* data, size_t length, Record& out, const char** error);

	// DatapointListener
	void datapoint_updated(const DatapointUpdate& update) override;

private:
	struct Slot;

	Slot* slot(uint16_t id) const;
	void restore(Slot& slot);
	bool write(Slot& slot, uint64_t now_ms);
	void write_due();
	void on_tick();
	void on_post();
	void refill(uint64_t now_ms);
	std::optional<int64_t> resolve_wall(const Slot& slot) const;

	EventLoop& loop_;
	const Clock& clock_;
	DatapointModel& model_;
	BlobStore& store_;
	ValueStoreLimits limits_;
	Slot* slots_ = nullptr;
	char* block_ = nullptr;
	size_t count_ = 0;
	TimerHandle tick_;
	bool listening_ = false;
	bool posted_ = false;
	uint64_t tokens_ = 0;      ///< Budget in units of 1/3600000 write (a write costs kWriteCost).
	uint64_t refilled_ms_ = 0; ///< Monotonic time of the last refill.
	bool budget_logged_ = false;
	bool failure_logged_ = false;
	uint32_t writes_ = 0;
};

} // namespace hc

#endif // HC_VALUE_STORE_H
