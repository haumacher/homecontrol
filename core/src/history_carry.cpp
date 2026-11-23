/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The carry-over of a history into new stores (homecontrol #115, #118; doc/persistence.md "Carrying a
// history over"): an archive the store does not have yet takes the slots the ring blob and the
// datapoint's other archives already hold for its time. load() decides which archives and datapoints
// (History::check_archives()); this runs afterwards on the event loop, one datapoint after the other,
// in steps of at most kCarryStepWork store calls and kCarryStepMs. Its writes go through the store's
// write queue only when the queue has room for them, one segment at a time, and each is read back once
// the queue has performed it; a journal written before the first of them and removed after the last
// makes a restart or another load() in the middle carry the archives over again; what the new archives
// replace is deleted only after the journal is gone.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include "history_internal.h"
#include "hc/log.h"
#include "hc/value_store.h"

namespace hc {

using history_detail::build_header;
using history_detail::get_u16;
using history_detail::get_u32;
using history_detail::kArchiveMagic0;
using history_detail::kArchiveMagic1;
using history_detail::kJournalMagic0;
using history_detail::kJournalMagic1;
using history_detail::kJournalMaxBytes;
using history_detail::kJournalPrefix;
using history_detail::kJournalVersion;
using history_detail::kMaxSegments;
using history_detail::kSegmentNameSize;
using history_detail::kTag;
using history_detail::put_u32;
using history_detail::segment_name;
using history_detail::SegmentState;
using history_detail::slot_empty;
using history_detail::slot_middle;
using history_detail::take;
using history_detail::to_double;

enum class History::CarryWait : uint8_t {
	next,  ///< The step's budget is spent: the next step after kCarryGapMs.
	poll,  ///< Waiting for the store's queue (room, a change to settle): again after kCarryPollMs.
	time,  ///< Waiting for the wall clock: again after kTickMs.
	retry, ///< A write or removal failed: the datapoint starts again at Carry::retry_at_ms.
	done,
};

namespace {

/// The first half of a segment's slots (History::Carry::kLow), the scratch of the ring's checksum and of
/// the read-back.
constexpr size_t kScratchBytes = 64 * sizeof(ArchiveSlot);
/// Store calls of the ring phase: the ring blob's size, header, checksum (kMaxBlobSize through the
/// scratch) and trailer, and its first and last sample.
constexpr uint32_t kRingCalls = 3 + (BlobStore::kMaxBlobSize + kScratchBytes - 1) / kScratchBytes + 2;
/// Store calls of a binary search over a ring blob of at most kMaxBlobSize / 8 samples.
constexpr uint32_t kLowerCalls = 13;

static_assert(kRingCalls <= History::kCarryStepWork, "the ring phase fits a step");
static_assert(kLowerCalls + 2 <= History::kCarryStepWork, "a ring search fits a step");

/// `seed` (what the stores kept up to the cut) into `live` (what was sampled since): both together.
void merge_into(ArchiveSlot& live, const ArchiveSlot& seed) {
	if (slot_empty(seed)) {
		return;
	}
	if (slot_empty(live)) {
		uint32_t start = live.start;
		live = seed;
		live.start = start;
		return;
	}
	live.min = std::min(live.min, seed.min);
	live.max = std::max(live.max, seed.max);
	live.count += seed.count;
	live.sum += seed.sum;
	live.held_s += seed.held_s;
}

/// The journal of `ring`'s new archives `resolutions`; returns its size.
size_t build_journal(uint8_t* j, uint32_t hash, const uint32_t* resolutions, uint8_t count) {
	j[0] = kJournalMagic0;
	j[1] = kJournalMagic1;
	j[2] = kJournalVersion;
	j[3] = count;
	put_u32(j + 4, hash);
	for (uint8_t i = 0; i < count; ++i) {
		put_u32(j + 8 + 4 * i, resolutions[i]);
	}
	size_t size = 12u + 4u * count;
	put_u32(j + size - 4, persist::crc32(0, j, size - 4));
	return size;
}

} // namespace

void History::carry_start(size_t jobs) {
	carry_stats_.histories = static_cast<uint32_t>(jobs);
	Log::info(kTag, "history: %u histor%s get new stores: what was kept is carried over in steps",
		static_cast<unsigned>(jobs), jobs == 1 ? "y" : "ies");
	carry_timer_ = loop_.schedule_at(clock_.monotonic_ms(),
		Callback::member<History, &History::carry_step>(this, "history.carry"));
	if (!carry_timer_.valid()) {
		carry_abort("no timer for its steps");
	}
}

void History::carry_abort(const char* why) {
	loop_.cancel(carry_timer_);
	carry_timer_ = TimerHandle();
	// The archives it had not finished stay unsaved (Archive::carry): the next load carries them over
	// again from what the store holds; the rings and the other archives are saved as usual again.
	for (size_t i = 0; i < count_; ++i) {
		rings_[i].carrying = false;
	}
	Log::warn(kTag, "history: the carry-over into the new stores stops (%s); the next load does it again", why);
	delete carry_;
	carry_ = nullptr;
}

void History::carry_end() {
	Log::info(kTag, "history: the carry-over into the new stores is done (%u steps, at most %u store calls each)",
		static_cast<unsigned>(carry_stats_.steps), static_cast<unsigned>(carry_stats_.max_step_work));
	delete carry_;
	carry_ = nullptr;
}

bool History::carry_afford(const Carry& c, uint32_t calls) const {
	if (c.work + calls > kCarryStepWork) {
		return false;
	}
	return c.work == 0 || clock_.monotonic_ms() - c.step_start_ms < kCarryStepMs;
}

void History::carry_step() {
	carry_timer_ = TimerHandle();
	if (carry_ == nullptr) {
		return;
	}
	Carry& c = *carry_;
	c.work = 0;
	c.step_start_ms = clock_.monotonic_ms();
	CarryWait wait = carry_run(c);
	++carry_stats_.steps;
	carry_stats_.work += c.work;
	carry_stats_.max_step_work = std::max(carry_stats_.max_step_work, c.work);
	if (wait == CarryWait::done) {
		carry_end();
		return;
	}
	uint64_t now = clock_.monotonic_ms();
	uint64_t at = now + kCarryGapMs;
	switch (wait) {
		case CarryWait::poll:
			at = now + kCarryPollMs;
			++carry_stats_.queue_waits;
			break;
		case CarryWait::time: at = now + kTickMs; break;
		case CarryWait::retry: at = std::max(c.retry_at_ms, now); break;
		default: break;
	}
	carry_timer_ = loop_.schedule_at(at, Callback::member<History, &History::carry_step>(this, "history.carry"));
	if (!carry_timer_.valid()) {
		carry_abort("no timer for its next step");
	}
}

History::CarryWait History::carry_run(Carry& c) {
	using Phase = Carry::Phase;
	static_assert(kScratchBytes == Carry::kLow * sizeof(ArchiveSlot), "the scratch is the first half");
	for (;;) {
		if (c.job >= c.job_count) {
			return CarryWait::done;
		}
		Carry::Job& job = c.job_at(c.job);
		Ring& r = rings_[job.ring];

		auto failed = [&](const char* what, BlobStatus status) {
			if (!job.failed_logged) {
				Log::error(kTag, "history of '%s': carrying it over into the new stores failed (%s: %s); "
					"it starts again in %u s, what was kept stays", r.name, what, blob_status_name(status),
					static_cast<unsigned>(kCarryRetryMs / 1000));
				job.failed_logged = true;
			}
			++carry_stats_.retries;
			c.phase = Phase::start;
			c.retry_at_ms = clock_.monotonic_ms() + kCarryRetryMs;
			return CarryWait::retry;
		};
		auto in_image = [&r](uint32_t resolution) {
			for (uint8_t k = 0; k < r.archive_count; ++k) {
				if (r.archives[k].resolution_s == resolution) {
					return true;
				}
			}
			return false;
		};
		auto leftover = [&c](uint32_t resolution) {
			for (uint8_t i = 0; i < c.leftover_count; ++i) {
				if (c.leftovers[i] == resolution) {
					return true;
				}
			}
			return false;
		};
		auto journal_name = [&r](char* name) { persist::blob_name(kJournalPrefix, r.name, name); };
		// A sample of the ring blob through a window of kRingWindow (one store call per window).
		auto ring_at = [&](uint32_t i, HistorySample& out) {
			if (i >= c.blob.samples) {
				return false;
			}
			if (i < c.window_base || i >= c.window_base + c.window_count) {
				c.window_base = i;
				c.window_count = 0;
				if (c.blob.sample_size == sizeof(HistorySample)) {
					uint32_t n = std::min(Carry::kRingWindow, c.blob.samples - i);
					size_t length = 0;
					BlobStatus status = store_.read(c.blob.name, c.blob.data + static_cast<size_t>(i) * sizeof(HistorySample),
						c.window, n * sizeof(HistorySample), length);
					++c.work;
					if (status != BlobStatus::ok || length != n * sizeof(HistorySample)) {
						return false;
					}
					c.window_count = n;
				} else {
					bool ok = ring_blob_sample(c.blob, i, c.window[0]);
					++c.work;
					if (!ok) {
						return false;
					}
					c.window_count = 1;
				}
			}
			out = c.window[i - c.window_base];
			return true;
		};
		// The first sample at or after `t`.
		auto ring_lower = [&](int64_t t) {
			uint32_t lo = 0;
			uint32_t hi = c.blob.samples;
			while (lo < hi) {
				uint32_t mid = lo + (hi - lo) / 2;
				HistorySample s;
				if (!ring_at(mid, s)) {
					return c.blob.samples;
				}
				if (static_cast<int64_t>(s.t) < t) {
					lo = mid + 1;
				} else {
					hi = mid;
				}
			}
			return lo;
		};
		auto use_reader = [&](int source) {
			if (c.reader_source != source) {
				c.reader.reset(r, c.sources[source]);
				c.reader_source = source;
			}
		};

		switch (c.phase) {
			case Phase::start: {
				c.leftover_count = 0;
				c.index = 0;
				c.segment = 0;
				c.ring = false;
				c.ring_from = INT64_MAX;
				c.source_count = 0;
				c.scanning = false;
				c.target = 0;
				c.target_open = false;
				c.in_slot = false;
				c.reader_source = -2;
				c.window_count = 0;
				if (!carry_afford(c, 2)) {
					return CarryWait::next;
				}
				// The archives an unfinished carry-over named that this image no longer has: deleted first, so
				// that no journal of ours forgets them while they are incomplete.
				uint32_t unfinished[kMaxArchives] = {};
				uint8_t count = 0;
				bool damaged = false;
				bool journal = read_journal(r, unfinished, count, damaged);
				c.work += journal ? 2 : 1;
				job.journal = job.journal || journal;
				for (uint8_t i = 0; i < count; ++i) {
					if (!in_image(unfinished[i]) && !leftover(unfinished[i])) {
						c.leftover_masks[c.leftover_count] = 0;
						c.leftovers[c.leftover_count++] = unfinished[i];
					}
				}
				c.phase = Phase::leftovers;
				break;
			}

			case Phase::leftovers: {
				while (c.index < c.leftover_count) {
					while (c.segment < kMaxSegments) {
						if (!carry_afford(c, 1)) {
							return CarryWait::next;
						}
						if (!store_.has_room(0)) {
							return CarryWait::poll;
						}
						char name[kSegmentNameSize];
						segment_name(r.hash, c.leftovers[c.index], c.segment, name, sizeof(name));
						BlobStatus status = store_.remove(name);
						++c.work;
						if (status == BlobStatus::ok) {
							c.leftover_masks[c.index] |= 1u << c.segment;
						} else if (status != BlobStatus::not_found) {
							return failed("an archive an unfinished carry-over left cannot be deleted", status);
						}
						++c.segment;
					}
					if (c.leftover_masks[c.index] != 0) {
						Log::info(kTag, "history of '%s': the %u s archive an unfinished carry-over left is deleted", r.name,
							static_cast<unsigned>(c.leftovers[c.index]));
					}
					++c.index;
					c.segment = 0;
				}
				c.index = 0;
				c.segment = 0;
				c.phase = Phase::leftovers_check;
				break;
			}

			case Phase::leftovers_check: {
				while (c.index < c.leftover_count) {
					while (c.segment < kMaxSegments) {
						if ((c.leftover_masks[c.index] & (1u << c.segment)) != 0) {
							char name[kSegmentNameSize];
							segment_name(r.hash, c.leftovers[c.index], c.segment, name, sizeof(name));
							if (!store_.settled(name)) {
								return CarryWait::poll;
							}
							if (!carry_afford(c, 1)) {
								return CarryWait::next;
							}
							size_t size = 0;
							BlobStatus status = store_.size(name, size);
							++c.work;
							if (status != BlobStatus::not_found) {
								return failed("an archive an unfinished carry-over left is still there", status);
							}
						}
						++c.segment;
					}
					++c.index;
					c.segment = 0;
				}
				c.phase = Phase::ring;
				break;
			}

			case Phase::ring: {
				if (c.low == nullptr || c.high == nullptr) {
					// The segment's slots, in two blocks of 2 KB (homecontrol #71: no large block).
					c.low = c.low != nullptr ? c.low : new (std::nothrow) ArchiveSlot[Carry::kLow];
					c.high = c.high != nullptr ? c.high : new (std::nothrow) ArchiveSlot[Carry::kHigh];
					if (c.low == nullptr || c.high == nullptr) {
						if (!c.memory_logged) {
							Log::warn(kTag, "history: no memory for the carry-over into the new stores (2 blocks of %u bytes); "
								"trying again in %u s", static_cast<unsigned>(kScratchBytes),
								static_cast<unsigned>(kCarryRetryMs / 1000));
							c.memory_logged = true;
						}
						c.retry_at_ms = clock_.monotonic_ms() + kCarryRetryMs;
						return CarryWait::retry;
					}
					c.clear_slots();
				}
				if (!carry_afford(c, kRingCalls)) {
					return CarryWait::next;
				}
				uint32_t calls = 0;
				c.ring = ring_blob(r, c.blob, false, reinterpret_cast<uint8_t*>(c.low), kScratchBytes, &calls) &&
					c.blob.samples > 0;
				c.work += calls;
				c.ring_spacing = c.blob.spacing_s != 0 ? c.blob.spacing_s : r.spacing_s;
				c.window_count = 0;
				c.clear_slots(); // the checksum went through them
				HistorySample first{};
				HistorySample last{};
				if (c.ring && ring_at(0, first) && ring_at(c.blob.samples - 1, last)) {
					c.ring_from = first.t;
					c.ring_last = last.t;
				} else {
					c.ring = false;
				}
				c.phase = Phase::cut;
				break;
			}

			case Phase::cut: {
				if (!c.cut_known) {
					// Loaded before the clock was synchronized: the stores count up to the ring's last sample,
					// or (without one) up to the first sample taken since (the live ones come after it).
					WallTime wall = clock_.wall_time();
					if (c.ring) {
						c.cut = c.ring_last;
					} else if (wall.synchronized && wall.seconds > 0 && wall.seconds <= static_cast<int64_t>(UINT32_MAX)) {
						c.cut = first_sample_t_ != 0 ? std::min<int64_t>(first_sample_t_, wall.seconds) : wall.seconds;
					} else {
						if (!job.time_logged) {
							Log::info(kTag, "history of '%s': the carry-over into the new stores waits for the time", r.name);
							job.time_logged = true;
						}
						return CarryWait::time;
					}
					c.cut_known = true;
				}
				for (uint8_t k = 0; k < r.archive_count; ++k) {
					Archive& a = r.archives[k];
					if (a.carry) {
						a.carry_number = slot_number(a, c.cut);
					}
				}
				c.index = 0;
				c.phase = Phase::headers;
				break;
			}

			case Phase::headers: {
				// The sources: the datapoint's archives the store had at the load that are finer than a new
				// one (those of the image and those it no longer has), not one of the new ones.
				uint32_t coarsest = 0;
				for (uint8_t k = 0; k < r.archive_count; ++k) {
					if (r.archives[k].carry) {
						coarsest = std::max(coarsest, r.archives[k].resolution_s);
					}
				}
				while (c.index < job.found_count) {
					uint32_t resolution = job.found_resolution[c.index];
					uint32_t found_segment = job.found_segment[c.index];
					bool target = false;
					for (uint8_t k = 0; k < r.archive_count; ++k) {
						target = target || (r.archives[k].carry && r.archives[k].resolution_s == resolution);
					}
					if (resolution < coarsest && !target && !leftover(resolution) && c.source_count < Carry::kMaxSources) {
						if (!carry_afford(c, 3)) {
							return CarryWait::next;
						}
						char name[kSegmentNameSize];
						segment_name(r.hash, resolution, found_segment, name, sizeof(name));
						uint8_t h[kSegmentHeaderBytes];
						size_t length = 0;
						BlobStatus status = store_.read(name, 0, h, sizeof(h), length);
						++c.work;
						if (status == BlobStatus::ok && length == sizeof(h) && h[0] == kArchiveMagic0 &&
							h[1] == kArchiveMagic1 && get_u32(h + 28) == persist::crc32(0, h, 28)) {
							Archive& s = c.sources[c.source_count];
							s = Archive();
							s.resolution_s = resolution;
							s.retention_s = get_u32(h + 8);
							s.segments = get_u16(h + 20);
							s.positions = s.segments * kSlotsPerSegment;
							s.slots = std::min(ring_samples(s.retention_s, s.resolution_s), s.positions);
							s.days = h[17] != 0 && s.resolution_s % 86400 == 0 ? s.resolution_s / 86400 : 0;
							if (s.segments > 0 && s.slots > 0 && (h[17] != 0) == (s.days > 0)) {
								SegmentState state = segment_state(r, s, found_segment, nullptr);
								c.work += 2;
								if (state == SegmentState::ok || state == SegmentState::retention_changed) {
									++c.source_count;
								}
							}
						}
					}
					++c.index;
				}
				c.index = 0;
				c.scanning = false;
				c.reader_source = -2;
				c.phase = Phase::reach;
				break;
			}

			case Phase::reach: {
				// How far back each source holds: its oldest valid slot of the window up to the cut.
				while (c.index < c.source_count) {
					Archive& s = c.sources[c.index];
					if (!c.scanning) {
						c.scan_end = slot_number(s, c.cut);
						c.scan = c.scan_end >= s.slots - 1 ? c.scan_end - (s.slots - 1) : 0;
						s.from = INT64_MAX;
						c.scanning = true;
					}
					use_reader(static_cast<int>(c.index));
					while (c.scanning) {
						if (!carry_afford(c, SlotReader::kMaxCalls)) {
							return CarryWait::next;
						}
						ArchiveSlot slot;
						if (c.reader.get(c.scan % s.positions, slot) && slot_valid(s, slot, c.scan)) {
							s.from = slot.start;
							c.scanning = false;
						} else if (c.scan >= c.scan_end) {
							c.scanning = false;
						} else {
							++c.scan;
						}
					}
					++c.index;
				}
				size_t usable = 0;
				for (size_t i = 0; i < c.source_count; ++i) {
					if (c.sources[i].from != INT64_MAX) {
						c.sources[usable++] = c.sources[i];
					}
				}
				c.source_count = usable;
				// Finest first (an insertion sort of at most kMaxSources).
				for (size_t i = 1; i < c.source_count; ++i) {
					for (size_t j = i; j > 0 && c.sources[j].resolution_s < c.sources[j - 1].resolution_s; --j) {
						std::swap(c.sources[j], c.sources[j - 1]);
					}
				}
				c.reader_source = -2;
				c.target = 0;
				c.target_open = false;
				c.phase = Phase::target;
				break;
			}

			case Phase::target: {
				if (!c.target_open) {
					while (c.target < r.archive_count && !r.archives[c.target].carry) {
						++c.target;
					}
					if (c.target >= r.archive_count) {
						c.phase = Phase::finish_journal;
						break;
					}
					Archive& a = r.archives[c.target];
					c.target_ring = c.ring && c.ring_spacing < a.resolution_s;
					bool any = c.target_ring;
					for (size_t i = 0; i < c.source_count; ++i) {
						any = any || c.sources[i].resolution_s < a.resolution_s;
					}
					if (!any) {
						++c.target; // nothing kept for its time: it starts empty
						break;
					}
					c.newest = slot_number(a, c.cut);
					uint32_t span = std::min(a.slots, a.positions) - 1;
					c.oldest = c.newest >= span ? c.newest - span : 0;
					c.seg_step = 0;
					c.slot = 0;
					c.filled = 0;
					c.slots_total = 0;
					c.in_slot = false;
					c.clear_slots();
					c.target_open = true;
				}
				Archive& a = r.archives[c.target];
				// The segments newest first: the recent slots, which the charts ask for most, come first.
				uint32_t first = (c.newest % a.positions) / kSlotsPerSegment;
				bool write = false;
				while (c.seg_step < a.segments && !write) {
					c.seg = (first + a.segments - c.seg_step) % a.segments;
					while (c.slot < kSlotsPerSegment) {
						if (!c.in_slot) {
							uint32_t p = c.seg * kSlotsPerSegment + c.slot;
							uint32_t back = (c.newest % a.positions + a.positions - p) % a.positions;
							if (back > c.newest - c.oldest) {
								++c.slot;
								continue;
							}
							uint32_t n = c.newest - back;
							int64_t s0 = slot_start(a, n);
							int64_t s1 = slot_start(a, n + 1);
							if (s0 <= 0 || s1 <= s0) {
								++c.slot;
								continue;
							}
							// Each slot from the finest source that reaches back to its start, else from the one
							// that reaches furthest (as a query chooses its store).
							int chosen = -2;
							int64_t furthest = INT64_MAX;
							if (c.target_ring && c.ring_from <= s0) {
								chosen = -1;
							}
							for (size_t k = 0; k < c.source_count && chosen == -2; ++k) {
								if (c.sources[k].resolution_s < a.resolution_s && c.sources[k].from <= s0) {
									chosen = static_cast<int>(k);
								}
							}
							if (chosen == -2) {
								if (c.target_ring && c.ring_from < s1) {
									chosen = -1;
									furthest = c.ring_from;
								}
								for (size_t k = 0; k < c.source_count; ++k) {
									if (c.sources[k].resolution_s < a.resolution_s && c.sources[k].from < s1 &&
										c.sources[k].from < furthest) {
										chosen = static_cast<int>(k);
										furthest = c.sources[k].from;
									}
								}
							}
							if (chosen == -2) {
								++c.slot;
								continue;
							}
							c.chosen = chosen;
							c.s0 = s0;
							c.s1 = s1;
							c.acc = ArchiveSlot{};
							if (chosen == -1) {
								c.k = UINT32_MAX;
								c.previous_t = -1;
							} else {
								c.m = slot_number(c.sources[chosen], s0);
								c.m1 = slot_number(c.sources[chosen], s1 - 1);
							}
							c.in_slot = true;
						}
						if (c.chosen == -1) {
							// The ring's samples over [s0, s1) as feed() and hold() consolidate them.
							if (c.k == UINT32_MAX) {
								if (!carry_afford(c, kLowerCalls)) {
									return CarryWait::next;
								}
								uint32_t lower = ring_lower(c.s0);
								c.k = lower > 0 ? lower - 1 : 0;
							}
							while (c.k < c.blob.samples) {
								if (!carry_afford(c, 2)) {
									return CarryWait::next;
								}
								HistorySample s;
								if (!ring_at(c.k, s) || static_cast<int64_t>(s.t) >= c.s1) {
									c.k = c.blob.samples;
									break;
								}
								if (static_cast<int64_t>(s.t) <= c.previous_t) {
									++c.k;
									continue; // not increasing: a blob the restore would not take either
								}
								c.previous_t = s.t;
								float value = static_cast<float>(to_double(r.type, s.bits));
								if (static_cast<int64_t>(s.t) >= c.s0) {
									take(c.acc, value);
									++c.acc.count;
								}
								HistorySample next;
								int64_t end = c.k + 1 < c.blob.samples && ring_at(c.k + 1, next) ? static_cast<int64_t>(next.t)
									: c.cut;
								end = std::min({end, static_cast<int64_t>(s.t) + c.ring_spacing + kHoldSlackS, c.s1, c.cut});
								int64_t begin = std::max<int64_t>(s.t, c.s0);
								if (end > begin) {
									take(c.acc, value);
									c.acc.sum += static_cast<double>(value) * static_cast<double>(end - begin);
									c.acc.held_s += static_cast<uint32_t>(end - begin);
								}
								++c.k;
							}
						} else {
							// The slots of a finer archive whose middle lies in [s0, s1).
							const Archive& src = c.sources[c.chosen];
							use_reader(c.chosen);
							while (c.m <= c.m1) {
								if (!carry_afford(c, SlotReader::kMaxCalls)) {
									return CarryWait::next;
								}
								ArchiveSlot slot;
								if (c.reader.get(c.m % src.positions, slot) && slot_valid(src, slot, c.m)) {
									int64_t middle = slot_middle(slot.start, src.resolution_s, src.days);
									if (middle >= c.s0 && middle < c.s1) {
										if (slot_empty(c.acc)) {
											c.acc.min = slot.min;
											c.acc.max = slot.max;
										} else {
											c.acc.min = std::min(c.acc.min, slot.min);
											c.acc.max = std::max(c.acc.max, slot.max);
										}
										c.acc.last = slot.last;
										c.acc.count += slot.count;
										c.acc.sum += slot.sum;
										c.acc.held_s += slot.held_s;
									}
								}
								if (c.m == c.m1) {
									c.m = 1; // done (m1 may be UINT32_MAX)
									c.m1 = 0;
								} else {
									++c.m;
								}
							}
						}
						if (!slot_empty(c.acc) && std::isfinite(c.acc.sum)) {
							c.acc.start = static_cast<uint32_t>(c.s0);
							c.slot_at(c.slot) = c.acc;
							++c.filled;
						}
						c.in_slot = false;
						++c.slot;
					}
					if (c.filled > 0) {
						write = true;
					} else {
						++c.seg_step;
						c.slot = 0;
					}
				}
				if (write) {
					c.phase = job.journal_written ? Phase::segment_write : Phase::journal_write;
					break;
				}
				if (c.slots_total > 0) {
					Log::info(kTag, "history of '%s': the %u s archive starts with %u slots consolidated from what was kept before",
						r.name, static_cast<unsigned>(a.resolution_s), static_cast<unsigned>(c.slots_total));
				}
				c.target_open = false;
				++c.target;
				break;
			}

			case Phase::journal_write: {
				// Before the first new segment: a restart from here on carries the new archives over again.
				uint32_t resolutions[kMaxArchives] = {};
				uint8_t count = 0;
				for (uint8_t k = 0; k < r.archive_count; ++k) {
					if (r.archives[k].carry) {
						resolutions[count++] = r.archives[k].resolution_s;
					}
				}
				uint8_t j[kJournalMaxBytes];
				size_t size = build_journal(j, r.hash, resolutions, count);
				if (!store_.has_room(size)) {
					return CarryWait::poll;
				}
				if (!carry_afford(c, 1)) {
					return CarryWait::next;
				}
				char name[persist::kBlobNameSize];
				journal_name(name);
				BlobChunk chunk{j, size};
				BlobStatus status = store_.write(name, &chunk, 1);
				++c.work;
				if (status != BlobStatus::ok) {
					return failed("its journal cannot be written", status);
				}
				job.journal = true;
				c.phase = Phase::journal_check;
				break;
			}

			case Phase::journal_check: {
				char name[persist::kBlobNameSize];
				journal_name(name);
				if (!store_.settled(name)) {
					return CarryWait::poll;
				}
				if (!carry_afford(c, 1)) {
					return CarryWait::next;
				}
				uint32_t resolutions[kMaxArchives] = {};
				uint8_t count = 0;
				for (uint8_t k = 0; k < r.archive_count; ++k) {
					if (r.archives[k].carry) {
						resolutions[count++] = r.archives[k].resolution_s;
					}
				}
				uint8_t want[kJournalMaxBytes];
				size_t size = build_journal(want, r.hash, resolutions, count);
				uint8_t got[kJournalMaxBytes];
				size_t length = 0;
				BlobStatus status = store_.read(name, 0, got, sizeof(got), length);
				++c.work;
				if (status != BlobStatus::ok || length != size || std::memcmp(got, want, size) != 0) {
					return failed("its journal did not reach the store", status);
				}
				job.journal_written = true;
				c.phase = Phase::segment_write;
				break;
			}

			case Phase::segment_write: {
				Archive& a = r.archives[c.target];
				// The slot of the cut: what was kept up to it and what was sampled live since, once.
				uint32_t cut_start = static_cast<uint32_t>(slot_start(a, a.carry_number));
				for (uint32_t j = 0; j < kSlotsPerSegment; ++j) {
					ArchiveSlot& seed = c.slot_at(j);
					if (slot_empty(seed) || seed.start != cut_start) {
						continue;
					}
					ArchiveSlot* live = a.has_open && a.open.start == seed.start ? &a.open : nullptr;
					for (uint16_t p = 0; p < a.pending_count && live == nullptr; ++p) {
						if (a.pending[p].start == seed.start) {
							live = &a.pending[p];
						}
					}
					if (live != nullptr) {
						if (!a.merged) {
							merge_into(*live, seed);
							if (live == &a.open) {
								a.open_dirty = true;
							}
						}
						seed = *live; // after a retry the live slot holds the merge already
					}
					a.merged = true;
				}
				if (!store_.has_room(kSegmentBytes)) {
					return CarryWait::poll;
				}
				if (!carry_afford(c, 2)) {
					return CarryWait::next;
				}
				char name[kSegmentNameSize];
				segment_name(r.hash, a.resolution_s, c.seg, name, sizeof(name));
				size_t size = 0;
				bool absent = store_.size(name, size) == BlobStatus::not_found;
				++c.work;
				uint8_t header[kSegmentHeaderBytes];
				build_header(header, r.hash, r.type, a.resolution_s, a.retention_s, a.days, static_cast<uint16_t>(c.seg),
					a.segments);
				BlobChunk chunks[3] = {{header, sizeof(header)}, {c.low, Carry::kLow * sizeof(ArchiveSlot)},
					{c.high, Carry::kHigh * sizeof(ArchiveSlot)}};
				BlobStatus status = store_.write(name, chunks, 3);
				++c.work;
				if (status != BlobStatus::ok) {
					return failed("a new segment cannot be written", status);
				}
				++archive_writes_;
				if (absent && segments_to_come_ > 0) {
					--segments_to_come_;
				}
				c.crc = persist::crc32(persist::crc32(persist::crc32(0, header, sizeof(header)), c.low,
					Carry::kLow * sizeof(ArchiveSlot)), c.high, Carry::kHigh * sizeof(ArchiveSlot));
				c.verify_crc = 0;
				c.verify_offset = 0;
				c.phase = Phase::segment_check;
				break;
			}

			case Phase::segment_check: {
				// Durable before anything it replaces may go: the queue performed it, and it reads back.
				Archive& a = r.archives[c.target];
				char name[kSegmentNameSize];
				segment_name(r.hash, a.resolution_s, c.seg, name, sizeof(name));
				if (!store_.settled(name)) {
					return CarryWait::poll;
				}
				while (c.verify_offset < kSegmentBytes) {
					if (!carry_afford(c, 1)) {
						return CarryWait::next;
					}
					// Into the first half: the queue has its own copy of what was written.
					uint8_t* chunk = reinterpret_cast<uint8_t*>(c.low);
					size_t want = std::min(kScratchBytes, kSegmentBytes - c.verify_offset);
					size_t length = 0;
					BlobStatus status = store_.read(name, c.verify_offset, chunk, want, length);
					++c.work;
					if (status != BlobStatus::ok || length != want) {
						return failed("a new segment did not reach the store", status);
					}
					c.verify_crc = persist::crc32(c.verify_crc, chunk, length);
					c.verify_offset += length;
				}
				if (c.verify_crc != c.crc) {
					return failed("a new segment did not reach the store", BlobStatus::storage_error);
				}
				c.slots_total += c.filled;
				++c.seg_step;
				c.slot = 0;
				c.filled = 0;
				c.clear_slots();
				c.phase = Phase::target;
				break;
			}

			case Phase::finish_journal: {
				// Every new segment is durable: the new archives are complete, the journal goes ...
				if (!job.journal) {
					c.index = 0;
					c.segment = 0;
					c.phase = Phase::finish_sources;
					break;
				}
				if (!store_.has_room(0)) {
					return CarryWait::poll;
				}
				if (!carry_afford(c, 1)) {
					return CarryWait::next;
				}
				char name[persist::kBlobNameSize];
				journal_name(name);
				BlobStatus status = store_.remove(name);
				++c.work;
				if (status != BlobStatus::ok && status != BlobStatus::not_found) {
					return failed("its journal cannot be deleted", status);
				}
				c.phase = Phase::finish_journal_check;
				break;
			}

			case Phase::finish_journal_check: {
				char name[persist::kBlobNameSize];
				journal_name(name);
				if (!store_.settled(name)) {
					return CarryWait::poll;
				}
				if (!carry_afford(c, 1)) {
					return CarryWait::next;
				}
				size_t size = 0;
				BlobStatus status = store_.size(name, size);
				++c.work;
				if (status != BlobStatus::not_found) {
					return failed("its journal is still there", status);
				}
				job.journal = false;
				job.journal_written = false;
				c.index = 0;
				c.segment = 0;
				c.phase = Phase::finish_sources;
				break;
			}

			case Phase::finish_sources: {
				// ... and then the archives the image no longer has (a restart before their removals are done
				// finds them as orphans of the load).
				while (c.index < job.found_count) {
					uint32_t resolution = job.found_resolution[c.index];
					if (!in_image(resolution) && !leftover(resolution)) {
						while (c.segment < kMaxSegments) {
							if (!store_.has_room(0)) {
								return CarryWait::poll;
							}
							if (!carry_afford(c, 1)) {
								return CarryWait::next;
							}
							char name[kSegmentNameSize];
							segment_name(r.hash, resolution, c.segment, name, sizeof(name));
							store_.remove(name); // a failure leaves an orphan, which the next load deletes
							++c.work;
							++c.segment;
						}
						Log::warn(kTag, "history of '%s': the %u s archive is dropped: the image no longer has it", r.name,
							static_cast<unsigned>(resolution));
					}
					++c.index;
					c.segment = 0;
				}
				// Done: the datapoint's ring and archives are saved as usual from now on.
				r.carrying = false;
				for (uint8_t k = 0; k < r.archive_count; ++k) {
					r.archives[k].carry = false;
					r.archives[k].stale = false;
				}
				if (job.failed_logged) {
					Log::info(kTag, "history of '%s': carried over into the new stores", r.name);
				}
				++c.job;
				c.cut_known = c.load_cut_known;
				c.cut = c.load_cut;
				c.phase = Phase::start;
				break;
			}
		}
	}
}

} // namespace hc
