/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_HISTORY_H
#define HC_HISTORY_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/platform/blob_store.h"
#include "hc/platform/clock.h"

namespace hc {

/// What the runtime image declares about the history of one datapoint (flagged
/// DatapointFlags::kHistory): how far back the ring reaches and how dense it samples.
struct HistorySpec {
	uint16_t datapoint;   ///< Id in the loaded DatapointModel.
	uint32_t retention_s; ///< How far back the ring reaches: spacing_s .. History::kMaxRetentionS.
	uint32_t spacing_s;   ///< Minimum distance of two samples, also the keepalive period:
	                      ///< History::kMinSpacingS .. History::kMaxSpacingS.
};

/// Outcome of History calls. Everything except `ok` is a refusal; a refused load keeps the previous
/// rings and logs the reason. history_status_name() gives the text.
enum class HistoryStatus : uint8_t {
	ok,
	malformed,         ///< Spacing or retention out of range, too many samples, a datapoint twice, a bad query.
	unknown_datapoint, ///< No datapoint with this id, or (query) no history for it.
	type_error,        ///< The datapoint is not boolean, unsigned, signed or real.
	flag_mismatch,     ///< A spec for a datapoint without kHistory, or a kHistory datapoint without a spec.
	budget_exceeded,   ///< The rings need more than the image's budget, or the budget exceeds the device's cap.
	out_of_memory,     ///< The rings could not be allocated (history is off until the next load).
	busy,              ///< Called from a datapoint listener.
	capacity,          ///< No listener or timer slot left.
};

/// "ok", "malformed", "unknown datapoint", ...
const char* history_status_name(HistoryStatus status);

/// One sample in a ring and on flash: the wall-clock time and the value's 32 bits (boolean 0/1,
/// unsigned and signed integers as they are, a real as an IEEE 754 single).
struct HistorySample {
	uint32_t t;    ///< UTC seconds.
	uint32_t bits;
};

static_assert(sizeof(HistorySample) == 8, "a history sample is 8 bytes");

/// One point of a range() result: a single sample, or the decimation of the samples of a bucket.
struct HistoryPoint {
	int64_t t;      ///< Start of the bucket (UTC seconds); the sample's time for a single sample.
	int64_t last_t; ///< Time of the bucket's last sample.
	double min;
	double max;
	double last;    ///< The value of the bucket's last sample.
	uint32_t count; ///< Samples in the bucket.
	double mean;    ///< The mean of the bucket's samples (for a boolean the share of samples that are on).
};

/// The limits the platform sets for the history.
struct HistoryLimits {
	/// The most sample memory an image may ask for (the C6 default: 64 KB).
	size_t cap_bytes = 65536;
	/// Rings with new samples are saved at most this often ...
	uint32_t min_save_interval_s = 900;
	/// ... and at most this many sample bytes per hour on average (flash wear, doc/persistence.md):
	/// the save interval is max(min_save_interval_s, sample bytes * 3600 / save_bytes_per_hour).
	size_t save_bytes_per_hour = 65536;
};

/// Short-term history of numeric and boolean datapoints for the UI's sparklines
/// (doc/persistence.md): per datapoint flagged kHistory, a ring of (time, value) samples in RAM,
/// allocated at load from the budget the image states (capped by the platform).
///
/// - Sampling: a changed good value is sampled at once if the last sample is at least spacing_s
///   old, otherwise by the tick (every kTickMs) when it is; the tick also adds a keepalive sample
///   of the current value when the last is spacing_s old, so a flat line has points. No samples
///   while the value is not good (a gap) or the wall clock is not synchronized.
/// - Retention: samples older than retention_s are dropped and never returned.
/// - range() decimates into at most max_points buckets with min, max and last.
/// - Rings with new samples are saved every save_interval_s() (from the HistoryLimits) and by
///   flush() (orderly shutdown), one blob per ring ("h." + 8 hex digits of the FNV-1a hash of the name); load() restores them.
///
/// Memory: load() allocates three blocks (the samples, the ring table, the names); nothing in
/// steady state. Threading: the event loop's thread.
class History : public DatapointListener {
public:
	static constexpr uint32_t kMinSpacingS = 10;
	static constexpr uint32_t kMaxSpacingS = 86400;
	static constexpr uint32_t kMaxRetentionS = 31 * 86400;
	/// Most samples in one ring (one ring fits a blob).
	static constexpr uint32_t kMaxSamples = 2048;
	/// Period of the sampling tick.
	static constexpr uint32_t kTickMs = 5000;
	/// Format generation of the ring blob (the third byte); a reader refuses a higher one.
	static constexpr uint8_t kFormatVersion = 1;

