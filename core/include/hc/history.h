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
#include "hc/platform/time_zone.h"

namespace hc {

/// One consolidated archive of a datapoint's history (issue #56): a slot per `resolution_s`,
/// kept for `retention_s`. A slot holds the minimum, maximum and last value, the time-weighted sum
/// and the sample count of its interval (ArchiveSlot).
struct ArchiveSpec {
	uint32_t resolution_s; ///< History::kMinResolutionS .. kMaxResolutionS, larger than the raw spacing.
	uint32_t retention_s;  ///< resolution_s .. History::kMaxArchiveRetentionS, at most kMaxSlots slots.
};

/// What the runtime image declares about the history of one datapoint (flagged
/// DatapointFlags::kHistory): how far back the raw ring reaches and how dense it samples, and the
/// archives that consolidate it for longer ranges.
struct HistorySpec {
	uint16_t datapoint;   ///< Id in the loaded DatapointModel.
	uint32_t retention_s; ///< How far back the ring reaches: spacing_s .. History::kMaxRetentionS.
	uint32_t spacing_s;   ///< Minimum distance of two samples, also the keepalive period:
	                      ///< History::kMinSpacingS .. History::kMaxSpacingS.
	const ArchiveSpec* archives = nullptr; ///< `archive_count` archives (load() copies them).
	uint8_t archive_count = 0;             ///< 0 .. History::kMaxArchives.
};

/// Outcome of History calls. Everything except `ok` is a refusal; a refused load keeps the previous
/// rings and logs the reason. history_status_name() gives the text.
enum class HistoryStatus : uint8_t {
	ok,
	malformed,         ///< Spacing or retention out of range, too many samples, a datapoint twice, a bad query.
	unknown_datapoint, ///< No datapoint with this id, or (query) no history for it.
	type_error,        ///< The datapoint is not boolean, unsigned, signed or real.
	flag_mismatch,     ///< A spec for a datapoint without kHistory, or a kHistory datapoint without a spec.
	budget_exceeded,   ///< The rings and archives need more RAM or flash than the image's budgets, or a
	                   ///< budget exceeds the device's cap.
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

/// One slot of an archive (issue #56), in RAM and on flash: 32 bytes without padding, which is what
/// E-Home counts. Empty (nothing fed) when `count` and `held_s` are 0.
struct ArchiveSlot {
	double sum;      ///< The integral of the value over the held seconds (for a boolean the seconds on).
	uint32_t start;  ///< The slot's start, UTC seconds.
	float min;       ///< Smallest value the slot saw (samples and the value held into it).
	float max;
	float last;      ///< The value at the slot's end (or the latest, for the open slot).
	uint32_t count;  ///< Samples fed (keepalives included).
	uint32_t held_s; ///< Seconds the slot had a good value: the mean is sum / held_s.
};

static_assert(sizeof(ArchiveSlot) == 32, "an archive slot is 32 bytes");

/// One bucket of a query(): the consolidation of the samples or archive slots whose time falls in
/// it (spec/api.md "history").
struct HistoryBucket {
	int64_t t;        ///< The bucket's start (UTC seconds); the sample's time when `single`.
	double last;
	double min;
	double max;
	double mean;      ///< Time-weighted over the held seconds (the sample's value when none was held).
	double on_s;      ///< A boolean's seconds on in the bucket (the time-weighted sum).
	uint32_t count;   ///< Samples (raw: in the bucket; archive: fed into its slots).
	bool single;      ///< A raw bucket of one sample and no held time: the point [t, value].
};

/// Receives the buckets of History::query(), oldest first.
class HistorySink {
public:
	/// Returns false to stop the query (the response is full).
	virtual bool bucket(const HistoryBucket& bucket) = 0;

protected:
	~HistorySink() = default;
};

/// Which store answered a History::query().
struct HistoryAnswer {
	uint32_t resolution_s = 0; ///< 0: the raw ring; otherwise the archive of this resolution.
	uint32_t retention_s = 0;  ///< How far back that store reaches.
	bool boolean = false;      ///< The datapoint is a boolean (its buckets carry on_s).
};

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
	/// The most RAM an image may ask for (`history.budget`): the raw rings' samples and the
	/// archives' open and pending slots (History::ram_bytes()). The C6 default: 64 KB.
	size_t cap_bytes = 65536;
	/// The most flash an image may ask for (`history.archiveBudget`): the archives' segments
	/// (History::archive_flash_bytes()). The C6 default: 64 KB, 16 of the storage partition's blocks.
	size_t archive_cap_bytes = 65536;
	/// Rings with new samples (and archives with new slots) are saved at most this often ...
	uint32_t min_save_interval_s = 900;
	/// ... and at most this many bytes per hour on average (flash wear, doc/persistence.md): the save
	/// interval is max(min_save_interval_s, (sample bytes + 4096 per archive) * 3600 / save_bytes_per_hour).
	size_t save_bytes_per_hour = 65536;
};

namespace history_detail {
enum class SegmentState : uint8_t;
}

/// Short-term history of numeric and boolean datapoints for the UI's sparklines and charts
/// (doc/persistence.md): per datapoint flagged kHistory, a ring of (time, value) samples in RAM,
/// allocated at load from the budget the image states (capped by the platform), and up to four
/// consolidated archives whose slots live on flash (issue #56).
///
/// - Sampling: a changed good value is sampled at once if the last sample is at least spacing_s
///   old, otherwise by the tick (every kTickMs) when it is; the tick also adds a keepalive sample
///   of the current value when the last is spacing_s old, so a flat line has points. No samples
///   while the value is not good (a gap) or the wall clock is not synchronized.
/// - Retention: samples older than retention_s are dropped and never returned.
/// - range() decimates into at most max_points buckets with min, max and last; query() answers
///   the API from the store that fits the range (the ring or an archive).
/// - Archives: every sample also feeds the open slot of each archive of its datapoint; a sample's
///   value is held until the next sample, at most spacing_s + kHoldSlackS (beyond that the value
///   was not good: a gap), which gives the time-weighted sum. A slot is aligned to UTC multiples of
///   its resolution, or, for a resolution of whole days, to the local days of the time zone. Closed
///   slots wait in RAM (at most one hour of them) until the next save writes them into the
///   archive's segments on flash (blobs of kSlotsPerSegment slots, patched in place); the open
///   slot is written too, and taken up again from flash when it opens after a restart or a swap.
/// - Rings with new samples are saved every save_interval_s() (from the HistoryLimits) and by
///   flush() (orderly shutdown), one blob per ring ("h." + 8 hex digits of the FNV-1a hash of the
///   name); load() restores them. Archive segments are "a." + the hash + "." + the resolution and
///   the segment index in hex.
///
/// Memory: load() allocates five blocks (the samples, the ring table, the names, the archive
/// table, the archives' pending slots); nothing in steady state. Threading: the event loop's thread.
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

