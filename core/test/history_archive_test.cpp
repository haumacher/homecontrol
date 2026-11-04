/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The consolidated archives of the history (issue #56, doc/persistence.md "Archives").
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/history.h"
#include "hc/technology.h"
#include "hc/tz.h"
#include "hc/value_store.h"
#include "rules_test_support.h"
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
using hc::HistoryLimits;
using hc::HistorySpec;
using hc::HistoryStatus;
using hc::Value;
using hc::ValueType;
using hc_test::FakeBlobStore;
using hc_test::FakeClock;
using hc_test::ScopedCapture;

namespace {

constexpr uint8_t kH = DatapointFlags::kHistory;
/// 2026-10-26 00:00:00 UTC, a multiple of 300 s (keepalives then fall on the 5-minute grid).
constexpr int64_t kMonday = 1792972800;

class Bus : public hc::Technology {
public:
	const char* name() const override { return "bus"; }
	bool command(const hc::Datapoint&, const Value&) override { return true; }
};

const DatapointSpec kSpecs[] = {
	{"Living.Temp", ValueType::real, kH},
	{"Heating", ValueType::boolean, kH},
};

/// Collects the buckets of a query.
struct Buckets : hc::HistorySink {
	bool bucket(const HistoryBucket& b) override {
		items.push_back(b);
		return true;
	}
	std::vector<HistoryBucket> items;
};

/// One boot: the model, the history (with a zone for the local days), an event loop on a fake
/// clock; the blob store outlives boots. Each second: the test's reports first, then the loop.
struct Boot {
	explicit Boot(FakeBlobStore& blobs, int64_t wall = kMonday, const hc::TimeZone* zone = nullptr,
		const HistoryLimits& limits = HistoryLimits())
		: model(clock), history(loop, clock, model, blobs, limits, zone) {
		clock.wall_seconds = wall;
		clock.synchronized = true;
		REQUIRE(model.load(kSpecs, 2) == DatapointStatus::ok);
		REQUIRE(model.bind(0, bus) == DatapointStatus::ok);
		REQUIRE(model.bind(1, bus) == DatapointStatus::ok);
	}

	/// The specs, and a plain ring for each flagged datapoint they leave out.
	HistoryStatus load(std::vector<HistorySpec> specs, size_t budget = 65536, size_t archive_budget = 65536) {
		for (uint16_t id = 0; id < model.size(); ++id) {
			bool given = false;
			for (const HistorySpec& s : specs) {
				given = given || s.datapoint == id;
			}
			if (!given && model.get(id)->has_history()) {
				specs.push_back(HistorySpec{id, 600, 60});
			}
		}
		return history.load(specs.data(), specs.size(), budget, archive_budget);
	}

	void report(uint16_t id, const Value& value) { REQUIRE(model.report(bus, id, value) == DatapointStatus::ok); }

	/// One second: `before` (the reports of this second), then the loop.
	template <class F>
	void run(int64_t seconds, F before) {
		for (int64_t i = 0; i < seconds; ++i) {
			clock.now_ms += 1000;
			clock.wall_seconds += 1;
			before(clock.wall_seconds);
			loop.run_once(clock.now_ms);
		}
	}

	void run(int64_t seconds) {
		run(seconds, [](int64_t) {});
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
	History history;
};

/// The temperature of minute `m` of an hour: a step from 20 to 22 at minute 15, plus 0.5 on odd
/// minutes, so that every minute's report is a change (sampled at once, at the minute).
double step(int64_t minute) {
	int64_t m = minute % 60;
	return (m < 15 ? 20.0 : 22.0) + (m % 2 == 1 ? 0.5 : 0.0);
}

/// The mean of step() over an hour, each minute held 60 s: (303.5 + 1001.5) / 60.
constexpr double kStepMean = 21.75;

ArchiveSpec archive(uint32_t resolution, uint32_t retention) {
	return ArchiveSpec{resolution, retention};
}

} // namespace

TEST_CASE("HistoryArchive: the arithmetic of the budgets (spec/runtime-image.md 3.7, as E-Home computes it)") {
	CHECK(sizeof(ArchiveSlot) == 32);
	CHECK(History::kSegmentBytes == 4096);
	CHECK(History::archive_ram_bytes(3600) == 96);
	CHECK(History::archive_ram_bytes(86400) == 96);
	CHECK(History::archive_ram_bytes(900) == 6 * 32);
	CHECK(History::archive_ram_bytes(60) == 62 * 32);
	CHECK(History::archive_ram_bytes(7) == (515 + 2) * 32); // below the minimum: the formula still holds
	// 2 days raw at 5 min, 31 days hourly, 2 years daily (the on-board case of issue #56).
	ArchiveSpec board[] = {archive(3600, 31 * 86400), archive(86400, 2 * 365 * 86400)};
	HistorySpec spec{0, 2 * 86400, 300, board, 2};
	CHECK(History::ring_samples(31 * 86400, 3600) == 745);
	CHECK(History::ring_samples(2 * 365 * 86400, 86400) == 731);
	CHECK(History::archive_segments(31 * 86400, 3600) == 6);
	CHECK(History::archive_segments(2 * 365 * 86400, 86400) == 6);
	CHECK(History::ram_bytes(spec) == 577 * 8 + 96 + 96);
	CHECK(History::flash_bytes(spec) == 12 * 4096);
	// E-Home's temperature with a year chart (haumacher/ehome#73): 24 h at 15 min, 30 d hourly, 1 y daily.
	ArchiveSpec year[] = {archive(3600, 30 * 86400), archive(86400, 365 * 86400)};
	HistorySpec temperature{0, 86400, 900, year, 2};
	CHECK(History::ram_bytes(temperature) == 97 * 8 + 96 + 96);
	CHECK(History::flash_bytes(temperature) == (6 + 3) * 4096);
	// At most 2048 slots: 17 segments.
	CHECK(History::archive_segments(2047 * 3600, 3600) == 17);
}

TEST_CASE("HistoryArchive: three days at 1 min feed an hourly archive with min, max, last and the time-weighted mean") {
	FakeBlobStore blobs;
	Boot boot(blobs);
	ArchiveSpec hourly[] = {archive(3600, 7 * 86400)};
	REQUIRE(boot.load({{0, 7200, 60, hourly, 1}}) == HistoryStatus::ok);
	boot.run(3 * 86400, [&](int64_t t) {
		if (t % 60 == 0) {
			boot.report(0, Value::of_real(step((t - kMonday) / 60)));
		}
	});
	int64_t now = boot.clock.wall_seconds;
	HistoryAnswer answer;
	auto buckets = boot.query(0, kMonday, now, 72, answer);
	CHECK(answer.resolution_s == 3600); // the raw ring (2 h) does not reach back three days
	CHECK(answer.retention_s == 7 * 86400);
	CHECK_FALSE(answer.boolean);
	// The first hour starts with the first sample at 00:01; the last (the open one) is complete
	// but for its last minute's hold up to now.
	REQUIRE(buckets.size() == 72);
	for (size_t h = 1; h + 1 < buckets.size(); ++h) {
		CAPTURE(h);
		const HistoryBucket& b = buckets[h];
		CHECK(b.t == kMonday + static_cast<int64_t>(h) * 3600);
		CHECK(b.count == 60);
		CHECK(b.min == 20.0);
		CHECK(b.max == 22.5);
		CHECK(b.last == 22.5);
		CHECK(b.mean == doctest::Approx(kStepMean).epsilon(1e-12));
		CHECK_FALSE(b.single);
	}
	// The first hour: samples from 00:01 (minute 0 has none), held from 00:01.
	CHECK(buckets[0].count == 59);
	CHECK(buckets[0].mean == doctest::Approx((60 * kStepMean - 20.0) / 59).epsilon(1e-12));
	// The sample at `now`, a full hour, opened the next slot.
	ArchiveSlot open;
	REQUIRE(boot.history.open_slot(0, 3600, open));
	CHECK(open.start == now);
	CHECK(open.count == 1);
	CHECK(open.held_s == 0);
	// Two whole hours over 3 buckets of 40 minutes: the slots fall into the bucket of their middle.
	buckets = boot.query(0, now - 2 * 3600 - 3600, now - 3600, 3, answer);
	CHECK(answer.resolution_s == 0); // 3 h, buckets of 1200 s: the raw ring reaches and is finer
	// Over 3 days in 3 buckets: each a day of 24 slots, consolidated.
	buckets = boot.query(0, kMonday, kMonday + 3 * 86400, 3, answer);
	CHECK(answer.resolution_s == 3600);
	REQUIRE(buckets.size() == 3);
	CHECK(buckets[1].count == 24 * 60);
	CHECK(buckets[1].min == 20.0);
	CHECK(buckets[1].max == 22.5);
	CHECK(buckets[1].mean == doctest::Approx(kStepMean).epsilon(1e-12));
}

TEST_CASE("HistoryArchive: the flash the archives still take shrinks as their segments are created (#52)") {
	FakeBlobStore blobs;
	Boot boot(blobs);
	ArchiveSpec hourly[] = {archive(3600, 7 * 86400)}; // 169 slots: two segments
	REQUIRE(boot.load({{0, 7200, 60, hourly, 1}}) == HistoryStatus::ok);
	CHECK(boot.history.flash_to_come() == 2 * History::kSegmentBytes);
	boot.run(86400, [&](int64_t t) {
		if (t % 60 == 0) {
			boot.report(0, Value::of_real(step((t - kMonday) / 60)));
		}
	});
	CHECK(boot.history.flash_to_come() == History::kSegmentBytes); // the first segment is on flash
	// A restart finds it: the second is still to come.
	Boot again(blobs);
	REQUIRE(again.load({{0, 7200, 60, hourly, 1}}) == HistoryStatus::ok);
	CHECK(again.history.flash_to_come() == History::kSegmentBytes);
}

TEST_CASE("HistoryArchive: a boolean's daily slot holds its on-time, the day from local midnight across a DST change") {
	hc_test::PosixRuleZone zone;
	REQUIRE(zone.parse("CET-1CEST,M3.5.0,M10.5.0/3"));
	FakeBlobStore blobs;
	// 2026-03-27 00:00 CET = 2026-03-26 23:00 UTC; DST begins 2026-03-29 02:00 CET.
	const int64_t march27 = hc::days_from_civil(2026, 3, 27) * 86400 - 3600;
	Boot boot(blobs, march27, &zone);
	ArchiveSpec daily[] = {archive(86400, 31 * 86400)};
	REQUIRE(boot.load({{1, 86400, 300, daily, 1}}) == HistoryStatus::ok);
	boot.report(1, Value::of_bool(false));
	// Four local days, on from 06:00 to 08:30 local time.
	boot.run(4 * 86400 + 3600, [&](int64_t t) {
		hc::LocalTime local;
		REQUIRE(zone.to_local(t, local));
		if (local.minute == 0 && local.second == 0 && local.hour == 6) {
			boot.report(1, Value::of_bool(true));
		}
		if (local.minute == 30 && local.second == 0 && local.hour == 8) {
			boot.report(1, Value::of_bool(false));
		}
	});
	HistoryAnswer answer;
	// The last bucket also takes the open day (2026-03-31 from 00:00 CEST, two hours off so far):
	// it reaches into the range with its middle beyond it.
	const int64_t march31 = hc::days_from_civil(2026, 3, 31) * 86400 - 7200;
	auto days = boot.query(1, march27, boot.clock.wall_seconds, 4, answer);
	CHECK(answer.resolution_s == 86400);
	CHECK(answer.boolean);
	REQUIRE(days.size() == 4);
	for (const HistoryBucket& day : days) {
		CHECK(day.on_s == 9000);
		CHECK(day.max == 1.0);
		CHECK(day.min == 0.0);
	}
	// The slots themselves: starts at local midnight, the DST day of 23 hours.
	ArchiveSlot open;
	REQUIRE(boot.history.open_slot(1, 86400, open));
	CHECK(open.start == march31);
	boot.history.flush();
	// The closed days, read back through a query of one bucket per day, mean = on-share of the held time.
	CHECK(days[0].mean == doctest::Approx(9000.0 / 86400));
	CHECK(days[2].mean == doctest::Approx(9000.0 / 82800)); // 2026-03-29: 23 hours
	CHECK(days[3].mean == doctest::Approx(9000.0 / (86400 + 7200)));
}

TEST_CASE("HistoryArchive: the query takes the finest store that reaches back and is not finer than a bucket needs") {
	FakeBlobStore blobs;
	Boot boot(blobs);
	ArchiveSpec board[] = {archive(3600, 31 * 86400), archive(86400, 2 * 365 * 86400)};
	REQUIRE(boot.load({{0, 2 * 86400, 300, board, 2}}) == HistoryStatus::ok);
	boot.report(0, Value::of_real(20));
	boot.run(3600);
	int64_t now = boot.clock.wall_seconds;
	HistoryAnswer answer;
	REQUIRE(boot.history.store_for(0, now - 365 * 86400, now, 12, answer) == HistoryStatus::ok);
	CHECK(answer.resolution_s == 86400);
	CHECK(answer.retention_s == 2 * 365 * 86400);
	REQUIRE(boot.history.store_for(0, now - 6 * 3600, now, 48, answer) == HistoryStatus::ok);
	CHECK(answer.resolution_s == 0);
	CHECK(answer.retention_s == 2 * 86400);
	REQUIRE(boot.history.store_for(0, now - 7 * 86400, now, 48, answer) == HistoryStatus::ok);
	CHECK(answer.resolution_s == 3600);
	// 30 days at 48 points: buckets of 15 h; the hourly archive reaches.
	REQUIRE(boot.history.store_for(0, now - 30 * 86400, now, 48, answer) == HistoryStatus::ok);
	CHECK(answer.resolution_s == 3600);
	// 3 years: nothing reaches; of the stores fine enough, the one that reaches furthest.
	REQUIRE(boot.history.store_for(0, now - 3 * 365 * 86400, now, 12, answer) == HistoryStatus::ok);
	CHECK(answer.resolution_s == 86400);
	// 1 h at 48 points: buckets of 75 s, finer than every store: the raw ring.
	REQUIRE(boot.history.store_for(0, now - 3600, now, 48, answer) == HistoryStatus::ok);
	CHECK(answer.resolution_s == 0);
	// 3 days at 48 points: buckets of 5400 s; the raw ring does not reach, the hourly archive does.
	REQUIRE(boot.history.store_for(0, now - 3 * 86400, now, 48, answer) == HistoryStatus::ok);
	CHECK(answer.resolution_s == 3600);
}

TEST_CASE("HistoryArchive: budgets and limits refuse with the reason; an image without archives behaves as before") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	ArchiveSpec board[] = {archive(3600, 31 * 86400), archive(86400, 2 * 365 * 86400)};
	// RAM: 577 * 8 + 2 * 96 = 4808, and the heating's plain ring of 11 samples, 88.
	CHECK(boot.load({{0, 2 * 86400, 300, board, 2}}, 4895, 65536) == HistoryStatus::budget_exceeded);
	CHECK(capture.sink.contains("the rings and archives need 4896 bytes of RAM, the image's budget is 4895"));
	// Flash: 12 segments.
	CHECK(boot.load({{0, 2 * 86400, 300, board, 2}}, 4896, 45056) == HistoryStatus::budget_exceeded);
	CHECK(capture.sink.contains("the archives need 49152 bytes of flash, the image's archive budget is 45056"));
	CHECK(boot.load({{0, 2 * 86400, 300, board, 2}}, 4896, 70000) == HistoryStatus::budget_exceeded);
	CHECK(capture.sink.contains("the image asks for 70000 bytes of flash for archives, the device allows at most 65536"));
	CHECK(boot.load({{0, 2 * 86400, 300, board, 2}}, 4896, 49152) == HistoryStatus::ok);
	CHECK(boot.history.ram_used() == 4896);
	CHECK(boot.history.flash_used() == 49152);
	CHECK(boot.history.archives(0) == 2);

	// The limits of the spec.
	ArchiveSpec fine[] = {archive(300, 86400)};
	CHECK(boot.load({{0, 86400, 300, fine, 1}}) == HistoryStatus::malformed); // not above the spacing
	ArchiveSpec coarse[] = {archive(32 * 86400, 64 * 86400)};
	CHECK(boot.load({{0, 86400, 300, coarse, 1}}) == HistoryStatus::malformed);
	// 2048 slots, the most of an archive, are 17 segments: more than the C6's cap of flash.
	ArchiveSpec most[] = {archive(3600, 2047 * 3600)};
	CHECK(boot.load({{0, 86400, 300, most, 1}}, 65536, 65536) == HistoryStatus::budget_exceeded);
	CHECK(capture.sink.contains("the archives need 69632 bytes of flash"));
	ArchiveSpec many[] = {archive(3600, 2048 * 3600)};
	CHECK(boot.load({{0, 86400, 300, many, 1}}) == HistoryStatus::malformed);
	CHECK(capture.sink.contains("needs 2049 slots"));
	REQUIRE(boot.load({{0, 2 * 86400, 300, board, 2}}, 4896, 49152) == HistoryStatus::ok);
	ArchiveSpec twice[] = {archive(3600, 86400), archive(3600, 7200)};
	CHECK(boot.load({{0, 86400, 300, twice, 2}}) == HistoryStatus::malformed);
	CHECK(capture.sink.contains("two archives of 3600 s"));
	ArchiveSpec five[] = {archive(600, 7200), archive(900, 7200), archive(1800, 7200), archive(3600, 7200),
		archive(7200, 7200)};
	CHECK(boot.load({{0, 3600, 300, five, 5}}) == HistoryStatus::malformed);
	ArchiveSpec decade[] = {archive(31 * 86400, 3651u * 86400u)};
	CHECK(boot.load({{0, 86400, 300, decade, 1}}) == HistoryStatus::malformed);
	// Refused loads kept the board's archives.
	CHECK(boot.history.archives(0) == 2);

	// Without archives: no archive blob, the budget is the rings' as before.
	FakeBlobStore plain;
	Boot old(plain);
	REQUIRE(old.load({{0, 3600, 60}}, 576, 0) == HistoryStatus::ok);
	CHECK(old.history.ram_used() == 61 * 8 + 11 * 8);
	CHECK(old.history.flash_used() == 0);
	old.report(0, Value::of_real(21));
	old.run(1800);
	old.history.flush();
	for (const auto& blob : plain.blobs) {
		CHECK(blob.first.rfind("a.", 0) != 0);
	}
	HistoryAnswer answer;
	auto buckets = old.query(0, old.clock.wall_seconds - 3600, old.clock.wall_seconds + 1, 4, answer);
	CHECK(answer.resolution_s == 0);
	CHECK_FALSE(buckets.empty());
}

TEST_CASE("HistoryArchive: a restart keeps the archives, the open slot goes on; a swap with the same spec keeps them") {
	FakeBlobStore blobs;
	ArchiveSpec hourly[] = {archive(3600, 7 * 86400)};
	int64_t wall = 0;
	std::vector<HistoryBucket> before;
	{
		Boot boot(blobs);
		REQUIRE(boot.load({{0, 7200, 60, hourly, 1}}) == HistoryStatus::ok);
		boot.run(5 * 3600 + 1800, [&](int64_t t) {
			if (t % 60 == 0) {
				boot.report(0, Value::of_real(step((t - kMonday) / 60)));
			}
		});
		// An image swap with the same spec: the archive keeps its slots.
		REQUIRE(boot.load({{0, 7200, 60, hourly, 1}}) == HistoryStatus::ok);
		HistoryAnswer answer;
		before = boot.query(0, kMonday, kMonday + 6 * 3600, 6, answer);
		REQUIRE(answer.resolution_s == 3600);
		REQUIRE(before.size() == 6);
		CHECK(before[2].mean == doctest::Approx(kStepMean));
		// The swap wrote the slots; the open one opens again with the next sample.
		ArchiveSlot open;
		CHECK_FALSE(boot.history.open_slot(0, 3600, open));
		boot.history.flush(); // the orderly shutdown
		char prefix[32];
		std::snprintf(prefix, sizeof(prefix), "a.%08x.e10.", static_cast<unsigned>(hc::persist::fnv1a("Living.Temp")));
		size_t segments = 0;
		for (const auto& blob : blobs.blobs) {
			segments += blob.first.rfind(prefix, 0) == 0 ? 1 : 0;
			if (blob.first.rfind(prefix, 0) == 0) {
				CHECK(blob.second.size() == 4096);
			}
		}
		CHECK(segments >= 1);
		wall = boot.clock.wall_seconds;
	}
	// The restart two minutes later: the slots on flash; the half hour of the open slot is taken up again.
	Boot boot(blobs, wall + 120);
	REQUIRE(boot.load({{0, 7200, 60, hourly, 1}}) == HistoryStatus::ok);
	boot.run(1800 - 120 - 60, [&](int64_t t) {
		if (t % 60 == 0) {
			boot.report(0, Value::of_real(step((t - kMonday) / 60)));
		}
	});
	HistoryAnswer answer;
	auto after = boot.query(0, kMonday, kMonday + 6 * 3600, 6, answer);
	REQUIRE(answer.resolution_s == 3600);
	REQUIRE(after.size() == 6);
	for (size_t h = 0; h < 5; ++h) {
		CHECK(after[h].count == before[h].count);
		CHECK(after[h].mean == doctest::Approx(before[h].mean));
		CHECK(after[h].min == before[h].min);
		CHECK(after[h].max == before[h].max);
	}
	// The open hour: 05:00..05:30 from before the restart (31 samples, 1800 s held), 05:33..05:59
	// after it (27 samples, 26 minutes held so far); the restart's minutes are a gap.
	ArchiveSlot open;
	REQUIRE(boot.history.open_slot(0, 3600, open));
	CHECK(open.start == kMonday + 5 * 3600);
	CHECK(open.count == 31 + 27);
	CHECK(open.held_s == 1800 + 26 * 60);
}

TEST_CASE("HistoryArchive: a changed resolution or layout drops the archive with a log line, a changed retention of the same layout keeps it") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	ArchiveSpec hourly[] = {archive(3600, 3 * 86400)};
	REQUIRE(boot.load({{0, 7200, 60, hourly, 1}}) == HistoryStatus::ok);
	boot.report(0, Value::of_real(20));
	boot.run(4 * 3600);
	boot.history.flush();
	auto segments = [&blobs]() {
		size_t n = 0;
		for (const auto& blob : blobs.blobs) {
			n += blob.first.rfind("a.", 0) == 0 ? 1 : 0;
		}
		return n;
	};
	CHECK(segments() == 1);
	// 3 days hourly = 73 slots, 1 segment; 4 days = 97 slots, still 1 segment: kept.
	ArchiveSpec longer[] = {archive(3600, 4 * 86400)};
	REQUIRE(boot.load({{0, 7200, 60, longer, 1}}) == HistoryStatus::ok);
	CHECK(capture.sink.contains("keeps its slots (retention 345600 s, was 259200 s, same segments)"));
	HistoryAnswer answer;
	CHECK(boot.query(0, kMonday, boot.clock.wall_seconds, 4, answer).size() == 4);
	CHECK(answer.resolution_s == 3600);
	// 7 days = 169 slots, 2 segments: another layout.
	ArchiveSpec week[] = {archive(3600, 7 * 86400)};
	REQUIRE(boot.load({{0, 7200, 60, week, 1}}) == HistoryStatus::ok);
	CHECK(capture.sink.contains("history of 'Living.Temp': the 3600 s archive is dropped: its retention changed the number of segments"));
	CHECK(segments() == 0);
	CHECK(boot.query(0, kMonday, boot.clock.wall_seconds - 3600, 3, answer).empty());
	boot.run(2 * 3600);
	boot.history.flush();
	CHECK(segments() == 1);
	// Another resolution: the old archive's segments go.
	ArchiveSpec two_hours[] = {archive(7200, 7 * 86400)};
	REQUIRE(boot.load({{0, 7200, 60, two_hours, 1}}) == HistoryStatus::ok);
	CHECK(capture.sink.contains("history of 'Living.Temp': the 3600 s archive is dropped: the image no longer has it"));
	CHECK(segments() == 0);
	// A changed type (the datapoint is now a boolean of the same name): dropped.
	boot.run(3 * 3600);
	boot.history.flush();
	CHECK(segments() == 1);
	const DatapointSpec as_boolean[] = {{"Living.Temp", ValueType::boolean, kH}, {"Heating", ValueType::boolean, kH}};
	REQUIRE(boot.model.load(as_boolean, 2) == DatapointStatus::ok);
	ArchiveSpec none[] = {archive(7200, 7 * 86400)};
	REQUIRE(boot.load({{0, 7200, 60, none, 1}, {1, 3600, 60}}) == HistoryStatus::ok);
	CHECK(capture.sink.contains("the 7200 s archive is dropped: the datapoint's type changed"));
	CHECK(segments() == 0);
}