	History(EventLoop& loop, const Clock& clock, DatapointModel& model, BlobStore& store,
		const HistoryLimits& limits = HistoryLimits());
	~History();

	History(const History&) = delete;
	History& operator=(const History&) = delete;

	/// The samples of a ring: retention / spacing rounded up, plus one (a ring spans its retention).
	static uint32_t ring_samples(uint32_t retention_s, uint32_t spacing_s);

	/// The sample memory of a ring in bytes (what counts against the budget): 8 per sample.
	static size_t ring_bytes(uint32_t retention_s, uint32_t spacing_s) {
		return static_cast<size_t>(ring_samples(retention_s, spacing_s)) * sizeof(HistorySample);
	}

	/// Replaces the rings by one per spec, for the model's current datapoints. Validates everything
	/// first (a refusal keeps the previous rings); `budget_bytes` is the image's sample budget, at
	/// most HistoryLimits::cap_bytes. Saves the previous rings' new samples, allocates, restores the
	/// saved rings, deletes rings no longer in the image. The first call registers the listener and
	/// starts the tick and the save timer.
	HistoryStatus load(const HistorySpec* specs, size_t count, size_t budget_bytes);

	/// Saves every ring with new samples now.
	void flush();

	/// The samples of a datapoint with from <= t < to (and within the retention), decimated to at
	/// most `max_points` points: when there are more samples, the range is split into max_points
	/// buckets of equal width (whole seconds) and each non-empty bucket gives one point with the
	/// minimum, maximum and last value; otherwise every sample is a point. `count` receives the
	/// number of points. Refused with unknown_datapoint (no history for `id`) or malformed
	/// (to <= from, max_points 0).
	HistoryStatus range(uint16_t id, int64_t from, int64_t to, HistoryPoint* out, size_t max_points,
		size_t& count) const;

	/// Samples in the ring of a datapoint (0 without history).
	size_t samples(uint16_t id) const;

	/// How far back the ring of a datapoint reaches, in seconds (its retention; 0 without history).
	uint32_t retention_s(uint16_t id) const;

	/// Number of rings.
	size_t size() const { return count_; }

	/// Sample memory of the loaded rings in bytes.
	size_t bytes() const { return sample_bytes_; }

	/// Ring blobs written since construction (successful writes).
	uint32_t writes() const { return writes_; }

	/// The save interval of the loaded rings in seconds (HistoryLimits).
	uint32_t save_interval_s() const { return save_interval_s_; }

	// DatapointListener
	void datapoint_updated(const DatapointUpdate& update) override;

private:
	struct Ring;

	HistoryStatus validate(const HistorySpec* specs, size_t count, size_t budget_bytes, size_t& bytes) const;
	HistoryStatus start(uint32_t save_interval_s);
	Ring* ring(uint16_t id) const;
	Ring* ring_of(const Datapoint& datapoint) const;
	void append(Ring& ring, uint32_t t, uint32_t bits);
	bool save(Ring& ring);
	void restore(Ring& ring);
	void on_tick();
	void free_rings();

	EventLoop& loop_;
	const Clock& clock_;
	DatapointModel& model_;
	BlobStore& store_;
	HistoryLimits limits_;
	Ring* rings_ = nullptr;
	HistorySample* samples_ = nullptr;
	char* names_ = nullptr;
	size_t count_ = 0;
	size_t sample_bytes_ = 0;
	TimerHandle tick_;
	TimerHandle save_timer_;
	bool listening_ = false;
	bool failure_logged_ = false;
	uint32_t writes_ = 0;
	uint32_t save_interval_s_ = 0;
};

} // namespace hc

#endif // HC_HISTORY_H
