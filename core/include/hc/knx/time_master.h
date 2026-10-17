/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_TIME_MASTER_H
#define HC_KNX_TIME_MASTER_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/dpt.h"
#include "hc/event_loop.h"
#include "hc/knx/adapter.h"
#include "hc/knx/address.h"
#include "hc/technology.h"
#include "hc/time_service.h"

namespace hc {
namespace knx {

/// What the KNX time master broadcasts and when (doc/knx.md, "Time master"). Programmatic now; the
/// runtime image loader (issue #8) fills it.
struct TimeMasterConfig {
	static constexpr uint32_t kDefaultIntervalS = 600;
	static constexpr uint32_t kMinIntervalS = 60;
	static constexpr uint32_t kMaxIntervalS = 86400;

	/// One group address the time goes to, and the datapoint that shows what was sent.
	struct Output {
		bool enabled = false;
		GroupAddress address;
		/// The datapoint's name in the model; nullptr: the default ("knx.time", "knx.date",
		/// "knx.datetime").
		const char* datapoint = nullptr;
	};

	Output time;      ///< DPT 10.001: time of day with the day of the week.
	Output date;      ///< DPT 11.001: the date.
	Output date_time; ///< DPT 19.001: date and time with the clock's flags.

	/// Seconds between two broadcasts, kMinIntervalS..kMaxIntervalS. The broadcasts align to the
	/// local wall clock: when the local time counted in seconds is a multiple of the interval
	/// (every 10 minutes at :00, :10, ...).
	uint32_t interval_s = kDefaultIntervalS;
	/// Also broadcast at local midnight (matters when the interval does not divide a day).
	bool at_midnight = true;
	/// Also broadcast as soon as the clock is synchronized (and after configure() if it already is).
	bool on_synchronized = true;
};

/// The KNX time master (issue #9): broadcasts the controller's local time to the bus so KNX devices
/// (thermostats, displays) stay in sync.
///
/// It owns one datapoint per configured address (a Technology that refuses every command) and
/// publishes the time to it (DatapointModel::publish()); the KnxAdapter is the datapoint's
/// transmitter through a transmit-only mapping (mappings()), frames the group write and answers
/// GroupValueRead from the last published value. One way to the bus.
///
/// Sends nothing while the clock is not synchronized. Broadcasts on synchronization, at every
/// aligned interval tick, at local midnight, after a clock jump and after a UTC offset change (a
/// DST transition) that no broadcast has shown yet. When synchronization is lost it stops and
/// marks its datapoints stale.
///
/// Wiring (the image loader of #8): load the model; configure the adapter with the image's
/// mappings; configure() the time master (it checks its addresses against them); configure the
/// adapter again with the image's mappings plus mappings(); register the time master with the time
/// service for kTimeEvents.
///
/// Memory: inside the object; nothing is allocated. Threading: the event loop's thread.
class TimeMaster : public Technology, public TimeListener {
public:
	/// The time events the time master needs (TimeService::add_listener()).
	static constexpr uint8_t kTimeEvents = TimeEvents::kSynchronized | TimeEvents::kSynchronizationLost |
		TimeEvents::kClockJumped | TimeEvents::kDayChanged | TimeEvents::kOffsetChanged;

	/// Longest wait of the tick timer: the wall clock is re-read at least this often, so a drift
	/// between the wall clock and the monotonic base cannot delay a broadcast by more than its drift
	/// over this period.
	static constexpr uint32_t kMaxTimerMs = 600000;

	/// Configured addresses at most (time, date, date and time).
	static constexpr size_t kMaxOutputs = 3;

	TimeMaster(EventLoop& loop, const TimeSource& time, DatapointModel& model, const KnxAdapter& adapter);
	~TimeMaster();

	TimeMaster(const TimeMaster&) = delete;
	TimeMaster& operator=(const TimeMaster&) = delete;

	/// Takes a configuration. Checks everything before anything changes: the interval within
	/// range; per enabled output a datapoint of that name in the model with the DPT's value type
	/// and kTransmit, not owned by another technology and transmitted by nobody but the adapter; no
	/// address or datapoint twice; no address another datapoint of the adapter sends on. A refusal
	/// (false, the reason in `error` and in the log) keeps the previous configuration. Binds the
	/// datapoints to the time master; when the clock is synchronized, the first broadcast follows
	/// on the next run of the loop (after the caller configured the adapter).
	bool configure(const TimeMasterConfig& config, const char** error = nullptr);

	/// Stops broadcasting and forgets the outputs (their datapoints stay bound until the next model
	/// load; their commands remain refused).
	void clear();

	/// The transmit-only mappings of the configured outputs for the adapter's configure(): writes
	/// at most `capacity` (kMaxOutputs is enough) and returns how many it wrote.
	size_t mappings(KnxMapping* out, size_t capacity) const;

	/// Configured outputs.
	size_t size() const { return count_; }

	/// Whether it broadcasts now: outputs configured and the clock synchronized.
	bool broadcasting() const { return tick_.valid(); }

	/// UTC seconds of the next aligned tick (0 while not broadcasting).
	int64_t next_tick_utc() const { return tick_.valid() ? target_utc_ : 0; }

	/// UTC seconds of the last broadcast (0 before the first).
	int64_t last_broadcast_utc() const { return last_utc_; }

	// Technology
	const char* name() const override { return "knx time master"; }
	bool command(const Datapoint& datapoint, const Value& value) override;

	// TimeListener
	void time_changed(uint8_t events) override;

private:
	enum class Kind : uint8_t { time, date, date_time };

	struct Output {
		Kind kind;
		uint16_t datapoint;
		GroupAddress address;
	};

	void kick();
	void tick();
	void arm();
	void broadcast(const char* why);
	void stop();

	EventLoop& loop_;
	const TimeSource& time_;
	DatapointModel& model_;
	const KnxAdapter& adapter_;
	Output outputs_[kMaxOutputs] = {};
	size_t count_ = 0;
	uint32_t interval_s_ = TimeMasterConfig::kDefaultIntervalS;
	bool at_midnight_ = true;
	bool on_synchronized_ = true;
	TimerHandle tick_;
	TimerHandle kick_;
	int64_t target_utc_ = 0;
	int64_t last_utc_ = 0;
	Date last_date_;           ///< Local date of the last broadcast.
	int32_t last_offset_ = 0;  ///< UTC offset of the last broadcast.
};

} // namespace knx
} // namespace hc

#endif // HC_KNX_TIME_MASTER_H
