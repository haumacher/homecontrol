/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_TELEGRAM_LOG_H
#define HC_KNX_TELEGRAM_LOG_H

#include <cstddef>
#include <cstdint>

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

/// Told about every record a TelegramLog stores (the monitor stream, hc::MonitorFeed). Called on the
/// event loop's thread right after the record was stored.
class TelegramListener {
public:
	/// `record` is the stored copy (with its times); `sequence` its sequence number.
	virtual void telegram_logged(const TelegramRecord& record, uint64_t sequence) = 0;

protected:
	~TelegramListener() = default;
};

/// The tap of the group telegrams (issue #19): the KNX adapter appends every telegram it saw
/// (received, and sent once confirmed), the log stamps it with the monotonic and wall time, counts
/// it and hands it to its listener, the monitor stream (hc::MonitorFeed), whose ring is where they
/// are kept: the console's `knx log` reads them there (issue #45 item 10: the log's own ring of 64
/// records, 3 KB, held the same telegrams a second time). Every record gets a sequence number (0,
/// 1, 2, ...).
///
/// Threading: called on the event loop's thread.
class TelegramLog {
public:
	explicit TelegramLog(const Clock& clock) : clock_(clock) {}

	/// Stamps `record` with the current monotonic and wall time and hands it to the listener.
	void append(const TelegramRecord& record);

	/// Records appended since construction (or clear()): the sequence number the next one gets.
	uint64_t total() const { return total_; }

	void clear() { total_ = 0; }

	/// The one listener (nullptr: none); it must outlive the log or be removed first.
	void set_listener(TelegramListener* listener) { listener_ = listener; }
	TelegramListener* listener() const { return listener_; }

private:
	const Clock& clock_;
	uint64_t total_ = 0;
	TelegramListener* listener_ = nullptr;
};

} // namespace knx
} // namespace hc

#endif // HC_KNX_TELEGRAM_LOG_H
