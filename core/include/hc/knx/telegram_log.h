/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_TELEGRAM_LOG_H
#define HC_KNX_TELEGRAM_LOG_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/knx/address.h"
#include "hc/knx/cemi.h"
#include "hc/platform/clock.h"

namespace hc {
namespace knx {

/// One group telegram as the controller saw it: what E-Home's group monitor shows (its
/// BusTelegram), plus the datapoint it belongs to.
struct TelegramRecord {
	static constexpr uint16_t kNoDatapoint = 0xFFFF;

	enum class Direction : uint8_t {
		received, ///< From the bus (L_Data.ind).
		sent,     ///< Sent by the controller and confirmed by the gateway (L_Data.con).
	};

	uint64_t monotonic_ms = 0;
	int64_t wall_seconds = 0;     ///< Seconds since the epoch; meaningful only if wall_synchronized.
	bool wall_synchronized = false;
	Direction direction = Direction::received;
	IndividualAddress source;
	GroupAddress destination;
	GroupService service = GroupService::other;
	Priority priority = Priority::low;
	bool repeated = false;
	uint8_t hop_count = 0;
	/// The application data: one octet with the value for a short telegram, empty for a read.
	uint8_t data[GroupData::kMaxData] = {};
	uint8_t length = 0;
	bool short_form = false;
	/// The mapped datapoint (the first one listening on the address), kNoDatapoint if none.
	uint16_t datapoint = kNoDatapoint;
};

/// The last kCapacity group telegrams, for the console (`knx log`) and E-Home's monitor (issue #19).
/// A fixed ring inside the object: no heap. Every record gets a sequence number (0, 1, 2, ...), so
/// a reader can ask for what it has not seen yet.
///
/// Threading: called on the event loop's thread.
class TelegramLog {
public:
	static constexpr size_t kCapacity = 64;

	explicit TelegramLog(const Clock& clock) : clock_(clock) {}

	/// Stores a record, stamping the current monotonic and wall time; the oldest is overwritten
	/// when the ring is full.
	void append(const TelegramRecord& record);

	/// Records held (at most kCapacity).
	size_t size() const { return count_; }

	/// Records appended since construction (or clear()): the sequence number the next one gets.
	uint64_t total() const { return total_; }

	/// The sequence number of the oldest record held.
	uint64_t first_sequence() const { return total_ - count_; }

	/// The record at `index`, 0 the oldest; nullptr outside 0..size()-1.
	const TelegramRecord* at(size_t index) const;

	/// The record with sequence number `sequence`; nullptr if it was overwritten or not written yet.
	const TelegramRecord* find(uint64_t sequence) const;

	/// Removes the datapoint references (a new mapping or model: the ids mean other datapoints).
	void forget_datapoints();

	void clear();

	/// Longest line format() writes including the NUL.
	static constexpr size_t kMaxLine = 192;

	/// Writes "<uptime s> rx 1.1.5 -> 1/2/3 write 0C 1A [name]" (the name from `model` if the record
	/// has a datapoint and `model` is given). Returns `buffer`.
	static const char* format(const TelegramRecord& record, const DatapointModel* model, char* buffer, size_t size);

private:
	const Clock& clock_;
	TelegramRecord records_[kCapacity];
	size_t head_ = 0; ///< Index of the oldest record.
	size_t count_ = 0;
	uint64_t total_ = 0;
};

} // namespace knx
} // namespace hc

#endif // HC_KNX_TELEGRAM_LOG_H