TEST_CASE("HistoryArchive: an outage of the value or of the clock leaves empty slots, not zeros") {
	FakeBlobStore blobs;
	Boot boot(blobs);
	ArchiveSpec hourly[] = {archive(3600, 7 * 86400)};
	REQUIRE(boot.load({{0, 7200, 60, hourly, 1}}) == HistoryStatus::ok);
	auto good = [&](int64_t t) {
		if (t % 60 == 0) {
			boot.report(0, Value::of_real(step((t - kMonday) / 60)));
		}
	};
	boot.run(3 * 3600 - 1, good);
	boot.run(1, [&](int64_t) { REQUIRE(boot.model.report_stale(boot.bus, 0) == DatapointStatus::ok); });
	boot.run(3 * 3600); // hours 3, 4, 5: stale
	boot.run(2 * 3600, good); // hours 6, 7
	boot.clock.synchronized = false;
	boot.run(2 * 3600, good); // hours 8, 9: no wall clock
	boot.clock.synchronized = true;
	boot.run(3600, good); // hour 10
	HistoryAnswer answer;
	auto buckets = boot.query(0, kMonday, kMonday + 11 * 3600, 11, answer);
	REQUIRE(answer.resolution_s == 3600);
	std::vector<int64_t> hours;
	for (const HistoryBucket& b : buckets) {
		hours.push_back((b.t - kMonday) / 3600);
		CHECK(b.min >= 20.0); // nothing consolidates to zero
		CHECK(b.mean >= 20.0);
	}
	// Hour 8 has the sample at its first second, taken while the clock was still synchronized, and
	// nothing after it.
	CHECK(hours == std::vector<int64_t>{0, 1, 2, 6, 7, 8, 10});
	CHECK(buckets[5].count == 1);
	// Hour 2 ends with the stale report at its last second: held up to it.
	CHECK(buckets[2].count == 60);
	CHECK(buckets[2].mean == doctest::Approx(kStepMean).epsilon(1e-3));
}

