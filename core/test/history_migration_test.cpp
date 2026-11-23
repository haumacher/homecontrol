/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The history the device kept survives a new image whose stores are dimensioned otherwise
// (homecontrol #115, haumacher/ehome#221: the stores follow from the setting "Aufzeichnung", a ring
// of 5 min over 24 h and archives of 3 h, 12 h, 7 days and 31 days, where E-Home derived 1 min rings
// and hourly and daily archives from the charts before). A ring restored into a coarser ring keeps a
// sample per spacing over its whole retention; a new archive starts with the slots the ring and the
// finer archives kept for its time, consolidated before the archives it replaces are deleted. The
// old stores are written here by hand in their frozen formats (doc/persistence.md), as a device in
// the field holds them.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/history.h"
#include "hc/technology.h"
#include "hc/value_store.h"
#include "test_support.h"

using hc::ArchiveSlot;
using hc::ArchiveSpec;
using hc::DatapointFlags;
using hc::DatapointModel;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::History;
using hc::HistoryAnswer;
using hc::HistoryBucket;
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
const char* const kName = "Living.Temp";

class Bus : public hc::Technology {
public:
	const char* name() const override { return "bus"; }
	bool command(const hc::Datapoint&, const Value&) override { return true; }
};

const DatapointSpec kSpecs[] = {{kName, ValueType::real, kH}};

struct Buckets : hc::HistorySink {
	bool bucket(const HistoryBucket& b) override {
		items.push_back(b);
		return true;
	}
	std::vector<HistoryBucket> items;
};

struct Boot {
	explicit Boot(FakeBlobStore& blobs, int64_t wall, bool synchronized = true)
		: model(clock), history(loop, clock, model, blobs) {
		clock.wall_seconds = wall;
		clock.synchronized = synchronized;
		REQUIRE(model.load(kSpecs, 1) == DatapointStatus::ok);
		REQUIRE(model.bind(0, bus) == DatapointStatus::ok);
	}

	HistoryStatus load(uint32_t retention, uint32_t spacing, const std::vector<ArchiveSpec>& archives) {
		HistorySpec spec{0, retention, spacing, archives.data(), static_cast<uint8_t>(archives.size())};
		return history.load(&spec, 1, 65536, 65536);
	}

	std::vector<HistoryBucket> query(int64_t from, int64_t to, uint32_t points, HistoryAnswer& answer) {
		Buckets sink;
		REQUIRE(history.query(0, from, to, points, sink, answer) == HistoryStatus::ok);
		return sink.items;
	}

	FakeClock clock;
	hc::EventLoop loop;
	DatapointModel model;
	Bus bus;
	History history;
};

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

/// The value the old stores hold at `t`: the hour of the day (a sawtooth 0..23 over every day).
float hour_value(int64_t t) {
	return static_cast<float>((t / 3600) % 24);
}

/// A ring blob in its frozen format (doc/persistence.md "Ring blob format"), with the spacing field.
std::vector<uint8_t> ring_blob(const std::vector<HistorySample>& samples, uint32_t spacing, uint32_t retention) {
	std::vector<uint8_t> fields = {1, static_cast<uint8_t>(std::strlen(kName))};
	fields.insert(fields.end(), kName, kName + std::strlen(kName));
	for (uint8_t b : {2, 1, static_cast<int>(ValueType::real), 3, 4}) {
		fields.push_back(b);
	}
	put_u32(fields, spacing);
	fields.push_back(4);
	fields.push_back(4);
	put_u32(fields, retention);
	for (uint8_t b : {5, 1, 8, 6, 4}) {
		fields.push_back(b);
	}
	put_u32(fields, static_cast<uint32_t>(samples.size()));
	std::vector<uint8_t> blob = {'H', 'H', 1, static_cast<uint8_t>(fields.size()), static_cast<uint8_t>(fields.size() >> 8)};
	for (uint8_t b : fields) {
		blob.push_back(b);
	}
	for (const HistorySample& s : samples) {
		put_u32(blob, s.t);
		put_u32(blob, s.bits);
	}
	put_u32(blob, hc::persist::crc32(0, blob.data(), blob.size()));
	return blob;
}

/// The ring of the old image: 24 h at 1 min up to `now`.
void old_ring(FakeBlobStore& blobs, int64_t now) {
	std::vector<HistorySample> samples;
	for (int64_t t = now - kDay; t < now; t += 60) {
		samples.push_back({static_cast<uint32_t>(t), float_bits(hour_value(t))});
	}
	char name[hc::persist::kBlobNameSize];
	hc::persist::blob_name("h.", kName, name);
	blobs.blobs[name] = ring_blob(samples, 60, kDay);
}

