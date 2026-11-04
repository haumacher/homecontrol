/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The consolidated archives of hc::History (issue #56, doc/persistence.md "Archives"): feeding the
// open slots, closing them into the pending queue, writing them into the segment blobs on flash,
// checking the segments at load, and the queries of the API.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "history_internal.h"
#include "hc/log.h"
#include "hc/tz.h"
#include "hc/value_store.h"

namespace hc {

using history_detail::get_u16;
using history_detail::get_u32;
using history_detail::kTag;
using history_detail::put_u16;
using history_detail::put_u32;
using history_detail::slot_empty;
using history_detail::to_double;

namespace {

constexpr const char* kArchivePrefix = "a.";
constexpr uint8_t kMagic0 = 'H';
constexpr uint8_t kMagic1 = 'A';
/// The most segments an archive can have (kMaxSlots slots).
constexpr uint32_t kMaxSegments =
	(History::kMaxSlots + History::kSlotsPerSegment - 1) / History::kSlotsPerSegment;
/// Slots a query reads from flash at a time.
constexpr uint32_t kWindow = 16;
/// Chunks of one patch: slots and the empty slots of the gaps between them.
constexpr size_t kMaxChunks = 32;
/// Empty slots, for the gaps of a patch and a new segment.
constexpr size_t kZeroBytes = 512;
const uint8_t kZeros[kZeroBytes] = {};
/// A local day is at most 25 hours (a DST change); a slot never holds more than its resolution and that.
constexpr uint32_t kDstSlackS = 7200;

static_assert(History::kSegmentBytes == 4096, "a segment is one LittleFS block");

/// "a." + 8 hex digits of the name's hash + "." + the resolution in hex + "." + the segment in hex.
void segment_name(uint32_t hash, uint32_t resolution_s, uint32_t segment, char* out, size_t size) {
	std::snprintf(out, size, "%s%08x.%x.%x", kArchivePrefix, static_cast<unsigned>(hash),
		static_cast<unsigned>(resolution_s), static_cast<unsigned>(segment));
}

constexpr size_t kSegmentNameSize = 32;

} // namespace

namespace history_detail {

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

namespace {

using history_detail::SegmentState;

const char* segment_state_reason(SegmentState state) {
	switch (state) {
		case SegmentState::damaged: return "its segments are damaged";
		case SegmentState::newer: return "written by a newer firmware";
		case SegmentState::type_changed: return "the datapoint's type changed";
		case SegmentState::layout_changed: return "its retention changed the number of segments";
		case SegmentState::foreign: return "its segments belong to another datapoint";
		default: return "";
	}
}

} // namespace

// ---- Segments -----------------------------------------------------------------------------------

namespace {

/// The header of segment `index` (doc/persistence.md "Archive segment format").
void build_header(uint8_t* h, uint32_t hash, ValueType type, uint32_t resolution_s, uint32_t retention_s, uint32_t days,
	uint16_t index, uint16_t segments) {
	std::memset(h, 0, History::kSegmentHeaderBytes);
	h[0] = kMagic0;
	h[1] = kMagic1;
	h[2] = History::kArchiveFormatVersion;
	h[3] = sizeof(ArchiveSlot);
	put_u32(h + 4, resolution_s);
	put_u32(h + 8, retention_s);
	put_u32(h + 12, hash);
	h[16] = static_cast<uint8_t>(type);
	h[17] = days > 0 ? 1 : 0;
	put_u16(h + 18, index);
	put_u16(h + 20, segments);
	put_u16(h + 22, static_cast<uint16_t>(History::kSlotsPerSegment));
	put_u32(h + 28, persist::crc32(0, h, 28));
}

} // namespace

/// Reads the slots of an archive from its segments, a window of kWindow slots at a time.
class History::SlotReader {
public:
	SlotReader(const History& history, const Ring& ring, const Archive& archive)
		: history_(history), ring_(ring), archive_(archive) {}

	/// The slot at `position` on flash; false if its segment is missing or not this archive's.
	bool get(uint32_t position, ArchiveSlot& out) {
		uint32_t segment = position / kSlotsPerSegment;
		uint32_t index = position % kSlotsPerSegment;
		if (segment != segment_) {
			segment_ = segment;
			count_ = 0;
			SegmentState state = history_.segment_state(ring_, archive_, segment, nullptr);
			usable_ = state == SegmentState::ok || state == SegmentState::retention_changed;
		}
		if (!usable_) {
			return false;
		}
		if (count_ == 0 || index < base_ || index >= base_ + count_) {
			char name[kSegmentNameSize];
			segment_name(ring_.hash, archive_.resolution_s, segment, name, sizeof(name));
			uint32_t want = std::min<uint32_t>(kWindow, kSlotsPerSegment - index);
			size_t length = 0;
			BlobStatus status = history_.store_.read(name, kSegmentHeaderBytes + index * sizeof(ArchiveSlot), window_,
				want * sizeof(ArchiveSlot), length);
			base_ = index;
			count_ = status == BlobStatus::ok ? static_cast<uint32_t>(length / sizeof(ArchiveSlot)) : 0;
			if (count_ == 0) {
				return false;
			}
		}
		out = window_[index - base_];
		return true;
	}

private:
	const History& history_;
	const Ring& ring_;
	const Archive& archive_;
	uint32_t segment_ = UINT32_MAX;
	bool usable_ = false;
	uint32_t base_ = 0;
	uint32_t count_ = 0;
	ArchiveSlot window_[kWindow];
};

SegmentState History::segment_state(const Ring& r, const Archive& a, uint32_t segment,
	uint32_t* saved_retention) const {
	char name[kSegmentNameSize];
	segment_name(r.hash, a.resolution_s, segment, name, sizeof(name));
	size_t size = 0;
	BlobStatus status = store_.size(name, size);
	if (status == BlobStatus::not_found) {
		return SegmentState::absent;
	}
	uint8_t h[kSegmentHeaderBytes];
	size_t length = 0;
	if (status == BlobStatus::ok) {
		status = store_.read(name, 0, h, sizeof(h), length);
	}
	if (status != BlobStatus::ok || length != sizeof(h) || h[0] != kMagic0 || h[1] != kMagic1 ||
		get_u32(h + 28) != persist::crc32(0, h, 28) || h[2] == 0) {
		return SegmentState::damaged;
	}
	if (h[2] > kArchiveFormatVersion) {
		return SegmentState::newer;
	}
	if (get_u32(h + 12) != r.hash || get_u32(h + 4) != a.resolution_s) {
		return SegmentState::foreign;
	}
	if (h[16] != static_cast<uint8_t>(r.type)) {
		return SegmentState::type_changed;
	}
	if (h[3] != sizeof(ArchiveSlot) || get_u16(h + 22) != kSlotsPerSegment || get_u16(h + 20) != a.segments ||
		(h[17] != 0) != (a.days > 0)) {
		return SegmentState::layout_changed;
	}
	if (get_u16(h + 18) != segment || size != kSegmentBytes) {
		return SegmentState::damaged;
	}
	if (saved_retention != nullptr) {
		*saved_retention = get_u32(h + 8);
	}
	return get_u32(h + 8) != a.retention_s ? SegmentState::retention_changed : SegmentState::ok;
}

// ---- Slot numbers and local days ----------------------------------------------------------------

int64_t History::local_day_start(int64_t day) const {
	// The first second whose local date is `day`: within 14 h before and 12 h after UTC midnight,
	// plus a DST hour; the local date is monotonic in UTC, so a binary search finds it.
	auto local_day = [this](int64_t t) -> int64_t {
		LocalTime local;
		if (zone_ == nullptr || !zone_->to_local(t, local)) {
			return t >= 0 ? t / 86400 : (t - 86399) / 86400;
		}
		return days_from_civil(local.year, local.month, local.day);
	};
	int64_t lo = day * 86400 - 54000;
	int64_t hi = day * 86400 + 46800;
	if (local_day(lo) >= day || local_day(hi) < day) {
		return day * 86400; // A zone the platform cannot convert: UTC days.
	}
	while (hi - lo > 1) {
		int64_t mid = lo + (hi - lo) / 2;
		if (local_day(mid) >= day) {
			hi = mid;
		} else {
			lo = mid;
		}
	}
	return hi;
}

uint32_t History::slot_number(const Archive& a, int64_t t) const {
	if (t < 0) {
		t = 0;
	}
	if (a.days == 0) {
		return static_cast<uint32_t>(t / a.resolution_s);
	}
	int64_t day = t / 86400;
	LocalTime local;
	if (zone_ != nullptr && zone_->to_local(t, local)) {
		day = days_from_civil(local.year, local.month, local.day);
	}
	return static_cast<uint32_t>((day < 0 ? 0 : day) / a.days);
}

int64_t History::slot_start(const Archive& a, uint32_t number) const {
	if (a.days == 0) {
		return static_cast<int64_t>(number) * a.resolution_s;
	}
	return local_day_start(static_cast<int64_t>(number) * a.days);
}

namespace {

/// The middle of a slot from its start: the number of a slot follows from it even after the zone
/// moved local midnight by some hours.
int64_t slot_middle(uint32_t start, uint32_t resolution_s, uint32_t days) {
	return static_cast<int64_t>(start) + (days > 0 ? static_cast<int64_t>(days) * 43200 : resolution_s / 2);
}

} // namespace

bool History::slot_valid(const Archive& a, const ArchiveSlot& s, uint32_t number) const {
	if (slot_empty(s) || s.start == 0) {
		return false;
	}
	if (!std::isfinite(s.sum) || !std::isfinite(s.min) || !std::isfinite(s.max) || !std::isfinite(s.last) ||
		s.min > s.max || s.held_s > a.resolution_s + kDstSlackS) {
		return false;
	}
	return slot_number(a, slot_middle(s.start, a.resolution_s, a.days)) == number;
}

// ---- Feeding ------------------------------------------------------------------------------------

namespace {

void take(ArchiveSlot& s, float value) {
	if (slot_empty(s)) {
		s.min = value;
		s.max = value;
	} else {
		s.min = std::min(s.min, value);
		s.max = std::max(s.max, value);
	}
	s.last = value;
}

} // namespace

void History::feed(Ring& r, uint32_t t, uint32_t bits) {
	if (r.archive_count == 0) {
		return;
	}
	if (r.credit_t != 0 && t < r.credit_t) {
		return; // The clock went back: the archives go on once it has passed their slots again.
	}
	if (r.credit_t != 0 && r.count > 0) {
		// The previous sample's value held until now, at most its spacing and the slack.
		const HistorySample& previous = r.back();
		uint64_t limit = static_cast<uint64_t>(previous.t) + r.spacing_s + kHoldSlackS;
		uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(t, limit));
		if (end > r.credit_t) {
			hold(r, to_double(r.type, previous.bits), r.credit_t, end);
		}
	}
	float value = static_cast<float>(to_double(r.type, bits));
	for (uint8_t k = 0; k < r.archive_count; ++k) {
		Archive& a = r.archives[k];
		if (!a.has_open || t >= a.open_end) {
			open_at(r, a, t);
		}
		take(a.open, value);
		++a.open.count;
		a.open_dirty = true;
	}
	r.credit_t = t;
}

void History::hold(Ring& r, double value, uint32_t from, uint32_t to) {
	float f = static_cast<float>(value);
	for (uint8_t k = 0; k < r.archive_count; ++k) {
		Archive& a = r.archives[k];
		int64_t x = from;
		while (x < to) {
			if (!a.has_open || x >= a.open_end) {
				open_at(r, a, static_cast<uint32_t>(x));
			}
			int64_t end = std::min<int64_t>(to, a.open_end);
			if (end <= x) {
				break;
			}
			uint32_t seconds = static_cast<uint32_t>(end - x);
			take(a.open, f);
			a.open.sum += value * seconds;
			a.open.held_s += seconds;
			a.open_dirty = true;
			x = end;
		}
	}
}

void History::settle(Ring& r, uint32_t now) {
	if (r.archive_count == 0) {
		return;
	}
	if (r.credit_t != 0 && r.count > 0 && now > r.credit_t) {
		const HistorySample& last = r.back();
		uint64_t limit = static_cast<uint64_t>(last.t) + r.spacing_s + kHoldSlackS;
		uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(now, limit));
		if (end > r.credit_t) {
			hold(r, to_double(r.type, last.bits), r.credit_t, end);
			r.credit_t = end;
		}
	}
	for (uint8_t k = 0; k < r.archive_count; ++k) {
		Archive& a = r.archives[k];
		if (a.has_open && now >= a.open_end) {
			close(r, a);
		}
	}
}

void History::open_at(Ring& r, Archive& a, uint32_t t) {
	if (a.has_open) {
		close(r, a);
	}
	uint32_t number = slot_number(a, t);
	int64_t start = slot_start(a, number);
	int64_t end = slot_start(a, number + 1);
	if (start > t || end <= t || start <= 0) {
		// Only a zone that contradicts itself gets here: a slot of the resolution from t.
		start = t;
		end = static_cast<int64_t>(t) + a.resolution_s;
	}
	a.has_open = true;
	a.open_number = number;
	a.open_end = end;
	a.open_dirty = false;
	a.open = ArchiveSlot{};
	a.open.start = static_cast<uint32_t>(start);
	// The slot was closed a moment ago (it ended while the clock stood behind) or saved before a
	// restart or a swap: it goes on.
	if (a.pending_count > 0 && a.pending[a.pending_count - 1].start == a.open.start) {
		a.open = a.pending[--a.pending_count];
		a.open_dirty = true;
		return;
	}
	if (r.persistable) {
		SlotReader reader(*this, r, a);
		ArchiveSlot saved;
		if (reader.get(number % a.positions, saved) && saved.start == a.open.start && slot_valid(a, saved, number)) {
			a.open = saved;
		}
	}
}

void History::close(Ring& r, Archive& a) {
	a.has_open = false;
	if (slot_empty(a.open)) {
		return;
	}
	if (a.pending_count == a.pending_capacity) {
		// Saves are later than an hour (a store that failed, or a long save interval): write now.
		save_archive(r, a);
		if (a.pending_count == a.pending_capacity) {
			std::memmove(a.pending, a.pending + 1, (a.pending_count - 1) * sizeof(ArchiveSlot));
			--a.pending_count;
			++slots_dropped_;
			if (!drop_logged_) {
				Log::warn(kTag, "history of '%s': archive slots dropped, they cannot be written (%u s archive)", r.name,
					static_cast<unsigned>(a.resolution_s));
				drop_logged_ = true;
			}
		}
	}
	a.pending[a.pending_count++] = a.open;
}

// ---- Writing ------------------------------------------------------------------------------------

bool History::save_archive(const Ring& r, Archive& a) {
	if (!r.persistable) {
		return false;
	}
	bool with_open = a.has_open && a.open_dirty && !slot_empty(a.open);
	size_t total = a.pending_count + (with_open ? 1 : 0);
	auto item = [&a](size_t i) -> const ArchiveSlot& { return i < a.pending_count ? a.pending[i] : a.open; };
	auto position = [this, &a](const ArchiveSlot& s) {
		return slot_number(a, slot_middle(s.start, a.resolution_s, a.days)) % a.positions;
	};
	size_t done = 0;
	bool ok = true;
	while (done < total) {
		// One patch: the following slots that lie in one segment at increasing positions, the gaps
		// between them written as empty slots.
		uint32_t first = position(item(done));
		uint32_t segment = first / kSlotsPerSegment;
		uint32_t next = first % kSlotsPerSegment;
		uint32_t begin = next;
		BlobChunk chunks[kMaxChunks];
		size_t n = 0;
		size_t end = done;
		while (end < total) {
			uint32_t p = position(item(end));
			uint32_t index = p % kSlotsPerSegment;
			if (p / kSlotsPerSegment != segment || index < next) {
				break;
			}
			size_t gap = (index - next) * sizeof(ArchiveSlot);
			size_t needed = 1 + (gap + kZeroBytes - 1) / kZeroBytes;
			if (n + needed > kMaxChunks) {
				break;
			}
			while (gap > 0) {
				size_t piece = std::min(gap, kZeroBytes);
				chunks[n++] = BlobChunk{kZeros, piece};
				gap -= piece;
			}
			chunks[n++] = BlobChunk{&item(end), sizeof(ArchiveSlot)};
			next = index + 1;
			++end;
		}
		char name[kSegmentNameSize];
		segment_name(r.hash, a.resolution_s, segment, name, sizeof(name));
		SegmentState state = segment_state(r, a, segment, nullptr);
		BlobStatus status = BlobStatus::ok;
		if (state != SegmentState::ok && state != SegmentState::retention_changed) {
			// A new segment: the header and empty slots, then the patch.
			uint8_t header[kSegmentHeaderBytes];
			build_header(header, r.hash, r.type, a.resolution_s, a.retention_s, a.days, static_cast<uint16_t>(segment),
				a.segments);
			BlobChunk fresh[1 + (kSlotsPerSegment * sizeof(ArchiveSlot) + kZeroBytes - 1) / kZeroBytes];
			size_t m = 0;
			fresh[m++] = BlobChunk{header, sizeof(header)};
			for (size_t left = kSlotsPerSegment * sizeof(ArchiveSlot); left > 0;) {
				size_t piece = std::min(left, kZeroBytes);
				fresh[m++] = BlobChunk{kZeros, piece};
				left -= piece;
			}
			status = store_.write(name, fresh, m);
			if (status == BlobStatus::ok) {
				++archive_writes_;
				if (state == SegmentState::absent && segments_to_come_ > 0) {
					--segments_to_come_;
				}
			}
		}
		if (status == BlobStatus::ok) {
			status = store_.patch(name, kSegmentHeaderBytes + begin * sizeof(ArchiveSlot), chunks, n);
		}
		if (status != BlobStatus::ok) {
			if (!archive_failure_logged_) {
				Log::error(kTag, "cannot save the %u s archive of '%s' (%s): %s; retrying at the next save",
					static_cast<unsigned>(a.resolution_s), r.name, name,
					status == BlobStatus::unsupported ? "the blob store cannot patch" : blob_status_name(status));
				archive_failure_logged_ = true;
			}
			ok = false;
			break;
		}
		++archive_writes_;
		done = end;
	}
	if (ok && archive_failure_logged_) {
		Log::info(kTag, "saving the archives works again");
		archive_failure_logged_ = false;
	}
	size_t written = std::min<size_t>(done, a.pending_count);
	std::memmove(a.pending, a.pending + written, (a.pending_count - written) * sizeof(ArchiveSlot));
	a.pending_count = static_cast<uint16_t>(a.pending_count - written);
	if (with_open && done == total) {
		a.open_dirty = false;
	}
	return ok;
}

// ---- Load ---------------------------------------------------------------------------------------

void History::check_archives() {
	size_t missing = 0;
	for (size_t i = 0; i < count_; ++i) {
		Ring& r = rings_[i];
		if (!r.persistable) {
			if (r.archive_count > 0) {
				Log::warn(kTag, "history of '%s': its archives are kept in RAM only (the blob name collides)", r.name);
			}
			continue;
		}
		for (uint8_t k = 0; k < r.archive_count; ++k) {
			Archive& a = r.archives[k];
			const char* reason = nullptr;
			uint32_t saved_retention = a.retention_s;
			uint32_t existing = 0;
			bool retention_changed = false;
			for (uint32_t s = 0; s < kMaxSegments; ++s) {
				uint32_t retention = a.retention_s;
				SegmentState state = segment_state(r, a, s, &retention);
				if (state == SegmentState::absent) {
					continue;
				}
				++existing;
				if (s >= a.segments && reason == nullptr) {
					reason = segment_state_reason(SegmentState::layout_changed);
				} else if (state == SegmentState::retention_changed) {
					retention_changed = true;
					saved_retention = retention;
				} else if (state != SegmentState::ok && reason == nullptr) {
					reason = segment_state_reason(state);
				}
			}
			if (reason != nullptr) {
				Log::warn(kTag, "history of '%s': the %u s archive is dropped: %s", r.name,
					static_cast<unsigned>(a.resolution_s), reason);
				for (uint32_t s = 0; s < kMaxSegments; ++s) {
					char name[kSegmentNameSize];
					segment_name(r.hash, a.resolution_s, s, name, sizeof(name));
					store_.remove(name);
				}
				existing = 0;
			} else if (retention_changed) {
				Log::info(kTag, "history of '%s': the %u s archive keeps its slots (retention %u s, was %u s, same segments)",
					r.name, static_cast<unsigned>(a.resolution_s), static_cast<unsigned>(a.retention_s),
					static_cast<unsigned>(saved_retention));
				for (uint32_t s = 0; s < a.segments; ++s) {
					char name[kSegmentNameSize];
					segment_name(r.hash, a.resolution_s, s, name, sizeof(name));
					uint8_t header[kSegmentHeaderBytes];
					build_header(header, r.hash, r.type, a.resolution_s, a.retention_s, a.days, static_cast<uint16_t>(s),
						a.segments);
					BlobChunk chunk{header, sizeof(header)};
					BlobStatus status = store_.patch(name, 0, &chunk, 1);
					if (status == BlobStatus::ok) {
						++archive_writes_;
					}
				}
			}
			missing += a.segments - std::min<uint32_t>(existing, a.segments);
		}
	}

	// Segments of archives no longer in the image (another resolution, a datapoint without history).
	class ArchiveKeeper : public persist::BlobKeeper {
	public:
		ArchiveKeeper(const Ring* rings, size_t count) : rings_(rings), count_(count) {}
		bool keep(const char* name) const override {
			char* end = nullptr;
			unsigned long hash = std::strtoul(name + 2, &end, 16);
			if (end == nullptr || *end != '.') {
				return false;
			}
			unsigned long resolution = std::strtoul(end + 1, &end, 16);
			if (end == nullptr || *end != '.') {
				return false;
			}
			unsigned long segment = std::strtoul(end + 1, &end, 16);
			for (size_t i = 0; i < count_; ++i) {
				const Ring& r = rings_[i];
				if (!r.persistable || r.hash != hash) {
					continue;
				}
				for (uint8_t k = 0; k < r.archive_count; ++k) {
					char own[kSegmentNameSize];
					segment_name(r.hash, r.archives[k].resolution_s, static_cast<uint32_t>(segment), own, sizeof(own));
					if (r.archives[k].resolution_s == resolution && segment < r.archives[k].segments &&
						std::strcmp(own, name) == 0) {
						return true;
					}
				}
			}
			return false;
		}

	private:
		const Ring* rings_;
		size_t count_;
	};
	// One line per archive of a datapoint of the image that the image no longer has (its segments
	// are deleted below with the orphans).
	class Dropped : public BlobVisitor {
	public:
		Dropped(const Ring* rings, size_t count) : rings_(rings), count_(count) {}
		void blob(const char* name, size_t) override {
			char* end = nullptr;
			if (std::strncmp(name, kArchivePrefix, 2) != 0) {
				return;
			}
			unsigned long hash = std::strtoul(name + 2, &end, 16);
			if (end == nullptr || *end != '.') {
				return;
			}
			unsigned long resolution = std::strtoul(end + 1, &end, 16);
			for (size_t i = 0; i < count_; ++i) {
				const Ring& r = rings_[i];
				if (!r.persistable || r.hash != hash) {
					continue;
				}
				bool kept = false;
				for (uint8_t k = 0; k < r.archive_count; ++k) {
					kept = kept || r.archives[k].resolution_s == resolution;
				}
				bool seen = false;
				for (size_t j = 0; j < found_; ++j) {
					seen = seen || (seen_[j].ring == i && seen_[j].resolution == resolution);
				}
				if (!kept && !seen && found_ < kMax) {
					seen_[found_++] = Seen{i, resolution};
				}
			}
		}
		void log() const {
			for (size_t j = 0; j < found_; ++j) {
				Log::warn(kTag, "history of '%s': the %lu s archive is dropped: the image no longer has it",
					rings_[seen_[j].ring].name, seen_[j].resolution);
			}
		}

	private:
		struct Seen {
			size_t ring;
			unsigned long resolution;
		};
		enum : size_t { kMax = 8 };
		const Ring* rings_;
		size_t count_;
		Seen seen_[kMax] = {};
		size_t found_ = 0;
	};
	Dropped dropped(rings_, count_);
	store_.visit(dropped);
	dropped.log();
	persist::delete_orphans(store_, kArchivePrefix, ArchiveKeeper(rings_, count_), kTag);

	size_t free = 0;
	segments_to_come_ = missing;
	if (missing > 0 && store_.free_space(free) && missing * kSegmentBytes > free) {
		Log::warn(kTag, "history: the archives need %u more bytes of flash as they fill, %u are free",
			static_cast<unsigned>(missing * kSegmentBytes), static_cast<unsigned>(free));
	}
}

// ---- Queries ------------------------------------------------------------------------------------

bool History::has_history(uint16_t id) const {
	const Ring* r = ring(id);
	const Datapoint* dp = model_.get(id);
	return r != nullptr && dp != nullptr && std::strcmp(dp->name(), r->name) == 0;
}

int History::choose_store(const Ring& r, int64_t from, int64_t to, uint32_t points) const {
	WallTime wall = clock_.wall_time();
	int64_t width = (to - from + points - 1) / points;
	// The stores, finest first: the ring (its resolution is its spacing), then the archives.
	auto covers = [&](uint32_t retention) { return !wall.synchronized || wall.seconds - retention <= from; };
	int furthest = -2;
	uint32_t furthest_retention = 0;
	for (int k = -1; k < static_cast<int>(r.archive_count); ++k) {
		uint32_t resolution = k < 0 ? r.spacing_s : r.archives[k].resolution_s;
		uint32_t retention = k < 0 ? r.retention_s : r.archives[k].retention_s;
		if (resolution > width) {
			break;
		}
		if (covers(retention)) {
			return k;
		}
		if (furthest == -2 || retention > furthest_retention) {
			furthest = k;
			furthest_retention = retention;
		}
	}
	return furthest != -2 ? furthest : -1;
}

HistoryStatus History::store_for(uint16_t id, int64_t from, int64_t to, uint32_t points, HistoryAnswer& answer) const {
	answer = HistoryAnswer();
	if (!has_history(id)) {
		return HistoryStatus::unknown_datapoint;
	}
	if (to <= from || points == 0) {
		return HistoryStatus::malformed;
	}
	const Ring& r = *ring(id);
	answer.boolean = r.type == ValueType::boolean;
	int k = choose_store(r, from, to, points);
	answer.resolution_s = k < 0 ? 0 : r.archives[k].resolution_s;
	answer.retention_s = k < 0 ? r.retention_s : r.archives[k].retention_s;
	return HistoryStatus::ok;
}

HistoryStatus History::query(uint16_t id, int64_t from, int64_t to, uint32_t points, HistorySink& sink,
	HistoryAnswer& answer) const {
	HistoryStatus status = store_for(id, from, to, points, answer);
	if (status != HistoryStatus::ok) {
		return status;
	}
	const Ring& r = *ring(id);
	WallTime wall = clock_.wall_time();
	int k = choose_store(r, from, to, points);
	if (k < 0) {
		return query_raw(r, from, to, points, wall.seconds, wall.synchronized, sink);
	}
	return query_archive(r, r.archives[k], from, to, points, wall.synchronized ? wall.seconds : -1, sink);
}

HistoryStatus History::query_raw(const Ring& r, int64_t from, int64_t to, uint32_t points, int64_t now, bool synced,
	HistorySink& sink) const {
	int64_t width = (to - from + points - 1) / points;
	int64_t lo = std::max<int64_t>(from, 0);
	if (synced) {
		lo = std::max(lo, now - static_cast<int64_t>(r.retention_s));
	}
	auto lower = [&r](int64_t key) {
		uint32_t a = 0;
		uint32_t b = r.count;
		while (a < b) {
			uint32_t mid = a + (b - a) / 2;
			if (static_cast<int64_t>(r.at(mid).t) < key) {
				a = mid + 1;
			} else {
				b = mid;
			}
		}
		return a;
	};
	// Where a sample's value stops holding: the next sample, its spacing and the slack, now.
	int64_t max_hold = static_cast<int64_t>(r.spacing_s) + kHoldSlackS;
	auto hold_end = [&](uint32_t i) {
		int64_t t = r.at(i).t;
		int64_t end = t + max_hold;
		if (i + 1 < r.count) {
			end = std::min<int64_t>(end, r.at(i + 1).t);
		} else {
			end = synced ? std::min(end, now) : t;
		}
		return std::max(end, t);
	};
	bool boolean = r.type == ValueType::boolean;
	for (uint32_t b = 0; b < points; ++b) {
		int64_t b0 = from + static_cast<int64_t>(b) * width;
		if (b0 >= to) {
			break;
		}
		int64_t b1 = std::min(b0 + width, to);
		if (b1 <= lo) {
			continue;
		}
		uint32_t first = lower(std::max(b0, lo));
		uint32_t end = lower(b1);
		if (first == end) {
			continue; // No sample: an empty bucket is left out.
		}
		HistoryBucket out{};
		out.count = end - first;
		double samples = 0;
		for (uint32_t i = first; i < end; ++i) {
			double v = to_double(r.type, r.at(i).bits);
			out.min = i == first ? v : std::min(out.min, v);
			out.max = i == first ? v : std::max(out.max, v);
			samples += v;
		}
		out.last = to_double(r.type, r.at(end - 1).bits);
		double sum = 0;
		int64_t held = 0;
		bool carried = false;
		for (uint32_t i = first > 0 ? first - 1 : first; i < end; ++i) {
			int64_t s0 = std::max<int64_t>(r.at(i).t, b0);
			int64_t s1 = std::min(hold_end(i), b1);
			if (s1 <= s0) {
				continue;
			}
			double v = to_double(r.type, r.at(i).bits);
			sum += v * static_cast<double>(s1 - s0);
			held += s1 - s0;
			if (i < first) {
				// The value held into the bucket from the sample before it.
				carried = true;
				out.min = std::min(out.min, v);
				out.max = std::max(out.max, v);
			}
		}
		out.mean = held > 0 ? sum / static_cast<double>(held) : samples / out.count;
		out.on_s = boolean ? sum : 0;
		out.single = out.count == 1 && !carried && !boolean;
		out.t = out.single ? static_cast<int64_t>(r.at(first).t) : b0;
		if (!sink.bucket(out)) {
			break;
		}
	}
	return HistoryStatus::ok;
}

HistoryStatus History::query_archive(const Ring& r, const Archive& a, int64_t from, int64_t to, uint32_t points,
	int64_t now, HistorySink& sink) const {
	int64_t width = (to - from + points - 1) / points;
	int64_t lo = std::max<int64_t>(from, 0);
	int64_t hi = std::min<int64_t>(to, static_cast<int64_t>(UINT32_MAX) + 1);
	if (hi <= lo) {
		return HistoryStatus::ok;
	}
	// The slots whose middle may fall in [from, to), within the retention window.
	uint32_t newest = a.has_open ? a.open_number : slot_number(a, now >= 0 ? now : hi - 1);
	uint32_t oldest = newest >= a.slots - 1 ? newest - (a.slots - 1) : 0;
	uint32_t n0 = std::max(oldest, slot_number(a, lo - static_cast<int64_t>(a.resolution_s)));
	uint32_t n1 = std::min(newest, slot_number(a, hi - 1));
	bool boolean = r.type == ValueType::boolean;
	SlotReader reader(*this, r, a);
	size_t pending = 0;
	HistoryBucket out{};
	double held = 0;
	int64_t bucket = -1;
	auto emit = [&]() {
		out.mean = held > 0 ? out.on_s / held : out.last;
		if (!boolean) {
			out.on_s = 0;
		}
		return sink.bucket(out);
	};
	for (uint32_t n = n0; n <= n1 && n >= n0; ++n) {
		ArchiveSlot s;
		bool found = false;
		if (a.has_open && n == a.open_number) {
			s = a.open;
			found = true;
		} else {
			while (pending < a.pending_count &&
				slot_number(a, slot_middle(a.pending[pending].start, a.resolution_s, a.days)) < n) {
				++pending;
			}
			if (pending < a.pending_count &&
				slot_number(a, slot_middle(a.pending[pending].start, a.resolution_s, a.days)) == n) {
				s = a.pending[pending];
				found = true;
			} else if (r.persistable) {
				found = reader.get(n % a.positions, s);
			}
		}
		if (!found || !slot_valid(a, s, n)) {
			continue;
		}
		// A slot belongs to the bucket of its middle; one that reaches into [from, to) with its middle
		// outside (the open slot of a query up to now, the first slot of a range) to the nearest one.
		int64_t middle = slot_middle(s.start, a.resolution_s, a.days);
		int64_t span = 2 * (middle - static_cast<int64_t>(s.start));
		if (static_cast<int64_t>(s.start) >= to || static_cast<int64_t>(s.start) + span <= from) {
			continue;
		}
		middle = std::min<int64_t>(std::max<int64_t>(middle, from), to - 1);
		int64_t b = (middle - from) / width;
		if (b != bucket) {
			if (bucket >= 0 && !emit()) {
				return HistoryStatus::ok;
			}
			bucket = b;
			out = HistoryBucket{};
			out.t = from + b * width;
			out.min = s.min;
			out.max = s.max;
			held = 0;
		}
		out.min = std::min<double>(out.min, s.min);
		out.max = std::max<double>(out.max, s.max);
		out.last = s.last;
		out.count += s.count;
		out.on_s += s.sum; // the sum, until emit() makes the mean of it
		held += s.held_s;
		if (n == UINT32_MAX) {
			break;
		}
	}
	if (bucket >= 0) {
		emit();
	}
	return HistoryStatus::ok;
}

// ---- Accessors ----------------------------------------------------------------------------------

size_t History::archives(uint16_t id) const {
	const Ring* r = ring(id);
	return r != nullptr ? r->archive_count : 0;
}

bool History::open_slot(uint16_t id, uint32_t resolution_s, ArchiveSlot& slot) const {
	const Ring* r = ring(id);
	for (uint8_t k = 0; r != nullptr && k < r->archive_count; ++k) {
		if (r->archives[k].resolution_s == resolution_s && r->archives[k].has_open) {
			slot = r->archives[k].open;
			return true;
		}
	}
	return false;
}

} // namespace hc