TEST_CASE("HistoryArchive: the save interval keeps the bytes written per hour; the archives write hourly at least") {
	FakeBlobStore blobs;
	// Two rings of 2048 samples (32 KB) and eight archives of two segments (8 blocks, 32 KB).
	auto four = [](std::vector<ArchiveSpec>& out) {
		for (uint32_t r : {600u, 1200u, 1800u, 3600u}) {
			out.push_back(archive(r, 253 * r));
		}
	};
	std::vector<ArchiveSpec> first;
	std::vector<ArchiveSpec> second;
	four(first);
	four(second);
	std::vector<HistorySpec> specs = {{0, 2047 * 60, 60, first.data(), 4}, {1, 2047 * 60, 60, second.data(), 4}};
	auto interval = [&](size_t bytes_per_hour) {
		HistoryLimits limits;
		limits.save_bytes_per_hour = bytes_per_hour;
		Boot boot(blobs, kMonday, nullptr, limits);
		REQUIRE(boot.load(specs) == HistoryStatus::ok);
		CHECK(boot.history.flash_used() == 16 * 4096);
		return boot.history.save_interval_s();
	};
	// 64 KB an hour: everything once an hour.
	CHECK(interval(65536) == 3600);
	// 48 KB: the archives take 32 KB an hour, the rings the remaining 16 KB, every 2 hours.
	CHECK(interval(49152) == 7200);
	// 32 KB: the archives alone fill it, the rings get a quarter, every 4 hours.
	CHECK(interval(32768) == 14400);
	// Without archives the plain pacing: 32 KB of samples at 16 KB an hour.
	HistoryLimits limits;
	limits.save_bytes_per_hour = 16384;
	Boot boot(blobs, kMonday, nullptr, limits);
	REQUIRE(boot.load({{0, 2047 * 60, 60}, {1, 2047 * 60, 60}}) == HistoryStatus::ok);
	CHECK(boot.history.save_interval_s() == 7200);
}