/// An old archive in its frozen segment format (doc/persistence.md "Segment format"): every slot of
/// `resolution` over `retention` up to `now` full, of the value `value(start)` held all through it.
void old_archive(FakeBlobStore& blobs, uint32_t resolution, uint32_t retention, int64_t now, float (*value)(int64_t),
	bool (*held)(int64_t) = nullptr) {
	uint32_t hash = hc::persist::fnv1a(kName);
	uint32_t slots = History::ring_samples(retention, resolution);
	uint32_t segments = History::archive_segments(retention, resolution);
	uint32_t positions = segments * History::kSlotsPerSegment;
	bool days = resolution % kDay == 0;
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
		b[17] = days ? 1 : 0;
		b[18] = static_cast<uint8_t>(s);
		b[20] = static_cast<uint8_t>(segments);
		b[22] = 127;
		u32(28, hc::persist::crc32(0, b.data(), 28));
	}
	uint32_t newest = static_cast<uint32_t>(now / resolution);
	for (uint32_t n = newest - (slots - 1); n < newest; ++n) {
		int64_t start = static_cast<int64_t>(n) * resolution;
		if (held != nullptr && !held(start)) {
			continue;
		}
		float v = value(start);
		ArchiveSlot slot{static_cast<double>(v) * resolution, static_cast<uint32_t>(start), v, v, v, resolution / 60,
			resolution};
		uint32_t p = n % positions;
		std::memcpy(blob[p / 127].data() + 32 + (p % 127) * 32, &slot, sizeof(slot));
	}
	for (uint32_t s = 0; s < segments; ++s) {
		char name[32];
		std::snprintf(name, sizeof(name), "a.%08x.%x.%x", static_cast<unsigned>(hash), static_cast<unsigned>(resolution),
			static_cast<unsigned>(s));
		blobs.blobs[name] = blob[s];
	}
}

/// The value of an old daily slot: the day number modulo 7, so every week averages 3.
float day_value(int64_t t) {
	return static_cast<float>((t / kDay) % 7);
}

size_t archive_blobs(const FakeBlobStore& blobs, uint32_t resolution) {
	char infix[16];
	std::snprintf(infix, sizeof(infix), ".%x.", static_cast<unsigned>(resolution));
	size_t n = 0;
	for (const auto& blob : blobs.blobs) {
		n += blob.first.rfind("a.", 0) == 0 && blob.first.find(infix, 10) != std::string::npos ? 1 : 0;
	}
	return n;
}

/// The stores of the level "10 Jahre" (haumacher/ehome#221).
const std::vector<ArchiveSpec> kTenYears = {
	{10800, 7 * kDay}, {43200, 30 * kDay}, {7 * kDay, 365 * kDay}, {31 * kDay, 3650 * kDay}};

} // namespace

TEST_CASE("HistoryMigration: a ring of 1 min over 24 h restored into one of 5 min keeps the whole day") {
	FakeBlobStore blobs;
	int64_t now = kMonday + 3 * kDay;
	old_ring(blobs, now);
	Boot boot(blobs, now);
	REQUIRE(boot.load(kDay, 300, {}) == HistoryStatus::ok);
	// 1440 samples a minute apart: every fifth one, over the 24 hours (not the last 289 minutes).
	CHECK(boot.history.samples(0) == 288);
	HistoryAnswer answer;
	auto first = boot.query(now - kDay, now - kDay + 3600, 12, answer);
	REQUIRE(first.size() == 12);
	CHECK(first[0].t == now - kDay);
	CHECK(first[11].t == now - kDay + 11 * 300);
	CHECK(first[0].last == hour_value(now - kDay));
	auto last = boot.query(now - 3600, now, 12, answer);
	REQUIRE(last.size() == 12);
	CHECK(last[11].t == now - 300);
	// The same spacing keeps every sample, as before (a restart).
	FakeBlobStore same;
	old_ring(same, now);
	Boot again(same, now);
	REQUIRE(again.load(kDay, 60, {}) == HistoryStatus::ok);
	CHECK(again.history.samples(0) == 1440);
}