	/// Most archives of one datapoint.
	static constexpr uint8_t kMaxArchives = 4;
	static constexpr uint32_t kMinResolutionS = 60;
	static constexpr uint32_t kMaxResolutionS = 31 * 86400;
	/// Ten years of 365 days.
	static constexpr uint32_t kMaxArchiveRetentionS = 3650u * 86400u;
	/// Most slots of one archive.
	static constexpr uint32_t kMaxSlots = 2048;
	/// Slots of one segment blob: with its 32-byte header exactly 4096 bytes, one LittleFS block.
	static constexpr uint32_t kSlotsPerSegment = 127;
	static constexpr size_t kSegmentHeaderBytes = 32;
	static constexpr size_t kSegmentBytes = kSegmentHeaderBytes + kSlotsPerSegment * sizeof(ArchiveSlot);
	/// Format generation of the segment blob (the third byte); a reader drops a higher one.
	static constexpr uint8_t kArchiveFormatVersion = 1;
	/// Closed slots wait in RAM for at most this long before they are written (the pending queue
	/// is sized for it: ceil(kPendingS / resolution) + 1 slots).
	static constexpr uint32_t kPendingS = 3600;
	/// A sample's value is held for at most its ring's spacing plus this (the tick's slack); a
	/// longer silence is a gap.
	static constexpr uint32_t kHoldSlackS = 10;

	/// `zone` gives the local days of daily archives; nullptr: UTC days.
	History(EventLoop& loop, const Clock& clock, DatapointModel& model, BlobStore& store,
		const HistoryLimits& limits = HistoryLimits(), const TimeZone* zone = nullptr);
	~History();