TEST_CASE("HistoryArchive probe: a store without patch, a failing store and the pending queue") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	blobs.patchable = false;
	HistoryLimits limits;
	limits.min_save_interval_s = 600;
	Boot boot(blobs, kMonday, nullptr, limits);
	ArchiveSpec minute[] = {archive(60, 86400)};
	REQUIRE(boot.load({{0, 3600, 10, minute, 1}}) == HistoryStatus::ok);
	// 62 slots of RAM for a 60 s archive: the open one and an hour and a minute pending.
	CHECK(boot.history.ram_used() == 361 * 8 + 62 * 32 + 11 * 8);
	boot.report(0, Value::of_real(1));
	boot.run(2 * 3600, [&](int64_t t) {
		if (t % 10 == 0) {
			boot.report(0, Value::of_real(static_cast<double>(t % 7)));
		}
	});
	CHECK(capture.sink.contains("the blob store cannot patch"));
	// The queue overflowed: slots dropped (logged once), the newest stay queryable.
	CHECK(boot.history.slots_dropped() > 0);
	CHECK(capture.sink.contains("archive slots dropped"));
	HistoryAnswer answer;
	auto buckets = boot.query(0, boot.clock.wall_seconds - 3 * 3600, boot.clock.wall_seconds, 180, answer);
	CHECK(answer.resolution_s == 60);
	CHECK(buckets.size() >= 60);
	CHECK(buckets.size() <= 62);
	// The store patches again: everything pending is written at the next save, the log says so.
	blobs.patchable = true;
	boot.run(limits.min_save_interval_s + 5, [&](int64_t t) {
		if (t % 10 == 0) {
			boot.report(0, Value::of_real(static_cast<double>(t % 7)));
		}
	});
	CHECK(capture.sink.contains("saving the archives works again"));
	CHECK(blobs.patches > 0);
}