TEST_CASE("HistoryMigration: the stores of a derived history become those of the level 10 Jahre without losing a slot") {
	// What a device of the field holds for a temperature E-Home derived from charts of 1 h, 7 d and 1 y
	// (haumacher/ehome#73): a ring of 1 min over 24 h, an hourly archive of 31 days and a daily one of a
	// year; E-Home's migration gives it the reach of its longest store, rounded up: "10 Jahre".
	ScopedCapture capture;
	FakeBlobStore blobs;
	int64_t now = kMonday + 3 * kDay + 1800;
	old_ring(blobs, now);
	old_archive(blobs, 3600, 31 * kDay, now, hour_value);
	old_archive(blobs, kDay, 365 * kDay, now, day_value);
	Boot boot(blobs, now);
	REQUIRE(boot.load(kDay, 300, kTenYears) == HistoryStatus::ok);
	for (uint32_t resolution : {10800u, 43200u, 7 * kDay, 31 * kDay}) {
		CAPTURE(resolution);
		char text[96];
		std::snprintf(text, sizeof(text), "history of 'Living.Temp': the %u s archive starts with",
			static_cast<unsigned>(resolution));
		CHECK(capture.sink.contains(text));
		CHECK(archive_blobs(blobs, resolution) == 1);
	}
	// The archives the image no longer has are gone once their slots are taken over.
	CHECK(archive_blobs(blobs, 3600) == 0);
	CHECK(archive_blobs(blobs, kDay) == 0);
	CHECK(capture.sink.contains("the 3600 s archive is dropped: the image no longer has it"));

	HistoryAnswer answer;
	// 7 days at 48 points: the 3 h archive, every bucket filled (the last day from the ring, the six
	// before from the hourly archive).
	auto week = boot.query(now - 7 * kDay, now, 48, answer);
	CHECK(answer.resolution_s == 10800);
	CHECK(week.size() == 48);
	// One 3 h slot two days back: the hours 9, 10 and 11 of the hourly archive, 180 samples.
	int64_t nine = kMonday + kDay + 9 * 3600;
	auto slot = boot.query(nine, nine + 10800, 1, answer);
	REQUIRE(slot.size() == 1);
	CHECK(answer.resolution_s == 10800);
	CHECK(slot[0].count == 180);
	CHECK(slot[0].mean == doctest::Approx(10.0));
	CHECK(slot[0].min == 9.0);
	CHECK(slot[0].max == 11.0);
	CHECK(slot[0].last == 11.0);
	// One 3 h slot of the last day: from the ring's minutes, 180 samples, held all through (asked from
	// before the ring's reach, so that the archive answers, slot by slot).
	int64_t six = now - 1800 - 6 * 3600;
	slot = boot.query(six - 7 * 10800, six + 10800, 8, answer);
	CHECK(answer.resolution_s == 10800);
	REQUIRE(slot.size() == 8);
	CHECK(slot[7].t == six);
	CHECK(slot[7].count == 180);
	CHECK(slot[7].mean == doctest::Approx((hour_value(six) + hour_value(six + 3600) + hour_value(six + 7200)) / 3));
	// The slot the ring starts in comes from the hourly archive, which reaches back over all of it.
	CHECK(slot[0].count == 180);
	// 30 days: the 12 h archive, from the hourly one.
	auto month = boot.query(now - 30 * kDay, now, 48, answer);
	CHECK(answer.resolution_s == 43200);
	CHECK(month.size() == 48);
	// A year: the weekly archive; a week older than the hourly archive is seven daily slots, mean 3.
	auto year = boot.query(now - 365 * kDay, now, 48, answer);
	CHECK(answer.resolution_s == 7 * kDay);
	CHECK(year.size() >= 47);
	int64_t week_start = (now / kDay - 100) / 7 * 7 * kDay;
	slot = boot.query(week_start, week_start + 7 * kDay, 1, answer);
	REQUIRE(slot.size() == 1);
	CHECK(slot[0].count == 7 * 1440);
	CHECK(slot[0].mean == doctest::Approx(3.0));
	CHECK(slot[0].min == 0.0);
	CHECK(slot[0].max == 6.0);
	// Ten years: the 31-day archive holds the year that was kept, nothing before it.
	auto decade = boot.query(now - 3650 * static_cast<int64_t>(kDay), now, 48, answer);
	CHECK(answer.resolution_s == 31 * kDay);
	REQUIRE_FALSE(decade.empty());
	CHECK(decade.front().t >= now - 3650 * static_cast<int64_t>(kDay) + (3650 - 400) * static_cast<int64_t>(kDay));
	uint32_t samples = 0;
	for (const HistoryBucket& b : decade) {
		samples += b.count;
	}
	// The year of daily slots (1440 samples each) at least, and the month of hourly ones it overlaps.
	CHECK(samples >= 330 * 1440);

	// A restart on the same image takes nothing over again: the archives are there.
	ScopedCapture second;
	Boot again(blobs, now + 60);
	REQUIRE(again.load(kDay, 300, kTenYears) == HistoryStatus::ok);
	CHECK_FALSE(second.sink.contains("starts with"));
	slot = again.query(nine, nine + 10800, 1, answer);
	REQUIRE(slot.size() == 1);
	CHECK(slot[0].count == 180);
}