	History(const History&) = delete;
	History& operator=(const History&) = delete;

	/// The samples of a ring: retention / spacing rounded up, plus one (a ring spans its retention).
	/// The same rule gives the slots of an archive (retention / resolution).
	static uint32_t ring_samples(uint32_t retention_s, uint32_t spacing_s);

	/// The sample memory of a ring in bytes: 8 per sample.
	static size_t ring_bytes(uint32_t retention_s, uint32_t spacing_s) {
		return static_cast<size_t>(ring_samples(retention_s, spacing_s)) * sizeof(HistorySample);
	}

	/// The RAM of an archive in bytes: its open slot and the pending queue of one kPendingS,
	/// (ceil(kPendingS / resolution) + 2) * 32 (96 bytes for a resolution of an hour or more).
	static size_t archive_ram_bytes(uint32_t resolution_s);

	/// The segments of an archive: ceil(slots / kSlotsPerSegment).
	static uint32_t archive_segments(uint32_t retention_s, uint32_t resolution_s);

	/// The flash of an archive in bytes: 4096 per segment.
	static size_t archive_flash_bytes(uint32_t retention_s, uint32_t resolution_s) {
		return static_cast<size_t>(archive_segments(retention_s, resolution_s)) * kSegmentBytes;
	}

	/// What a spec costs against `history.budget`: the ring's samples and the archives' RAM.
	static size_t ram_bytes(const HistorySpec& spec);

	/// What a spec costs against `history.archiveBudget`: the archives' segments.
	static size_t flash_bytes(const HistorySpec& spec);

	/// Replaces the rings and archives by those of the specs, for the model's current datapoints.
	/// Validates everything first (a refusal keeps the previous ones); `budget_bytes` is the image's
	/// RAM budget (at most HistoryLimits::cap_bytes), `archive_budget_bytes` its flash budget (at
	/// most HistoryLimits::archive_cap_bytes). Saves the previous rings' new samples and archives'
	/// slots, allocates, restores the saved rings, drops archives whose layout changed (logged) and
	/// deletes rings and archives no longer in the image. The first call registers the listener and
	/// starts the tick and the save timer.
	HistoryStatus load(const HistorySpec* specs, size_t count, size_t budget_bytes, size_t archive_budget_bytes = 0);

	/// Saves every ring with new samples and every archive with new slots now.
	void flush();

	/// The samples of a datapoint with from <= t < to (and within the retention), decimated to at
	/// most `max_points` points: when there are more samples, the range is split into max_points
	/// buckets of equal width (whole seconds) and each non-empty bucket gives one point with the
	/// minimum, maximum and last value; otherwise every sample is a point. `count` receives the
	/// number of points. Refused with unknown_datapoint (no history for `id`) or malformed
	/// (to <= from, max_points 0).
	HistoryStatus range(uint16_t id, int64_t from, int64_t to, HistoryPoint* out, size_t max_points,
		size_t& count) const;

	/// The history of a datapoint over [from, to) in `points` buckets of ceil((to - from) / points)
	/// seconds, from the store that fits (spec/api.md "history"): of the stores whose resolution
	/// (the ring's spacing, an archive's resolution) is at most the bucket width (the ring when
	/// none is), the finest that reaches back to `from`, otherwise the one that reaches furthest.
	/// Each non-empty bucket goes to `sink`, oldest first, until it returns false. `answer` names
	/// the store. Refused with unknown_datapoint or malformed (to <= from, points 0).
	HistoryStatus query(uint16_t id, int64_t from, int64_t to, uint32_t points, HistorySink& sink,
		HistoryAnswer& answer) const;

	/// The store query() takes for these arguments, without reading it (`answer`); refused like query().
	HistoryStatus store_for(uint16_t id, int64_t from, int64_t to, uint32_t points, HistoryAnswer& answer) const;

	/// Whether the datapoint has a history.
	bool has_history(uint16_t id) const;

	/// Samples in the ring of a datapoint (0 without history).
	size_t samples(uint16_t id) const;

	/// How far back the ring of a datapoint reaches, in seconds (its retention; 0 without history).
	uint32_t retention_s(uint16_t id) const;

