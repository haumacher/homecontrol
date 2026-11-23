/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The parts of hc::History that its two sources share: the ring and archive records and the
// conversions of the sample bits (history.cpp: rings, history_archive.cpp: archives).
#ifndef HC_HISTORY_INTERNAL_H
#define HC_HISTORY_INTERNAL_H

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "hc/history.h"
#include "hc/value_store.h"

// The samples and slots go to flash as they are in RAM: little-endian words, IEEE 754 numbers.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the history blob formats assume a little-endian CPU");
#endif

namespace hc {

namespace history_detail {

constexpr const char* kTag = "hc.history";

inline void put_u32(uint8_t* p, uint32_t v) {
	for (int i = 0; i < 4; ++i) {
		p[i] = static_cast<uint8_t>(v >> (8 * i));
	}
}

inline void put_u16(uint8_t* p, uint16_t v) {
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
}

inline uint32_t get_u32(const uint8_t* p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
		(static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t get_u16(const uint8_t* p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline bool sampled_type(ValueType type) {
	return type == ValueType::boolean || type == ValueType::unsigned_int || type == ValueType::signed_int ||
		type == ValueType::real;
}

inline uint32_t to_bits(const Value& value) {
	switch (value.type()) {
		case ValueType::boolean: return value.as_bool() ? 1 : 0;
		case ValueType::unsigned_int: return value.as_unsigned();
		case ValueType::signed_int: return static_cast<uint32_t>(value.as_signed());
		case ValueType::real: {
			double d = value.as_real();
			float f = d > FLT_MAX ? FLT_MAX : d < -FLT_MAX ? -FLT_MAX : static_cast<float>(d);
			uint32_t bits = 0;
			std::memcpy(&bits, &f, sizeof(bits));
			return bits;
		}
		default: return 0;
	}
}

inline double to_double(ValueType type, uint32_t bits) {
	switch (type) {
		case ValueType::boolean: return bits != 0 ? 1.0 : 0.0;
		case ValueType::unsigned_int: return bits;
		case ValueType::signed_int: return static_cast<int32_t>(bits);
		case ValueType::real: {
			float f = 0;
			std::memcpy(&f, &bits, sizeof(f));
			return f;
		}
		default: return 0;
	}
}

/// A ring blob whose header and checksum were read (History::ring_blob()): where its samples are.
struct RingBlob {
	char name[persist::kBlobNameSize] = {};
	size_t data = 0;          ///< Offset of the first (oldest) sample.
	uint32_t samples = 0;
	uint32_t sample_size = 0; ///< At least 8; a reader takes the first 8 bytes of each.
	uint32_t spacing_s = 0;   ///< The spacing the samples were taken at (0: the blob does not say).
};

/// Whether a slot was fed at all.
inline bool slot_empty(const ArchiveSlot& slot) {
	return slot.count == 0 && slot.held_s == 0;
}

constexpr const char* kArchivePrefix = "a.";
/// The journal of a carry-over (homecontrol #118, doc/persistence.md "Journal format"): "ac." + 8 hex
/// digits of the name's hash. Magic `H` `J`, the format generation, the number of archives, the hash,
/// their resolutions (u32 each) and the CRC-32 of everything before it.
constexpr const char* kJournalPrefix = "ac.";
constexpr uint8_t kJournalMagic0 = 'H';
constexpr uint8_t kJournalMagic1 = 'J';
constexpr uint8_t kJournalVersion = 1;
constexpr size_t kJournalMaxBytes = 12 + 4 * History::kMaxArchives;
constexpr uint8_t kArchiveMagic0 = 'H';
constexpr uint8_t kArchiveMagic1 = 'A';
/// The most segments an archive can have (kMaxSlots slots).
constexpr uint32_t kMaxSegments = (History::kMaxSlots + History::kSlotsPerSegment - 1) / History::kSlotsPerSegment;
constexpr size_t kSegmentNameSize = 32;

/// "a." + 8 hex digits of the name's hash + "." + the resolution in hex + "." + the segment in hex.
inline void segment_name(uint32_t hash, uint32_t resolution_s, uint32_t segment, char* out, size_t size) {
	std::snprintf(out, size, "%s%08x.%x.%x", kArchivePrefix, static_cast<unsigned>(hash),
		static_cast<unsigned>(resolution_s), static_cast<unsigned>(segment));
}

/// The header of segment `index` (doc/persistence.md "Archive segment format").
void build_header(uint8_t* h, uint32_t hash, ValueType type, uint32_t resolution_s, uint32_t retention_s, uint32_t days,
	uint16_t index, uint16_t segments);

/// The middle of a slot from its start: the number of a slot follows from it even after the zone
/// moved local midnight by some hours.
inline int64_t slot_middle(uint32_t start, uint32_t resolution_s, uint32_t days) {
	return static_cast<int64_t>(start) + (days > 0 ? static_cast<int64_t>(days) * 43200 : resolution_s / 2);
}

/// A value into a slot's minimum, maximum and last.
inline void take(ArchiveSlot& s, float value) {
	if (slot_empty(s)) {
		s.min = value;
		s.max = value;
	} else {
		s.min = value < s.min ? value : s.min;
		s.max = value > s.max ? value : s.max;
	}
	s.last = value;
}

enum class SegmentState : uint8_t {
	ok,
	retention_changed, ///< The same layout: the data stays, the header is rewritten.
	absent,
	damaged,
	newer,
	type_changed,
	layout_changed,
	foreign,
};

} // namespace history_detail

struct History::Ring {
	HistorySample* samples = nullptr;
	const char* name = ""; ///< Own copy of the datapoint's name.
	uint32_t capacity = 0;
	uint32_t head = 0;     ///< Index of the oldest sample.
	uint32_t count = 0;
	uint32_t spacing_s = 0;
	uint32_t retention_s = 0;
	uint32_t hash = 0;     ///< FNV-1a of the name (the blob names).
	uint16_t id = 0;
	ValueType type = ValueType::none;
	bool dirty = false;       ///< Samples added since the last save.
	bool persistable = true;  ///< False for the second of two names with the same hash.
	bool meter = false;       ///< HistorySpec::meter.
	/// The archives, finest first (a range of History::archives_).
	Archive* archives = nullptr;
	uint8_t archive_count = 0;
	/// The archives hold the last sample's value up to this time (UTC seconds); 0: nothing to hold
	/// (no sample since the load).
	uint32_t credit_t = 0;
	/// A carry-over into new stores is in progress for this datapoint (homecontrol #118): its ring
	/// blob and its archives on the store are what it reads, so the periodic save leaves them alone.
	bool carrying = false;

	HistorySample& at(uint32_t i) const { return samples[(head + i) % capacity]; }
	const HistorySample& back() const { return at(count - 1); }
};

struct History::Archive {
	// (Ordered by size: 96 bytes on the C6.)
	uint32_t resolution_s = 0;
	uint32_t retention_s = 0;
	uint32_t slots = 0;       ///< The retention window: ring_samples(retention, resolution).
	uint32_t positions = 0;   ///< Slots on flash: segments * kSlotsPerSegment; slot n is at n % positions.
	uint32_t days = 0;        ///< The resolution in whole local days; 0: aligned to UTC multiples.
	uint32_t open_number = 0;
	/// The slot of the carry-over's cut (homecontrol #118, `merged`).
	uint32_t carry_number = 0;
	uint32_t present = 0;     ///< At load: bit s, segment s is on the store (one walk, check_archives()).
	uint16_t segments = 0;
	uint16_t pending_capacity = 0;
	uint16_t pending_count = 0;
	bool has_open = false;
	bool open_dirty = false;  ///< The open slot changed since it was last written.
	/// A carry-over fills it (homecontrol #118): none of its segments was on the store at the load, or
	/// a journal says an earlier carry-over did not finish it. Until the carry-over is done it is not
	/// saved and a slot opening is not taken up from the store (except the slot of the cut, below).
	bool carry = false;
	/// Its segments were on the store at the load (an unfinished carry-over's): they are rewritten.
	bool stale = false;
	/// The carry-over has written the slot of its cut (`carry_number`), merged with the live one.
	bool merged = false;
	ArchiveSlot* pending = nullptr; ///< Closed slots not yet on flash, oldest first.
	int64_t open_end = 0;     ///< The open slot's end (exclusive), UTC seconds.
	ArchiveSlot open = {};
	int64_t from = 0;         ///< As a source of a carry-over: the oldest time it holds.
};

/// Reads the slots of an archive from its segments, a window of kWindow slots at a time. A get()
/// makes at most kMaxCalls store calls, which it adds to `calls` (the carry-over's work).
class History::SlotReader {
public:
	static constexpr uint32_t kWindow = 16;
	static constexpr uint32_t kMaxCalls = 3;

	SlotReader(const History& history, const Ring& ring, const Archive& archive, uint32_t* calls = nullptr)
		: history_(&history), ring_(&ring), archive_(&archive), calls_(calls) {}

	/// A reader of no archive yet (reset() gives it one).
	SlotReader(const History& history, uint32_t* calls) : history_(&history), calls_(calls) {}

	/// Reads `archive` of `ring` from now on (the window is dropped).
	void reset(const Ring& ring, const Archive& archive) {
		ring_ = &ring;
		archive_ = &archive;
		segment_ = UINT32_MAX;
		count_ = 0;
	}

	/// The slot at `position` on flash; false if its segment is missing or not this archive's.
	bool get(uint32_t position, ArchiveSlot& out);

private:
	const History* history_;
	const Ring* ring_ = nullptr;
	const Archive* archive_ = nullptr;
	uint32_t* calls_;
	uint32_t segment_ = UINT32_MAX;
	bool usable_ = false;
	uint32_t base_ = 0;
	uint32_t count_ = 0;
	ArchiveSlot window_[kWindow];
};

/// The state of a carry-over into new stores (homecontrol #118, history_carry.cpp): the jobs, allocated
/// by load() when an archive needs one, and the work of the current one; freed when it is done (on the
/// heap: the event loop's stack keeps its margin). No block above 3 KB (homecontrol #71: a swap takes no
/// large block): the jobs in pages, the slots of a segment in two halves, allocated at the first step
/// that needs them. One datapoint after the other (a job), each in phases that a step leaves and the
/// next takes up where it stopped.
struct History::Carry {
	static constexpr size_t kMaxFound = 8;
	static constexpr size_t kMaxSources = 6;
	static constexpr uint32_t kRingWindow = 32;
	static constexpr size_t kJobsPerPage = 32;
	/// Slots of the first half of a segment; the second has the rest.
	static constexpr uint32_t kLow = 64;
	static constexpr uint32_t kHigh = History::kSlotsPerSegment - kLow;

	struct Job {
		uint16_t ring = 0;            ///< Index in History::rings_.
		uint8_t found_count = 0;
		bool journal = false;         ///< A journal of the datapoint is on the store (or queued for it).
		bool journal_written = false; ///< This carry-over's journal is durable.
		bool failed_logged = false;
		bool time_logged = false;
		/// The datapoint's archives the store had at the load: their resolutions and one segment each.
		uint32_t found_resolution[kMaxFound] = {};
		uint8_t found_segment[kMaxFound] = {};
	};

	enum class Phase : uint8_t {
		start,
		leftovers,       ///< Removing the archives an unfinished carry-over left that the image no longer has.
		leftovers_check,
		ring,            ///< Checking the ring blob.
		cut,
		headers,         ///< Reading the headers of the source archives.
		reach,           ///< How far back each source reaches.
		target,          ///< Consolidating the slots of a segment of a new archive.
		journal_write,
		journal_check,
		segment_write,
		segment_check,
		finish_journal,
		finish_journal_check,
		finish_sources,
	};

	Job** pages = nullptr;
	size_t page_count = 0;
	size_t job_count = 0;
	size_t job = 0;
	Phase phase = Phase::start;

	Job& job_at(size_t i) { return pages[i / kJobsPerPage][i % kJobsPerPage]; }

	/// The cut of the load (when the clock was synchronized), and the current job's.
	bool load_cut_known = false;
	int64_t load_cut = 0;
	bool cut_known = false;
	int64_t cut = 0;

	// The step.
	uint32_t work = 0;
	uint64_t step_start_ms = 0;
	uint64_t retry_at_ms = 0;
	bool memory_logged = false;

	// Leftovers (from the journal on the store).
	uint8_t leftover_count = 0;
	uint32_t leftovers[History::kMaxArchives] = {};
	uint32_t leftover_masks[History::kMaxArchives] = {};
	/// Cursors of the removal loops.
	uint32_t index = 0;
	uint32_t segment = 0;

	// The sources.
	history_detail::RingBlob blob;
	bool ring = false;
	uint32_t ring_spacing = 0;
	int64_t ring_from = INT64_MAX;
	int64_t ring_last = 0;
	size_t source_count = 0;
	Archive sources[kMaxSources];
	uint32_t scan = 0;
	uint32_t scan_end = 0;
	bool scanning = false;

	// The target: archive `target` of the ring, its segments newest first.
	uint8_t target = 0;
	bool target_open = false;
	bool target_ring = false;   ///< The ring is a source of this target (finer than it).
	uint32_t newest = 0;
	uint32_t oldest = 0;
	uint32_t seg_step = 0;
	uint32_t seg = 0;
	uint32_t slot = 0;          ///< The slot of the segment in work.
	uint32_t filled = 0;
	uint32_t slots_total = 0;
	/// The segment's slots in two halves (nullptr until the first step that needs them).
	ArchiveSlot* low = nullptr;
	ArchiveSlot* high = nullptr;
	uint32_t crc = 0;
	uint32_t verify_crc = 0;
	size_t verify_offset = 0;

	ArchiveSlot& slot_at(uint32_t j) { return j < kLow ? low[j] : high[j - kLow]; }
	void clear_slots() {
		std::memset(static_cast<void*>(low), 0, kLow * sizeof(ArchiveSlot));
		std::memset(static_cast<void*>(high), 0, kHigh * sizeof(ArchiveSlot));
	}

	// The slot in work: [s0, s1) from source `chosen` (-1: the ring).
	bool in_slot = false;
	int chosen = -1;
	int64_t s0 = 0;
	int64_t s1 = 0;
	ArchiveSlot acc = {};
	uint32_t m = 0;             ///< The next slot of the source archive.
	uint32_t m1 = 0;
	uint32_t k = UINT32_MAX;    ///< The next sample of the ring (UINT32_MAX: not searched yet).
	int64_t previous_t = -1;

	// Readers.
	SlotReader reader;
	int reader_source = -2;
	HistorySample window[kRingWindow];
	uint32_t window_base = 0;
	uint32_t window_count = 0;

	explicit Carry(const History& history) : reader(history, &work) {}
	~Carry() {
		for (size_t i = 0; i < page_count; ++i) {
			delete[] pages[i];
		}
		delete[] pages;
		delete[] low;
		delete[] high;
	}
	Carry(const Carry&) = delete;
	Carry& operator=(const Carry&) = delete;

	/// The pages of `jobs` jobs; false without the memory (then nothing is allocated).
	bool allocate_jobs(size_t jobs) {
		page_count = (jobs + kJobsPerPage - 1) / kJobsPerPage;
		pages = new (std::nothrow) Job*[page_count];
		if (pages == nullptr) {
			page_count = 0;
			return false;
		}
		for (size_t i = 0; i < page_count; ++i) {
			pages[i] = new (std::nothrow) Job[kJobsPerPage];
			if (pages[i] == nullptr) {
				for (size_t k = 0; k < i; ++k) {
					delete[] pages[k];
				}
				delete[] pages;
				pages = nullptr;
				page_count = 0;
				return false;
			}
		}
		return true;
	}
};

} // namespace hc

#endif // HC_HISTORY_INTERNAL_H