TEST_CASE("HistoryArchive probe: a segment written by hand is read (the frozen format) and a damaged one is ignored") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	uint32_t hash = hc::persist::fnv1a("Living.Temp");
	// Segment 0 of a 3600 s archive of 3 days: 73 slots, 1 segment of 127.
	std::vector<uint8_t> blob(4096, 0);
	auto u32 = [&blob](size_t at, uint32_t v) {
		for (int i = 0; i < 4; ++i) {
			blob[at + i] = static_cast<uint8_t>(v >> (8 * i));
		}
	};
	blob[0] = 'H';
	blob[1] = 'A';
	blob[2] = 1;
	blob[3] = 32;
	u32(4, 3600);
	u32(8, 3 * 86400);
	u32(12, hash);
	blob[16] = static_cast<uint8_t>(ValueType::real);
	blob[17] = 0;
	blob[18] = 0; // segment 0
	blob[20] = 1; // of 1
	blob[22] = 127;
	u32(28, hc::persist::crc32(0, blob.data(), 28));
	// The slot of the hour before kMonday + 2 h: number (kMonday + 3600) / 3600 at that position.
	uint32_t number = static_cast<uint32_t>((kMonday + 3600) / 3600);
	ArchiveSlot slot{21.5 * 3600, static_cast<uint32_t>(kMonday + 3600), 20.0f, 23.0f, 22.0f, 60, 3600};
	std::memcpy(blob.data() + 32 + (number % 127) * 32, &slot, sizeof(slot));
	char name[32];
	std::snprintf(name, sizeof(name), "a.%08x.e10.0", static_cast<unsigned>(hash));
	blobs.blobs[name] = blob;

	Boot boot(blobs, kMonday + 2 * 3600 + 10);
	ArchiveSpec hourly[] = {archive(3600, 3 * 86400)};
	REQUIRE(boot.load({{0, 600, 60, hourly, 1}}) == HistoryStatus::ok);
	HistoryAnswer answer;
	auto buckets = boot.query(0, kMonday, kMonday + 2 * 3600, 2, answer);
	REQUIRE(answer.resolution_s == 3600);
	REQUIRE(buckets.size() == 1);
	CHECK(buckets[0].t == kMonday + 3600);
	CHECK(buckets[0].mean == doctest::Approx(21.5));
	CHECK(buckets[0].min == 20.0);
	CHECK(buckets[0].max == 23.0);
	CHECK(buckets[0].last == 22.0);
	CHECK(buckets[0].count == 60);

	// A slot that is not plausible (its start is not its position's) is not returned; a damaged header drops the archive.
	blobs.blobs[name][32 + ((number + 1) % 127) * 32 + 8] = 0x11;
	blobs.blobs[name][5] ^= 0xFF;
	Boot again(blobs, kMonday + 2 * 3600 + 10);
	REQUIRE(again.load({{0, 600, 60, hourly, 1}}) == HistoryStatus::ok);
	CHECK(capture.sink.contains("the 3600 s archive is dropped: its segments are damaged"));
	CHECK(again.query(0, kMonday, kMonday + 2 * 3600, 2, answer).empty());
}