	/// Archives of a datapoint (0 without history).
	size_t archives(uint16_t id) const;

	/// The open slot of the archive of `resolution_s` of a datapoint; false if there is none open.
	bool open_slot(uint16_t id, uint32_t resolution_s, ArchiveSlot& slot) const;

	/// Number of rings.
	size_t size() const { return count_; }

	/// Sample memory of the loaded rings in bytes.
	size_t bytes() const { return sample_bytes_; }

	/// RAM of the loaded rings and archives against the budget (ram_bytes()).
	size_t ram_used() const { return ram_bytes_; }

	/// Flash of the loaded archives against the archive budget (flash_bytes()).
	size_t flash_used() const { return flash_bytes_; }

	/// Ring blobs written since construction (successful writes).
	uint32_t writes() const { return writes_; }

	/// Archive segment writes (creations and patches) since construction.
	uint32_t archive_writes() const { return archive_writes_; }

	/// Closed archive slots dropped because the pending queue was full and could not be written.
	uint32_t slots_dropped() const { return slots_dropped_; }

	/// The save interval of the loaded rings in seconds (HistoryLimits).
	uint32_t save_interval_s() const { return save_interval_s_; }

	// DatapointListener
	void datapoint_updated(const DatapointUpdate& update) override;

private:
	struct Ring;
	struct Archive;
	class SlotReader;

	HistoryStatus validate(const HistorySpec* specs, size_t count, size_t budget_bytes, size_t archive_budget_bytes,
		size_t& ram, size_t& flash, size_t& archives, size_t& pending) const;
	HistoryStatus start(uint32_t save_interval_s);
	Ring* ring(uint16_t id) const;
	Ring* ring_of(const Datapoint& datapoint) const;
	void append(Ring& ring, uint32_t t, uint32_t bits);
	bool save(Ring& ring);
	void restore(Ring& ring);
	void on_tick();
	void free_rings();

	// Archives (history_archive.cpp).
	void feed(Ring& ring, uint32_t t, uint32_t bits);
	void settle(Ring& ring, uint32_t now);
	void hold(Ring& ring, double value, uint32_t from, uint32_t to);
	void open_at(Ring& ring, Archive& archive, uint32_t t);
	void close(Ring& ring, Archive& archive);
	bool save_archive(const Ring& ring, Archive& archive);
	void check_archives();
	history_detail::SegmentState segment_state(const Ring& ring, const Archive& archive, uint32_t segment,
		uint32_t* saved_retention) const;
	uint32_t slot_number(const Archive& archive, int64_t t) const;
	int64_t slot_start(const Archive& archive, uint32_t number) const;
	int64_t local_day_start(int64_t day) const;
	bool slot_valid(const Archive& archive, const ArchiveSlot& slot, uint32_t number) const;
	/// -1: the ring, k: archive k.
	int choose_store(const Ring& ring, int64_t from, int64_t to, uint32_t points) const;
	HistoryStatus query_raw(const Ring& ring, int64_t from, int64_t to, uint32_t points, int64_t now, bool synced,
		HistorySink& sink) const;
	HistoryStatus query_archive(const Ring& ring, const Archive& archive, int64_t from, int64_t to, uint32_t points,
		int64_t now, HistorySink& sink) const;

	EventLoop& loop_;
	const Clock& clock_;
	DatapointModel& model_;
	BlobStore& store_;
	HistoryLimits limits_;
	const TimeZone* zone_;
	Ring* rings_ = nullptr;
	HistorySample* samples_ = nullptr;
	char* names_ = nullptr;
	Archive* archives_ = nullptr;
	ArchiveSlot* pending_ = nullptr;
	size_t count_ = 0;
	size_t sample_bytes_ = 0;
	size_t ram_bytes_ = 0;
	size_t flash_bytes_ = 0;
	TimerHandle tick_;
	TimerHandle save_timer_;
	bool listening_ = false;
	bool failure_logged_ = false;
	bool archive_failure_logged_ = false;
	bool drop_logged_ = false;
	uint32_t writes_ = 0;
	uint32_t archive_writes_ = 0;
	uint32_t slots_dropped_ = 0;
	uint32_t save_interval_s_ = 0;
};

} // namespace hc

#endif // HC_HISTORY_H