TEST_CASE("HistoryMigration: before the clock is synchronized the window ends at the ring's last sample") {
	// A boot without the time yet (the first load after a restart): the new archive is placed by the
	// ring's newest sample, not left empty.
	ScopedCapture capture;
	FakeBlobStore blobs;
	int64_t now = kMonday + 3 * kDay;
	old_ring(blobs, now);
	old_archive(blobs, 3600, 31 * kDay, now, hour_value);
	Boot boot(blobs, now, false);
	REQUIRE(boot.load(kDay, 300, {{10800, 7 * kDay}}) == HistoryStatus::ok);
	CHECK(capture.sink.contains("the 10800 s archive starts with"));
	boot.clock.synchronized = true;
	HistoryAnswer answer;
	int64_t nine = kMonday + kDay + 9 * 3600;
	auto slot = boot.query(nine, nine + 10800, 1, answer);
	REQUIRE(slot.size() == 1);
	CHECK(slot[0].count == 180);

	// Without a ring sample and without the time nothing can be placed: the archive starts empty, said so.
	ScopedCapture quiet;
	FakeBlobStore only_archive;
	old_archive(only_archive, 3600, 31 * kDay, now, hour_value);
	Boot blind(only_archive, now, false);
	REQUIRE(blind.load(kDay, 300, {{10800, 7 * kDay}}) == HistoryStatus::ok);
	CHECK(quiet.sink.contains("the 10800 s archive starts empty: the time is not known yet"));
}

TEST_CASE("HistoryMigration: a source of another datapoint or type is not taken, and a live value goes on in the seeded slot") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	int64_t now = kMonday + 3 * kDay + 600;
	old_ring(blobs, now);
	// The ring blob of another type: the datapoint was a boolean before.
	char name[hc::persist::kBlobNameSize];
	hc::persist::blob_name("h.", kName, name);
	std::vector<uint8_t>& blob = blobs.blobs[name];
	Boot boot(blobs, now);
	{
		FakeBlobStore other;
		other.blobs[name] = blob;
		other.blobs[name][5 + 2 + std::strlen(kName) + 2] = static_cast<uint8_t>(ValueType::boolean);
		size_t crc_at = other.blobs[name].size() - 4;
		uint32_t crc = hc::persist::crc32(0, other.blobs[name].data(), crc_at);
		for (int i = 0; i < 4; ++i) {
			other.blobs[name][crc_at + i] = static_cast<uint8_t>(crc >> (8 * i));
		}
		Boot changed(other, now);
		REQUIRE(changed.load(kDay, 300, {{10800, 7 * kDay}}) == HistoryStatus::ok);
		CHECK(archive_blobs(other, 10800) == 0);
	}
	REQUIRE(boot.load(kDay, 300, {{10800, 7 * kDay}}) == HistoryStatus::ok);
	// The open slot of now (from the ring's minutes since its start) goes on with the live samples.
	REQUIRE(boot.model.report(boot.bus, 0, Value::of_real(50)) == DatapointStatus::ok);
	for (int i = 0; i < 600; ++i) {
		boot.clock.now_ms += 1000;
		boot.clock.wall_seconds += 1;
		boot.loop.run_once(boot.clock.now_ms);
	}
	ArchiveSlot open;
	REQUIRE(boot.history.open_slot(0, 10800, open));
	int64_t start = now / 10800 * 10800;
	CHECK(open.start == start);
	// The ring's minutes from the slot's start up to the load, then the live samples every 5 min.
	CHECK(open.count == (now - start) / 60 + 1 + 2);
	CHECK(open.max == 50.0f);
}

TEST_CASE("HistoryMigration probe: slots the device was off for carry no weight in the coarser archive") {
	// The hourly archive holds only the even hours (the device was off in the odd ones; their slots
	// were never written). A 12 h slot eleven days back, beyond the 3 h archive's week, is consolidated from the
	// six held hours 0, 2, ..., 10: mean 5, min 0, max 10, held 6 h, not 12.
	FakeBlobStore blobs;
	int64_t now = kMonday + 12 * kDay + 1800;
	old_ring(blobs, now);
	old_archive(blobs, 3600, 31 * kDay, now, hour_value, [](int64_t t) { return (t / 3600) % 2 == 0; });
	Boot boot(blobs, now);
	REQUIRE(boot.load(kDay, 300, kTenYears) == HistoryStatus::ok);
	HistoryAnswer answer;
	int64_t day = kMonday + kDay;
	auto slot = boot.query(day, day + 43200, 1, answer);
	REQUIRE(slot.size() == 1);
	CHECK(answer.resolution_s == 43200);
	CHECK(slot[0].min == doctest::Approx(0));
	CHECK(slot[0].max == doctest::Approx(10));
	CHECK(slot[0].mean == doctest::Approx(5));
	auto evening = boot.query(day + 43200, day + kDay, 1, answer);
	REQUIRE(evening.size() == 1);
	CHECK(evening[0].mean == doctest::Approx(17));
	CHECK(evening[0].max == doctest::Approx(22));
}