TEST_CASE("HistoryArchive probe: the clock going back does not feed old slots; a jump forward closes the open one") {
	FakeBlobStore blobs;
	Boot boot(blobs);
	ArchiveSpec hourly[] = {archive(3600, 7 * 86400)};
	REQUIRE(boot.load({{0, 7200, 60, hourly, 1}}) == HistoryStatus::ok);
	auto good = [&](int64_t t) {
		if (t % 60 == 0) {
			boot.report(0, Value::of_real(step((t - kMonday) / 60)));
		}
	};
	boot.run(2 * 3600, good);
	ArchiveSlot open;
	REQUIRE(boot.history.open_slot(0, 3600, open));
	uint32_t start = open.start;
	uint32_t count = open.count;
	// Back by 30 minutes: the samples are taken by the ring, the archive waits.
	boot.clock.wall_seconds -= 1800;
	boot.run(1200, good);
	REQUIRE(boot.history.open_slot(0, 3600, open));
	CHECK(open.start == start);
	CHECK(open.count == count);
	// Forward by a day: the open slot closes, the next opens; the day between stays empty.
	boot.clock.wall_seconds += 86400;
	boot.run(1200, good); // ends in the middle of an hour
	REQUIRE(boot.history.open_slot(0, 3600, open));
	CHECK(open.start > start + 86400 - 3600);
	HistoryAnswer answer;
	auto buckets = boot.query(0, kMonday, boot.clock.wall_seconds + 1, 26, answer);
	CHECK(answer.resolution_s == 3600);
	CHECK(buckets.size() == 4);
}
