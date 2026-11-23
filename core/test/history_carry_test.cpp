/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Carrying a history into new stores runs in steps after the load (homecontrol #118): on the desk board
// one datapoint's carry-over held the loop for 1.5 s in the apply step, its segments written on the loop
// because the write queue was full. Here the stores of 20 datapoints change at once behind a write
// queue of the C6's size: no step makes more than History::kCarryStepWork store calls, nothing goes to
// the store on the loop, every slot is carried over, and what the new archives replace goes only after
// they are durable. A power cut and another load in the middle leave the old or the new stores readable
// and end in the same archives as a carry-over that ran through.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "allocation_counter.h"
#include "doctest.h"
#include "fake_blob_store.h"
#include "fake_worker.h"
#include "hc/blob_queue.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/history.h"
#include "hc/technology.h"
#include "hc/value_store.h"
#include "test_support.h"

using hc::ArchiveSlot;
using hc::ArchiveSpec;
using hc::BlobChunk;
using hc::BlobStatus;
using hc::DatapointFlags;
using hc::DatapointModel;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::History;
using hc::HistoryAnswer;
using hc::HistoryBucket;
using hc::HistoryLimits;
using hc::HistorySample;
using hc::HistorySpec;
using hc::HistoryStatus;
using hc::Value;
using hc::ValueType;
using hc_test::FakeBlobStore;
using hc_test::FakeClock;
using hc_test::ScopedCapture;

namespace {

constexpr uint8_t kH = DatapointFlags::kHistory;
/// 2026-10-26 00:00:00 UTC.
constexpr int64_t kMonday = 1792972800;
constexpr uint32_t kDay = 86400;
/// The load: half past midnight of the fourth day.
constexpr int64_t kNow = kMonday + 3 * kDay + 1800;

class Bus : public hc::Technology {
public:
	const char* name() const override { return "bus"; }
	bool command(const hc::Datapoint&, const Value&) override { return true; }
};

struct Buckets : hc::HistorySink {
	bool bucket(const HistoryBucket& b) override {
		items.push_back(b);
		return true;
	}
	std::vector<HistoryBucket> items;
};

/// The store calls of the history, counted per loop turn (the test hook of the bound), in front of the
/// store it is given.
class Counting : public hc::BlobStore {
public:
	explicit Counting(hc::BlobStore& inner) : inner_(inner) {}
	BlobStatus write(const char* name, const BlobChunk* chunks, size_t count) override {
		++calls;
		return inner_.write(name, chunks, count);
	}
	BlobStatus read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const override {
		++calls;
		return inner_.read(name, offset, buffer, size, length);
	}
	BlobStatus size(const char* name, size_t& size) const override {
		++calls;
		return inner_.size(name, size);
	}
	BlobStatus remove(const char* name) override {
		++calls;
		return inner_.remove(name);
	}
	void visit(hc::BlobVisitor& visitor) const override {
		++calls;
		inner_.visit(visitor);
	}
	BlobStatus patch(const char* name, size_t offset, const BlobChunk* chunks, size_t count) override {
		++calls;
		return inner_.patch(name, offset, chunks, count);
	}
	bool free_space(size_t& bytes) const override { return inner_.free_space(bytes); }
	bool has_room(size_t bytes) const override { return inner_.has_room(bytes); }
	bool settled(const char* name) const override { return inner_.settled(name); }

	mutable uint32_t calls = 0;

private:
	hc::BlobStore& inner_;
};

/// The flash behind the queue: what reached it, in order, and what reached it on the loop (the queue's
/// fallback when it is full) rather than from its worker.
class Flash : public FakeBlobStore {
public:
	BlobStatus write(const char* name, const BlobChunk* chunks, size_t count) override {
		BlobStatus status = FakeBlobStore::write(name, chunks, count);
		if (status == BlobStatus::ok) {
			record(std::string("w ") + name);
		}
		return status;
	}
	BlobStatus remove(const char* name) override {
		BlobStatus status = FakeBlobStore::remove(name);
		if (status == BlobStatus::ok) {
			record(std::string("r ") + name);
		}
		return status;
	}
	BlobStatus patch(const char* name, size_t offset, const BlobChunk* chunks, size_t count) override {
		BlobStatus status = FakeBlobStore::patch(name, offset, chunks, count);
		if (status == BlobStatus::ok) {
			record(std::string("p ") + name);
		}
		return status;
	}
	void record(const std::string& op) {
		log.push_back(op);
		if (on_loop) {
			loop_log.push_back(op);
		}
	}
	std::vector<std::string> log;
	std::vector<std::string> loop_log;
	bool on_loop = false;
};

std::string name_of(int i) {
	char name[32];
	std::snprintf(name, sizeof(name), "Room%02d.Temp", i);
	return name;
}

uint32_t hash_of(const std::string& name) {
	return hc::persist::fnv1a(name.c_str());
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
	for (int i = 0; i < 4; ++i) {
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

uint32_t float_bits(float f) {
	uint32_t bits = 0;
	std::memcpy(&bits, &f, sizeof(bits));
	return bits;
}

/// The hour of the day: a sawtooth 0..23.
float hour_value(int64_t t) {
	return static_cast<float>((t / 3600) % 24);
}

/// The day number modulo 7: every week averages 3.
float day_value(int64_t t) {
	return static_cast<float>((t / kDay) % 7);
}

/// The ring of the old image in its frozen format (doc/persistence.md): 24 h at 1 min up to `now`.
void old_ring(FakeBlobStore& blobs, const std::string& name, int64_t now) {
	std::vector<uint8_t> fields = {1, static_cast<uint8_t>(name.size())};
	fields.insert(fields.end(), name.begin(), name.end());
	for (uint8_t b : {2, 1, static_cast<int>(ValueType::real), 3, 4}) {
		fields.push_back(b);
	}
	put_u32(fields, 60);
	fields.push_back(4);
	fields.push_back(4);
	put_u32(fields, kDay);
	for (uint8_t b : {5, 1, 8, 6, 4}) {
		fields.push_back(b);
	}
	uint32_t count = kDay / 60;
	put_u32(fields, count);
	std::vector<uint8_t> blob = {'H', 'H', 1, static_cast<uint8_t>(fields.size()), static_cast<uint8_t>(fields.size() >> 8)};
	blob.insert(blob.end(), fields.begin(), fields.end());
	for (int64_t t = now - kDay; t < now; t += 60) {
		put_u32(blob, static_cast<uint32_t>(t));
		put_u32(blob, float_bits(hour_value(t)));
	}
	put_u32(blob, hc::persist::crc32(0, blob.data(), blob.size()));
	char blob_name[hc::persist::kBlobNameSize];
	hc::persist::blob_name("h.", name.c_str(), blob_name);
	blobs.blobs[blob_name] = blob;
}

std::string segment_name(uint32_t hash, uint32_t resolution, uint32_t segment) {
	char name[32];
	std::snprintf(name, sizeof(name), "a.%08x.%x.%x", static_cast<unsigned>(hash), static_cast<unsigned>(resolution),
		static_cast<unsigned>(segment));
	return name;
}

/// An old archive in its frozen segment format: every slot of `resolution` over `retention` up to `now`
/// full, of the value `value(start)` held all through it.
void old_archive(FakeBlobStore& blobs, const std::string& dp, uint32_t resolution, uint32_t retention, int64_t now,
	float (*value)(int64_t)) {
	uint32_t hash = hash_of(dp);
	uint32_t slots = History::ring_samples(retention, resolution);
	uint32_t segments = History::archive_segments(retention, resolution);
	uint32_t positions = segments * History::kSlotsPerSegment;
	std::vector<std::vector<uint8_t>> blob(segments, std::vector<uint8_t>(History::kSegmentBytes, 0));
	for (uint32_t s = 0; s < segments; ++s) {
		std::vector<uint8_t>& b = blob[s];
		auto u32 = [&b](size_t at, uint32_t v) {
			for (int i = 0; i < 4; ++i) {
				b[at + i] = static_cast<uint8_t>(v >> (8 * i));
			}
		};
		b[0] = 'H';
		b[1] = 'A';
		b[2] = 1;
		b[3] = 32;
		u32(4, resolution);
		u32(8, retention);
		u32(12, hash);
		b[16] = static_cast<uint8_t>(ValueType::real);
		b[17] = resolution % kDay == 0 ? 1 : 0;
		b[18] = static_cast<uint8_t>(s);
		b[20] = static_cast<uint8_t>(segments);
		b[22] = 127;
		u32(28, hc::persist::crc32(0, b.data(), 28));
	}
	uint32_t newest = static_cast<uint32_t>(now / resolution);
	for (uint32_t n = newest - (slots - 1); n < newest; ++n) {
		int64_t start = static_cast<int64_t>(n) * resolution;
		float v = value(start);
		ArchiveSlot slot{static_cast<double>(v) * resolution, static_cast<uint32_t>(start), v, v, v, resolution / 60,
			resolution};
		uint32_t p = n % positions;
		std::memcpy(blob[p / 127].data() + 32 + (p % 127) * 32, &slot, sizeof(slot));
	}
	for (uint32_t s = 0; s < segments; ++s) {
		blobs.blobs[segment_name(hash, resolution, s)] = blob[s];
	}
}

/// What a device of the field holds for `dp` (haumacher/ehome#73): a ring of 1 min over 24 h, an hourly
/// archive of 31 days and a daily one of a year.
void old_stores(FakeBlobStore& blobs, const std::string& dp) {
	old_ring(blobs, dp, kNow);
	old_archive(blobs, dp, 3600, 31 * kDay, kNow, hour_value);
	old_archive(blobs, dp, kDay, 365 * kDay, kNow, day_value);
}

/// The stores of the level "10 Jahre" (haumacher/ehome#221).
const std::vector<ArchiveSpec> kTenYears = {
	{10800, 7 * kDay}, {43200, 30 * kDay}, {7 * kDay, 365 * kDay}, {31 * kDay, 3650 * kDay}};
/// Another image: the 12 h archive stays, a 6 h one is new.
const std::vector<ArchiveSpec> kOther = {{21600, 14 * kDay}, {43200, 30 * kDay}};

/// One slot as it is on the store, its start aside.
using Slot = std::tuple<uint32_t, uint32_t, double, float, float, float>;
/// An archive's slots by start.
using Slots = std::map<uint32_t, Slot>;

/// The slots of the archive of `resolution` of `dp` on `blobs`.
Slots slots_of(const FakeBlobStore& blobs, const std::string& dp, uint32_t resolution) {
	Slots out;
	for (uint32_t s = 0; s < 32; ++s) {
		auto it = blobs.blobs.find(segment_name(hash_of(dp), resolution, s));
		if (it == blobs.blobs.end()) {
			continue;
		}
		for (uint32_t j = 0; j < History::kSlotsPerSegment; ++j) {
			ArchiveSlot slot;
			std::memcpy(&slot, it->second.data() + 32 + j * 32, sizeof(slot));
			if (slot.count != 0 || slot.held_s != 0) {
				out[slot.start] = Slot{slot.count, slot.held_s, slot.sum, slot.min, slot.max, slot.last};
			}
		}
	}
	return out;
}

/// The blobs whose names start with `prefix` and hold `infix`.
size_t blobs_with(const FakeBlobStore& blobs, const std::string& prefix, const std::string& infix = "") {
	size_t n = 0;
	for (const auto& blob : blobs.blobs) {
		n += blob.first.rfind(prefix, 0) == 0 && blob.first.find(infix) != std::string::npos ? 1 : 0;
	}
	return n;
}

std::string hex(uint32_t v) {
	char text[16];
	std::snprintf(text, sizeof(text), "%x", static_cast<unsigned>(v));
	return text;
}

/// "a." + the 8 hex digits of the name's hash + ".": the prefix of a datapoint's archive segments.
std::string archives_of(const std::string& dp) {
	char text[16];
	std::snprintf(text, sizeof(text), "a.%08x.", static_cast<unsigned>(hash_of(dp)));
	return text;
}

/// The controller's history over `store`, with `count` recorded datapoints Room00.Temp ... and the flash
/// budget for them.
struct Rig {
	Rig(hc::BlobStore& store, int count) : model(clock), history(loop, clock, model, store, limits(count)) {
		clock.wall_seconds = kNow;
		clock.synchronized = true;
		loop.set_clock(&clock);
		for (int i = 0; i < count; ++i) {
			names.push_back(name_of(i));
		}
		for (int i = 0; i < count; ++i) {
			specs.push_back(DatapointSpec{names[i].c_str(), ValueType::real, kH});
		}
		REQUIRE(model.load(specs.data(), specs.size()) == DatapointStatus::ok);
		for (int i = 0; i < count; ++i) {
			REQUIRE(model.bind(static_cast<uint16_t>(i), bus) == DatapointStatus::ok);
		}
	}

	static HistoryLimits limits(int count) {
		HistoryLimits l;
		l.archive_cap_bytes = static_cast<size_t>(count) * 4 * History::kSegmentBytes;
		return l;
	}

	HistoryStatus load(const std::vector<ArchiveSpec>& archives) {
		std::vector<HistorySpec> h;
		for (size_t i = 0; i < names.size(); ++i) {
			h.push_back(HistorySpec{static_cast<uint16_t>(i), kDay, 300, archives.data(),
				static_cast<uint8_t>(archives.size())});
		}
		return history.load(h.data(), h.size(), 65536, names.size() * 4 * History::kSegmentBytes);
	}

	/// The loop without a queue: until the carry-over is done.
	void settle() {
		hc_test::run_while(loop, clock, [this] { return history.carrying(); });
	}

	std::vector<HistoryBucket> query(uint16_t id, int64_t from, int64_t to, uint32_t points, HistoryAnswer& answer) {
		Buckets sink;
		REQUIRE(history.query(id, from, to, points, sink, answer) == HistoryStatus::ok);
		return sink.items;
	}

	FakeClock clock;
	hc::EventLoop loop;
	DatapointModel model;
	Bus bus;
	std::vector<std::string> names;
	std::vector<DatapointSpec> specs;
	History history;
};

/// The rig behind a write queue of the C6's size (8 KB, 24 changes) whose worker performs one change per
/// loop turn.
struct QueuedRig {
	explicit QueuedRig(int count) : queue(loop, clock, flash, &worker), counting(queue), rig(counting, count, *this) {}

	// The queue needs the loop and the clock before the history exists: they live here.
	FakeClock clock;
	hc::EventLoop loop;

	struct Inner {
		Inner(hc::BlobStore& store, int count, QueuedRig& outer)
			: model(outer.clock), history(outer.loop, outer.clock, model, store, Rig::limits(count)) {
			outer.clock.wall_seconds = kNow;
			outer.clock.synchronized = true;
			outer.loop.set_clock(&outer.clock);
			for (int i = 0; i < count; ++i) {
				names.push_back(name_of(i));
			}
			for (int i = 0; i < count; ++i) {
				specs.push_back(DatapointSpec{names[i].c_str(), ValueType::real, kH});
			}
			REQUIRE(model.load(specs.data(), specs.size()) == DatapointStatus::ok);
			for (int i = 0; i < count; ++i) {
				REQUIRE(model.bind(static_cast<uint16_t>(i), bus) == DatapointStatus::ok);
			}
		}
		DatapointModel model;
		Bus bus;
		std::vector<std::string> names;
		std::vector<DatapointSpec> specs;
		History history;
	};

	HistoryStatus load(const std::vector<ArchiveSpec>& archives) {
		std::vector<HistorySpec> h;
		for (size_t i = 0; i < rig.names.size(); ++i) {
			h.push_back(HistorySpec{static_cast<uint16_t>(i), kDay, 300, archives.data(),
				static_cast<uint8_t>(archives.size())});
		}
		return rig.history.load(h.data(), h.size(), 65536, rig.names.size() * 4 * History::kSegmentBytes);
	}

	/// One loop turn after the worker performed one change; the history's store calls in it.
	uint32_t turn() {
		worker.deliver();
		counting.calls = 0;
		clock.now_ms += 10;
		flash.on_loop = true;
		loop.run_once(clock.now_ms);
		flash.on_loop = false;
		return counting.calls;
	}

	/// Until the carry-over is done and the queue empty; the most store calls of a turn.
	uint32_t settle(size_t limit = 200000) {
		uint32_t most = 0;
		for (size_t i = 0; i < limit && (rig.history.carrying() || !queue.idle()); ++i) {
			most = std::max(most, turn());
		}
		return most;
	}

	Flash flash;
	hc_test::ManualWorker worker;
	hc::BlobQueue queue;
	Counting counting;
	Inner rig;
};

/// A carry-over without queue, interruption or live samples, of `count` datapoints from the old stores.
FakeBlobStore reference(const std::vector<ArchiveSpec>& archives, int count = 1) {
	FakeBlobStore blobs;
	for (int i = 0; i < count; ++i) {
		old_stores(blobs, name_of(i));
	}
	Rig rig(blobs, count);
	REQUIRE(rig.load(archives) == HistoryStatus::ok);
	rig.settle();
	REQUIRE_FALSE(rig.history.carrying());
	return blobs;
}

/// The new archives of `dp` on `got` are those of Room00.Temp on `want`, slot by slot.
void same_archives(const FakeBlobStore& got, const std::string& dp, const FakeBlobStore& want,
	const std::vector<ArchiveSpec>& archives) {
	for (const ArchiveSpec& a : archives) {
		CAPTURE(dp);
		CAPTURE(a.resolution_s);
		Slots expected = slots_of(want, name_of(0), a.resolution_s);
		CHECK_FALSE(expected.empty());
		CHECK(slots_of(got, dp, a.resolution_s) == expected);
	}
}

/// What the old stores held is gone, and nothing of the carry-over is left: the ring, the new archives.
void only_new_stores(const FakeBlobStore& blobs, const std::string& dp) {
	std::string prefix = archives_of(dp);
	CHECK(blobs_with(blobs, prefix + hex(3600) + ".") == 0);
	CHECK(blobs_with(blobs, prefix + hex(kDay) + ".") == 0);
	char journal[hc::persist::kBlobNameSize];
	hc::persist::blob_name("ac.", dp.c_str(), journal);
	CHECK(blobs.blobs.count(journal) == 0);
}

} // namespace

TEST_CASE("HistoryCarry: the stores of 20 datapoints change at once: in bounded steps, through the queue, no slot lost") {
	ScopedCapture capture;
	QueuedRig q(20);
	for (int i = 0; i < 20; ++i) {
		old_stores(q.flash, name_of(i));
	}
	std::map<std::string, std::vector<uint8_t>> before = q.flash.blobs;
	q.counting.calls = 0;
	REQUIRE(q.load(kTenYears) == HistoryStatus::ok);
	// The load itself reads each ring in a few calls and walks the store instead of asking for every
	// possible segment (homecontrol #118): about 16 calls a history (a ring of 1440 samples), not about 95.
	CHECK(q.counting.calls <= 20 * 17);
	CHECK(q.rig.history.carrying());
	CHECK(capture.sink.contains("history: 20 histories get new stores: what was kept is carried over in steps"));
	// Nothing was written or deleted at the load (the queue holds nothing of the carry-over yet), the old
	// archives are all there.
	CHECK(q.queue.idle());
	CHECK(q.flash.blobs == before);

	uint32_t most = q.settle();
	REQUIRE_FALSE(q.rig.history.carrying());
	// No loop turn made more store calls than a step may, and the steps did not do it in one.
	CHECK(most <= History::kCarryStepWork);
	CHECK(q.rig.history.carry_stats().max_step_work <= History::kCarryStepWork);
	CHECK(q.rig.history.carry_stats().steps > 20 * 4);
	CHECK(q.rig.history.carry_stats().retries == 0);
	// The writes waited for room in the queue: none went to the flash on the loop.
	CHECK(q.queue.direct() == 0);
	CHECK(q.queue.dropped() == 0);
	CHECK(capture.sink.contains("history: the carry-over into the new stores is done"));

	FakeBlobStore want = reference(kTenYears);
	for (int i = 0; i < 20; ++i) {
		std::string dp = name_of(i);
		CAPTURE(dp);
		same_archives(q.flash, dp, want, kTenYears);
		only_new_stores(q.flash, dp);
		// The order on the flash: the journal, the new segments, the journal's removal, then the old
		// archives.
		std::string prefix = archives_of(dp);
		char journal[hc::persist::kBlobNameSize];
		hc::persist::blob_name("ac.", dp.c_str(), journal);
		long journal_written = -1;
		long journal_removed = -1;
		long first_new = -1;
		long last_new = -1;
		long first_old_removed = -1;
		for (size_t k = 0; k < q.flash.log.size(); ++k) {
			const std::string& op = q.flash.log[k];
			long at = static_cast<long>(k);
			if (op == std::string("w ") + journal) {
				journal_written = at;
			} else if (op == std::string("r ") + journal) {
				journal_removed = at;
			} else if (op.rfind("w " + prefix, 0) == 0) {
				first_new = first_new < 0 ? at : first_new;
				last_new = at;
			} else if (op.rfind("r " + prefix, 0) == 0) {
				first_old_removed = first_old_removed < 0 ? at : first_old_removed;
			}
		}
		REQUIRE(journal_written >= 0);
		CHECK(journal_written < first_new);
		CHECK(last_new < journal_removed);
		CHECK(journal_removed < first_old_removed);
	}

	// The archives answer as the derived ones did (homecontrol #115's checks), for every datapoint.
	for (uint16_t id = 0; id < 20; ++id) {
		CAPTURE(id);
		HistoryAnswer answer;
		int64_t nine = kMonday + kDay + 9 * 3600;
		Buckets sink;
		REQUIRE(q.rig.history.query(id, nine, nine + 10800, 1, sink, answer) == HistoryStatus::ok);
		REQUIRE(sink.items.size() == 1);
		CHECK(answer.resolution_s == 10800);
		CHECK(sink.items[0].count == 180);
		CHECK(sink.items[0].mean == doctest::Approx(10.0));
		int64_t week_start = (kNow / kDay - 100) / 7 * 7 * kDay;
		Buckets week;
		REQUIRE(q.rig.history.query(id, week_start, week_start + 7 * kDay, 1, week, answer) == HistoryStatus::ok);
		REQUIRE(week.items.size() == 1);
		CHECK(answer.resolution_s == 7 * kDay);
		CHECK(week.items[0].count == 7 * 1440);
		CHECK(week.items[0].mean == doctest::Approx(3.0));
	}
}

TEST_CASE("HistoryCarry: a power cut at any turn leaves the old or the new stores, and the restart ends in the same archives") {
	FakeBlobStore want = reference(kTenYears);
	QueuedRig q(1);
	old_stores(q.flash, name_of(0));
	std::map<std::string, std::vector<uint8_t>> old = q.flash.blobs;
	REQUIRE(q.load(kTenYears) == HistoryStatus::ok);
	std::vector<std::map<std::string, std::vector<uint8_t>>> cuts;
	for (size_t i = 0; i < 100000 && (q.rig.history.carrying() || !q.queue.idle()); ++i) {
		q.turn();
		cuts.push_back(q.flash.blobs); // what a power cut now leaves: what the worker performed
	}
	REQUIRE(cuts.size() > 20);
	const std::string dp = name_of(0);
	char journal[hc::persist::kBlobNameSize];
	hc::persist::blob_name("ac.", dp.c_str(), journal);
	size_t with_journal = 0;
	for (size_t i = 0; i < cuts.size(); ++i) {
		CAPTURE(i);
		FakeBlobStore flash;
		flash.blobs = cuts[i];
		// Readable at every moment: all the old archives, or (the journal gone) all the new ones complete.
		bool old_complete = true;
		for (const auto& blob : old) {
			if (blob.first.rfind("a.", 0) == 0) {
				auto it = flash.blobs.find(blob.first);
				old_complete = old_complete && it != flash.blobs.end() && it->second == blob.second;
			}
		}
		bool journal_there = flash.blobs.count(journal) > 0;
		with_journal += journal_there ? 1 : 0;
		bool new_complete = !journal_there;
		for (const ArchiveSpec& a : kTenYears) {
			new_complete = new_complete && slots_of(flash, dp, a.resolution_s) == slots_of(want, dp, a.resolution_s);
		}
		CHECK((old_complete || new_complete));
		// The restart (the same image, the same time): the same archives, nothing twice, nothing left over.
		Rig restart(flash, 1);
		REQUIRE(restart.load(kTenYears) == HistoryStatus::ok);
		restart.settle();
		same_archives(flash, dp, want, kTenYears);
		only_new_stores(flash, dp);
	}
	CHECK(with_journal > 5); // the cuts went through the carry-over, not only before or after it
}

TEST_CASE("HistoryCarry: another image in the middle of a carry-over does it over, from the stores that were kept") {
	ScopedCapture capture;
	const std::string dp = name_of(0);
	char journal[hc::persist::kBlobNameSize];
	hc::persist::blob_name("ac.", dp.c_str(), journal);
	auto partway = [&](QueuedRig& q) {
		old_stores(q.flash, dp);
		REQUIRE(q.load(kTenYears) == HistoryStatus::ok);
		// Until the journal and some of the new segments are on the flash.
		for (size_t i = 0; i < 100000 && !(q.flash.blobs.count(journal) > 0 &&
				blobs_with(q.flash, archives_of(dp) + hex(43200) + ".") > 0); ++i) {
			q.turn();
		}
		REQUIRE(q.rig.history.carrying());
		REQUIRE(blobs_with(q.flash, archives_of(dp) + hex(31 * kDay) + ".") == 0);
	};

	SUBCASE("the same image again") {
		QueuedRig q(1);
		partway(q);
		REQUIRE(q.load(kTenYears) == HistoryStatus::ok);
		CHECK(capture.sink.contains("the carry-over into the new stores stops"));
		CHECK(capture.sink.contains("the 10800 s archive is carried over again (the carry-over before did not finish)"));
		CHECK(q.rig.history.carrying());
		CHECK(q.settle() <= History::kCarryStepWork);
		CHECK(q.queue.direct() == 0);
		same_archives(q.flash, dp, reference(kTenYears), kTenYears);
		only_new_stores(q.flash, dp);
	}

	SUBCASE("an image with other stores") {
		QueuedRig q(1);
		partway(q);
		REQUIRE(q.load(kOther) == HistoryStatus::ok);
		CHECK(q.settle() <= History::kCarryStepWork);
		CHECK(q.queue.direct() == 0);
		// The unfinished archives the image no longer has are gone, the 12 h one is filled again, the 6 h
		// one is new: as if the first image had never come.
		CHECK(capture.sink.contains("the 10800 s archive an unfinished carry-over left is deleted"));
		same_archives(q.flash, dp, reference(kOther), kOther);
		only_new_stores(q.flash, dp);
		for (uint32_t gone : {10800u, 7 * kDay, 31 * kDay}) {
			CHECK(blobs_with(q.flash, archives_of(dp) + hex(gone) + ".") == 0);
		}
	}
}

TEST_CASE("HistoryCarry: live samples during the carry-over go on in the new archive, counted once with what was kept") {
	// The value comes in right at the load, before the first step: the open slot of the 3 h archive holds
	// it while the carry-over has not reached the slot yet; when it writes the slot, the minutes the ring
	// kept from the slot's start up to the load join it (once), and the samples go on.
	FakeBlobStore blobs;
	old_stores(blobs, name_of(0));
	Rig rig(blobs, 1);
	REQUIRE(rig.load({{10800, 7 * kDay}}) == HistoryStatus::ok);
	REQUIRE(rig.history.carrying());
	REQUIRE(rig.model.report(rig.bus, 0, Value::of_real(50)) == DatapointStatus::ok);
	ArchiveSlot open;
	REQUIRE(rig.history.open_slot(0, 10800, open));
	CHECK(open.count == 1); // the live sample alone so far
	for (int i = 0; i < 600; ++i) {
		rig.clock.now_ms += 1000;
		rig.clock.wall_seconds += 1;
		rig.loop.run_once(rig.clock.now_ms);
	}
	REQUIRE_FALSE(rig.history.carrying());
	REQUIRE(rig.history.open_slot(0, 10800, open));
	int64_t start = kNow / 10800 * 10800;
	CHECK(open.start == start);
	// The ring's minutes from the slot's start up to the load, the live sample and the two keepalives.
	CHECK(open.count == (kNow - start) / 60 + 1 + 2);
	CHECK(open.max == 50.0f);
	// Held from the slot's start up to the load, then live up to now (the tick credits every 5 s).
	CHECK(open.held_s >= (kNow - start) + 595);
	CHECK(open.held_s <= (kNow - start) + 600);
	// Saved as usual once the carry-over is done: a restart takes the slot up from the store.
	rig.history.flush();
	Rig restart(blobs, 1);
	restart.clock.wall_seconds = kNow + 1200;
	REQUIRE(restart.load({{10800, 7 * kDay}}) == HistoryStatus::ok);
	CHECK_FALSE(restart.history.carrying());
	REQUIRE(restart.model.report(restart.bus, 0, Value::of_real(51)) == DatapointStatus::ok);
	ArchiveSlot again;
	REQUIRE(restart.history.open_slot(0, 10800, again));
	CHECK(again.count == open.count + 1);
}

TEST_CASE("HistoryCarry: a write queue that other writers keep full makes the carry-over wait, never write on the loop") {
	// The value store, the blocks and the rings write beside the carry-over (homecontrol #101): when the
	// arena has no room for a segment, the carry-over looks again later instead of letting the queue fall
	// back to a write on the loop; the other writers keep that fallback.
	ScopedCapture capture;
	QueuedRig q(3);
	for (int i = 0; i < 3; ++i) {
		old_stores(q.flash, name_of(i));
	}
	REQUIRE(q.load(kTenYears) == HistoryStatus::ok);
	std::string filler(3000, 'x');
	size_t turns = 0;
	size_t full = 0; // turns of the carry-over in which a segment would not have fit into the queue
	for (; turns < 200000 && (q.rig.history.carrying() || !q.queue.idle()); ++turns) {
		if (turns % 2 == 0) {
			// Another writer of the loop: three blobs of 3000 bytes in turn.
			char name[16];
			std::snprintf(name, sizeof(name), "v.fill%u", static_cast<unsigned>(turns / 2 % 3));
			BlobChunk chunk{filler.data(), filler.size()};
			q.flash.on_loop = true;
			BlobStatus status = q.queue.write(name, &chunk, 1);
			CHECK((status == BlobStatus::ok || status == BlobStatus::busy)); // busy: full, its last one in flight
			q.flash.on_loop = false;
		}
		full += q.rig.history.carrying() && !q.queue.has_room(History::kSegmentBytes) ? 1 : 0;
		CHECK(q.turn() <= History::kCarryStepWork);
	}
	REQUIRE_FALSE(q.rig.history.carrying());
	// The queue was full now and then (the other writer's fallback went to the flash on the loop, the
	// carry-over waited) ...
	CHECK(q.queue.direct() > 0);
	CHECK(full > 0);
	CHECK(q.rig.history.carry_stats().queue_waits > 0);
	// ... but nothing of the history did.
	for (const std::string& op : q.flash.loop_log) {
		CAPTURE(op);
		CHECK(op.find(" v.fill") != std::string::npos);
	}
	for (int i = 0; i < 3; ++i) {
		// Said once, however often a step waited in the middle.
		for (uint32_t gone : {3600u, kDay}) {
			std::string line = "history of '" + name_of(i) + "': the " + std::to_string(gone) +
				" s archive is dropped: the image no longer has it";
			CHECK(capture.sink.count(line) == 1);
		}
	}
	FakeBlobStore want = reference(kTenYears);
	for (int i = 0; i < 3; ++i) {
		same_archives(q.flash, name_of(i), want, kTenYears);
		only_new_stores(q.flash, name_of(i));
	}
}

TEST_CASE("HistoryCarry: a store that refuses the writes keeps what was kept; the carry-over starts again and counts the live slot once") {
	// The queue tries a refused write five times and drops it; the carry-over finds the segment missing
	// when it reads it back, deletes nothing, and starts the datapoint again later. The slot of the cut
	// was merged with the live sample before the failed write: the retry does not merge it again.
	ScopedCapture capture;
	const std::string dp = name_of(0);
	char journal[hc::persist::kBlobNameSize];
	hc::persist::blob_name("ac.", dp.c_str(), journal);
	QueuedRig q(1);
	old_stores(q.flash, dp);
	std::map<std::string, std::vector<uint8_t>> old = q.flash.blobs;
	REQUIRE(q.load({{10800, 7 * kDay}}) == HistoryStatus::ok);
	REQUIRE(q.rig.model.report(q.rig.bus, 0, Value::of_real(50)) == DatapointStatus::ok);
	for (size_t i = 0; i < 100000 && q.flash.blobs.count(journal) == 0; ++i) {
		q.turn();
	}
	REQUIRE(q.flash.blobs.count(journal) == 1);
	q.flash.fail_writes = true;
	for (size_t i = 0; i < 100000 && q.rig.history.carry_stats().retries == 0; ++i) {
		q.turn();
	}
	CHECK(q.rig.history.carry_stats().retries == 1);
	CHECK(capture.sink.contains("history of 'Room00.Temp': carrying it over into the new stores failed (a new segment did not "
		"reach the store"));
	CHECK(q.queue.dropped() >= 1);
	CHECK(q.rig.history.carrying());
	for (const auto& blob : old) {
		CAPTURE(blob.first);
		REQUIRE(q.flash.blobs.count(blob.first) == 1);
		CHECK(q.flash.blobs[blob.first] == blob.second);
	}
	q.flash.fail_writes = false;
	q.settle();
	REQUIRE_FALSE(q.rig.history.carrying());
	CHECK(capture.sink.contains("history of 'Room00.Temp': carried over into the new stores"));
	only_new_stores(q.flash, dp);
	ArchiveSlot open;
	REQUIRE(q.rig.history.open_slot(0, 10800, open));
	int64_t start = kNow / 10800 * 10800;
	CHECK(open.start == start);
	// The ring's 30 minutes of the slot up to the load and the live sample: once.
	CHECK(open.count == (kNow - start) / 60 + 1);
	CHECK(open.max == 50.0f);
	// The slots before it as without the failure; the slot of the cut on the store is the merged one.
	Slots got = slots_of(q.flash, dp, 10800);
	Slots want = slots_of(reference({{10800, 7 * kDay}}), dp, 10800);
	REQUIRE(got.count(static_cast<uint32_t>(start)) == 1);
	CHECK(std::get<0>(got[static_cast<uint32_t>(start)]) == open.count);
	got.erase(static_cast<uint32_t>(start));
	want.erase(static_cast<uint32_t>(start));
	CHECK(got.size() > 50);
	CHECK(got == want);
}

TEST_CASE("HistoryCarry: without the history the journal goes with the archives, also in the middle of a carry-over") {
	const std::string dp = name_of(0);
	char journal[hc::persist::kBlobNameSize];
	hc::persist::blob_name("ac.", dp.c_str(), journal);
	QueuedRig q(1);
	old_stores(q.flash, dp);
	// The journal of a datapoint the model no longer has.
	char gone[hc::persist::kBlobNameSize];
	hc::persist::blob_name("ac.", "Gone.Temp", gone);
	q.flash.blobs[gone] = std::vector<uint8_t>(16, 0);
	REQUIRE(q.load(kTenYears) == HistoryStatus::ok);
	for (size_t i = 0; i < 100000 && q.flash.blobs.count(journal) == 0; ++i) {
		q.turn();
	}
	CHECK(q.flash.blobs.count(gone) == 0);
	REQUIRE(q.rig.history.carrying());
	// The datapoint is no longer recorded: nothing of its history stays.
	const DatapointSpec plain[] = {{q.rig.names[0].c_str(), ValueType::real, 0}};
	REQUIRE(q.rig.model.load(plain, 1) == DatapointStatus::ok);
	REQUIRE(q.rig.history.load(nullptr, 0, 0) == HistoryStatus::ok);
	CHECK_FALSE(q.rig.history.carrying());
	q.settle();
	CHECK(blobs_with(q.flash, "a.") == 0);
	CHECK(blobs_with(q.flash, "ac.") == 0);
}

TEST_CASE("HistoryCarry: the carry-over takes no block above 3 KB, as a swap must not (homecontrol #71)") {
	// The model of a fragmented heap whose largest free block is 3 KB: the load and every step of the
	// carry-over get what they ask for (the segment's slots in two halves, the jobs in pages), and the
	// result is that of a carry-over on any heap.
	class Excluded : public FakeBlobStore {
	public:
		BlobStatus write(const char* name, const BlobChunk* chunks, size_t count) override {
			hc_test::HeapExclusion flash; // the device's flash, not its heap
			return FakeBlobStore::write(name, chunks, count);
		}
		BlobStatus patch(const char* name, size_t offset, const BlobChunk* chunks, size_t count) override {
			hc_test::HeapExclusion flash;
			return FakeBlobStore::patch(name, offset, chunks, count);
		}
		BlobStatus remove(const char* name) override {
			hc_test::HeapExclusion flash;
			return FakeBlobStore::remove(name);
		}
	};
	Excluded blobs;
	old_stores(blobs, name_of(0));
	Rig rig(blobs, 1);
	{
		hc_test::BlockLimit limit(3 * 1024);
		REQUIRE(rig.load(kTenYears) == HistoryStatus::ok);
		rig.settle();
		CHECK(limit.refused() == 0);
	}
	REQUIRE_FALSE(rig.history.carrying());
	same_archives(blobs, name_of(0), reference(kTenYears), kTenYears);
	only_new_stores(blobs, name_of(0));
}

TEST_CASE("HistoryCarry: without the memory for a segment's slots the carry-over waits and keeps what was kept") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	old_stores(blobs, name_of(0));
	std::map<std::string, std::vector<uint8_t>> old = blobs.blobs;
	Rig rig(blobs, 1);
	REQUIRE(rig.load(kTenYears) == HistoryStatus::ok);
	{
		hc_test::BlockLimit limit(1024); // the halves are 2 KB each
		hc_test::run_while(rig.loop, rig.clock, [] { return true; }, 100);
		CHECK(limit.refused() > 0);
	}
	CHECK(capture.sink.contains("history: no memory for the carry-over into the new stores (2 blocks of 2048 bytes); "
		"trying again in 30 s"));
	CHECK(rig.history.carrying());
	CHECK(blobs.blobs == old);
	rig.settle();
	same_archives(blobs, name_of(0), reference(kTenYears), kTenYears);
	only_new_stores(blobs, name_of(0));
}
