/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <algorithm>
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

using hc::DatapointFlags;
using hc::DatapointModel;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::History;
using hc::HistoryLimits;
using hc::HistoryPoint;
using hc::HistorySpec;
using hc::HistoryStatus;
using hc::Value;
using hc::ValueType;
using hc_test::FakeBlobStore;
using hc_test::FakeClock;
using hc_test::ScopedCapture;

namespace {

constexpr uint8_t kH = DatapointFlags::kHistory;
constexpr int64_t kWall = 1790000000;

class Bus : public hc::Technology {
public:
	const char* name() const override { return "bus"; }
	bool command(const hc::Datapoint&, const Value&) override { return true; }
};

const DatapointSpec kSpecs[] = {
	{"Living.Temp", ValueType::real, kH},
	{"Living.Light", ValueType::boolean, kH},
	{"Living.Plain", ValueType::real, 0},
	{"Living.Name", ValueType::string, 0},
};

/// One boot: a model, the history, an event loop on a fake clock; the blob store outlives boots.
struct Boot {
	explicit Boot(FakeBlobStore& blobs, int64_t wall = kWall, const HistoryLimits& limits = HistoryLimits())
		: model(clock), history(loop, clock, model, blobs, limits) {
		clock.wall_seconds = wall;
		clock.synchronized = true;
	}

	void load_model(const DatapointSpec* specs, size_t count) {
		REQUIRE(model.load(specs, count) == DatapointStatus::ok);
		for (size_t i = 0; i < count; ++i) {
			REQUIRE(model.bind(static_cast<uint16_t>(i), bus) == DatapointStatus::ok);
		}
	}

	/// The default model with Temp and Light at the given retention and spacing.
	void load(uint32_t retention_s = 3600, uint32_t spacing_s = 60, size_t budget = 4096) {
		load_model(kSpecs, 4);
		const HistorySpec specs[] = {{0, retention_s, spacing_s}, {1, retention_s, spacing_s}};
		REQUIRE(history.load(specs, 2, budget) == HistoryStatus::ok);
	}

	void report(uint16_t id, const Value& value) { REQUIRE(model.report(bus, id, value) == DatapointStatus::ok); }

	void run(int seconds) {
		for (int i = 0; i < seconds; ++i) {
			clock.now_ms += 1000;
			clock.wall_seconds += 1;
			loop.run_once(clock.now_ms);
		}
	}

	std::vector<HistoryPoint> range(uint16_t id, int64_t from, int64_t to, size_t max_points) {
		std::vector<HistoryPoint> points(max_points);
		size_t count = 0;
		REQUIRE(history.range(id, from, to, points.data(), max_points, count) == HistoryStatus::ok);
		points.resize(count);
		return points;
	}

	std::vector<HistoryPoint> all(uint16_t id) { return range(id, 0, clock.wall_seconds + 1, 4096); }

	FakeClock clock;
	hc::EventLoop loop;
	DatapointModel model;
	Bus bus;
	History history;
};

std::string blob_of(const char* datapoint) {
	char name[hc::persist::kBlobNameSize];
	hc::persist::blob_name("h.", datapoint, name);
	return name;
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
	for (int i = 0; i < 4; ++i) {
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

/// A ring blob built by hand: `extra` header bytes (a field of a newer firmware) and samples of
/// `sample_size` bytes (the bytes beyond 8 are 0xEE).
std::vector<uint8_t> make_blob(const char* name, ValueType type, const std::vector<hc::HistorySample>& samples,
	uint8_t sample_size = 8, std::vector<uint8_t> extra = {}, uint8_t version = History::kFormatVersion) {
	std::vector<uint8_t> fields = extra;
	fields.push_back(1);
	fields.push_back(static_cast<uint8_t>(std::strlen(name)));
	fields.insert(fields.end(), name, name + std::strlen(name));
	fields.insert(fields.end(), {2, 1, static_cast<uint8_t>(type)});
	fields.insert(fields.end(), {5, 1, sample_size});
	fields.insert(fields.end(), {6, 4});
	put_u32(fields, static_cast<uint32_t>(samples.size()));
	std::vector<uint8_t> blob = {'H', 'H', version, static_cast<uint8_t>(fields.size()),
		static_cast<uint8_t>(fields.size() >> 8)};
	blob.insert(blob.end(), fields.begin(), fields.end());
	for (const hc::HistorySample& s : samples) {
		put_u32(blob, s.t);
		put_u32(blob, s.bits);
		for (size_t i = 8; i < sample_size; ++i) {
			blob.push_back(0xEE);
		}
	}
	uint32_t crc = hc::persist::crc32(0, blob.data(), blob.size());
	put_u32(blob, crc);
	return blob;
}

uint32_t float_bits(float f) {
	uint32_t bits = 0;
	std::memcpy(&bits, &f, sizeof(bits));
	return bits;
}

} // namespace

TEST_CASE("History: a ring takes 8 bytes per sample, retention / spacing + 1 samples") {
	CHECK(History::ring_samples(86400, 60) == 1441);
	CHECK(History::ring_bytes(86400, 60) == 11528);
	CHECK(History::ring_samples(7 * 86400, 600) == 1009);
	CHECK(History::ring_samples(3600, 60) == 61);
	CHECK(History::ring_samples(100, 30) == 5);
}

TEST_CASE("History: a load whose rings exceed the budget is refused with the reason; the old rings stay") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(3600, 60, 976);
	CHECK(boot.history.size() == 2);
	CHECK(boot.history.bytes() == 976);
	boot.report(0, Value::of_real(20.0));
	CHECK(boot.history.samples(0) == 1);

	// (7200 / 60 + 1) + (3600 / 60 + 1) = 182 samples of 8 bytes.
	const HistorySpec bigger[] = {{0, 7200, 60}, {1, 3600, 60}};
	CHECK(boot.history.load(bigger, 2, 976) == HistoryStatus::budget_exceeded);
	CHECK(capture.sink.contains("the rings need 1456 bytes, the image's budget is 976"));
	CHECK(boot.history.load(bigger, 2, 70000) == HistoryStatus::budget_exceeded);
	CHECK(capture.sink.contains("the image asks for 70000 bytes, the device allows at most 65536"));
	CHECK(boot.history.size() == 2);
	CHECK(boot.history.samples(0) == 1);

	// A platform with a smaller cap.
	FakeBlobStore other;
	HistoryLimits small;
	small.cap_bytes = 512;
	Boot tiny(other, kWall, small);
	tiny.load_model(kSpecs, 4);
	const HistorySpec specs[] = {{0, 3600, 60}, {1, 3600, 60}};
	CHECK(tiny.history.load(specs, 2, 1024) == HistoryStatus::budget_exceeded);
	CHECK(tiny.history.size() == 0);
}

TEST_CASE("History: specs are validated against the model") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	const DatapointSpec specs[] = {
		{"Temp", ValueType::real, kH},
		{"Plain", ValueType::real, 0},
		{"Name", ValueType::string, kH},
		{"Mode", ValueType::enumeration, kH},
	};
	boot.load_model(specs, 4);
	const HistorySpec plain[] = {{1, 3600, 60}};
	CHECK(boot.history.load(plain, 1, 4096) == HistoryStatus::flag_mismatch);
	CHECK(capture.sink.contains("'Plain' has a history spec but no history flag"));
	const HistorySpec text[] = {{2, 3600, 60}};
	CHECK(boot.history.load(text, 1, 4096) == HistoryStatus::type_error);
	CHECK(capture.sink.contains("'Name' is string"));
	const HistorySpec mode[] = {{3, 3600, 60}};
	CHECK(boot.history.load(mode, 1, 4096) == HistoryStatus::type_error);

	const DatapointSpec two[] = {{"Temp", ValueType::real, kH}, {"Hum", ValueType::real, kH}};
	boot.load_model(two, 2);
	const HistorySpec missing[] = {{0, 3600, 60}};
	CHECK(boot.history.load(missing, 1, 4096) == HistoryStatus::flag_mismatch);
	CHECK(capture.sink.contains("'Hum' has the history flag but no spec"));
	const HistorySpec twice[] = {{0, 3600, 60}, {0, 3600, 60}};
	CHECK(boot.history.load(twice, 2, 4096) == HistoryStatus::malformed);
	const HistorySpec fast[] = {{0, 3600, 5}, {1, 3600, 60}};
	CHECK(boot.history.load(fast, 2, 4096) == HistoryStatus::malformed);
	CHECK(capture.sink.contains("spacing 5 s outside 10..86400 s"));
	const HistorySpec short_retention[] = {{0, 30, 60}, {1, 3600, 60}};
	CHECK(boot.history.load(short_retention, 2, 4096) == HistoryStatus::malformed);
	const HistorySpec dense[] = {{0, 86400, 10}, {1, 3600, 60}};
	CHECK(boot.history.load(dense, 2, 65536) == HistoryStatus::malformed);
	CHECK(capture.sink.contains("needs 8641 samples"));
	const HistorySpec unknown[] = {{7, 3600, 60}, {1, 3600, 60}};
	CHECK(boot.history.load(unknown, 2, 4096) == HistoryStatus::unknown_datapoint);
	const HistorySpec good[] = {{0, 86400, 60}, {1, 7 * 86400, 600}};
	CHECK(boot.history.load(good, 2, 65536) == HistoryStatus::ok);
	CHECK(boot.history.bytes() == (1441 + 1009) * 8);
}

TEST_CASE("History: a change is sampled at once, then not before the spacing; keepalives fill flat lines") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(3600, 60);
	int64_t t0 = boot.clock.wall_seconds;
	boot.run(2);
	boot.report(0, Value::of_real(21.0)); // t0 + 2
	CHECK(boot.history.samples(0) == 1);
	boot.run(10);
	boot.report(0, Value::of_real(22.0)); // within the spacing: waits
	boot.report(0, Value::of_real(23.0));
	CHECK(boot.history.samples(0) == 1);
	boot.run(53); // t0 + 65: the tick finds the spacing passed and samples the current value
	std::vector<HistoryPoint> points = boot.all(0);
	REQUIRE(points.size() == 2);
	CHECK(points[0].t == t0 + 2);
	CHECK(points[0].last == 21.0);
	CHECK(points[1].t == t0 + 65);
	CHECK(points[1].last == 23.0);

	// Flat for ten minutes: a keepalive every 60 s (on the 5 s tick).
	boot.run(600);
	points = boot.all(0);
	REQUIRE(points.size() == 12);
	for (size_t i = 1; i < points.size(); ++i) {
		CHECK(points[i].t - points[i - 1].t >= 60);
		CHECK(points[i].t - points[i - 1].t < 65);
	}
	CHECK(points.back().last == 23.0);
	// A change after the spacing is sampled at once. On a flat line the keepalive takes a sample
	// every 60 s on the tick, so the change must come in the second the spacing has passed, before
	// that second's tick runs (the tick then finds the new sample and adds no keepalive).
	CHECK(points.back().t == t0 + 665);
	boot.run(59);
	size_t before = boot.history.samples(0);
	boot.clock.now_ms += 1000;
	boot.clock.wall_seconds += 1; // t0 + 725: the last sample is 60 s old
	boot.report(0, Value::of_real(19.5));
	CHECK(boot.history.samples(0) == before + 1);
	CHECK(boot.all(0).back().t == boot.clock.wall_seconds);
	CHECK(boot.all(0).back().last == 19.5);
	boot.loop.run_once(boot.clock.now_ms);
	CHECK(boot.history.samples(0) == before + 1);
	// A datapoint never reported has no samples.
	CHECK(boot.history.samples(1) == 0);
	CHECK(boot.history.samples(2) == 0);
}

TEST_CASE("History: no samples while the value is stale or the clock is not synchronized") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(3600, 60);
	boot.report(0, Value::of_real(21.0));
	REQUIRE(boot.model.report_stale(boot.bus, 0) == DatapointStatus::ok);
	boot.run(600);
	CHECK(boot.history.samples(0) == 1); // a gap
	boot.report(0, Value::of_real(21.0));
	CHECK(boot.history.samples(0) == 1); // the same value: the next keepalive takes it
	boot.run(5);
	CHECK(boot.history.samples(0) == 2);

	boot.clock.synchronized = false;
	boot.report(0, Value::of_real(30.0));
	boot.run(600);
	CHECK(boot.history.samples(0) == 2);
}

TEST_CASE("History: the ring wraps and keeps the newest samples in time order") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(600, 60); // 11 samples
	int64_t t0 = boot.clock.wall_seconds + 2;
	boot.run(2);
	for (int i = 0; i < 30; ++i) {
		boot.report(0, Value::of_real(i));
		boot.run(i < 29 ? 60 : 0); // query right after the last one
	}
	CHECK(boot.history.samples(0) == 11);
	std::vector<HistoryPoint> points = boot.all(0);
	REQUIRE(points.size() == 11);
	for (int i = 0; i < 11; ++i) {
		CHECK(points[i].last == 19 + i);
		CHECK(points[i].t == t0 + (19 + i) * 60);
	}
}

TEST_CASE("History: samples older than the retention are not returned and dropped") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(600, 60);
	boot.report(0, Value::of_real(1.0));
	boot.run(300);
	CHECK(boot.history.samples(0) == 6); // the report and five keepalives
	REQUIRE(boot.model.report_stale(boot.bus, 0) == DatapointStatus::ok);
	boot.run(450); // the first samples are older than 600 s now
	std::vector<HistoryPoint> points = boot.all(0);
	for (const HistoryPoint& p : points) {
		CHECK(p.t >= boot.clock.wall_seconds - 600);
	}
	CHECK(points.size() == 3);
	boot.run(2000);
	CHECK(boot.all(0).empty());
	CHECK(boot.history.samples(0) == 6); // still in RAM ...
	boot.report(0, Value::of_real(2.0));
	CHECK(boot.history.samples(0) == 1); // ... dropped with the next sample
}

TEST_CASE("History: range() decimates into buckets with min, max and last") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(1000, 10); // 101 samples
	boot.run(2);
	int64_t t0 = boot.clock.wall_seconds;
	std::vector<double> values;
	for (int i = 0; i < 100; ++i) {
		values.push_back((i * 37) % 101 - 50 + 0.5);
		boot.report(0, Value::of_real(values.back()));
		if (i < 99) {
			boot.run(10);
		}
	}
	REQUIRE(boot.history.samples(0) == 100);

	// Ten buckets of 100 s, ten samples each.
	std::vector<HistoryPoint> points = boot.range(0, t0, t0 + 1000, 10);
	REQUIRE(points.size() == 10);
	for (int b = 0; b < 10; ++b) {
		auto first = values.begin() + b * 10;
		CHECK(points[b].t == t0 + b * 100);
		CHECK(points[b].count == 10);
		CHECK(points[b].min == *std::min_element(first, first + 10));
		CHECK(points[b].max == *std::max_element(first, first + 10));
		CHECK(points[b].last == values[b * 10 + 9]);
		CHECK(points[b].last_t == t0 + (b * 10 + 9) * 10);
		double sum = 0;
		for (auto v = first; v != first + 10; ++v) {
			sum += *v;
		}
		CHECK(points[b].mean == doctest::Approx(sum / 10)); // issue #54: the chart's mean line
	}

	// An uneven split never gives more than max_points, and every sample is in one bucket.
	points = boot.range(0, t0, t0 + 1000, 7);
	CHECK(points.size() <= 7);
	uint32_t total = 0;
	for (const HistoryPoint& p : points) {
		total += p.count;
	}
	CHECK(total == 100);

	// Enough room: every sample is a point; a sub-range selects from <= t < to.
	points = boot.range(0, t0, t0 + 1000, 200);
	CHECK(points.size() == 100);
	points = boot.range(0, t0 + 100, t0 + 200, 100);
	REQUIRE(points.size() == 10);
	CHECK(points[0].t == t0 + 100);
	CHECK(points[0].last == values[10]);
	CHECK(points[0].mean == values[10]); // a single sample is its own mean
	CHECK(boot.history.retention_s(0) == 1000);
	CHECK(boot.history.retention_s(2) == 0);

	// Refusals.
	HistoryPoint out[4];
	size_t count = 0;
	CHECK(boot.history.range(2, t0, t0 + 10, out, 4, count) == HistoryStatus::unknown_datapoint);
	CHECK(boot.history.range(0, t0, t0, out, 4, count) == HistoryStatus::malformed);
	CHECK(boot.history.range(0, t0, t0 + 10, out, 0, count) == HistoryStatus::malformed);
}

TEST_CASE("History: a boolean bucket's max says it was on at some time") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(1000, 10);
	boot.run(2);
	int64_t t0 = boot.clock.wall_seconds;
	for (int i = 0; i < 20; ++i) {
		boot.report(1, Value::of_bool(i % 5 == 2));
		boot.run(10);
	}
	std::vector<HistoryPoint> points = boot.range(1, t0, t0 + 200, 4);
	REQUIRE(points.size() == 4);
	for (const HistoryPoint& p : points) {
		CHECK(p.min == 0.0);
		CHECK(p.max == 1.0);
		CHECK(p.last == 0.0);
		CHECK(p.mean == doctest::Approx(1.0 / p.count)); // the on-share: one on sample per bucket
	}
}

TEST_CASE("History: rings are saved on the interval and restored after a reboot") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	std::vector<HistoryPoint> before;
	int64_t shutdown = 0;
	{
		Boot first(blobs);
		first.load(3600, 60);
		first.run(2);
		for (int i = 0; i < 10; ++i) {
			first.report(0, Value::of_real(20 + i * 0.25));
			first.run(60);
		}
		CHECK(blobs.writes == 0);
		first.run(900 - 602);
		CHECK(blobs.writes == 1); // Temp; Light has no samples
		CHECK(blobs.blobs.count(blob_of("Living.Temp")) == 1);
		first.run(900);
		CHECK(blobs.writes == 2); // keepalives made it dirty again
		first.history.flush();     // nothing new since
		CHECK(blobs.writes == 2);
		first.run(30);
		first.history.flush();     // orderly shutdown
		CHECK(blobs.writes == 3);
		before = first.all(0);
		shutdown = first.clock.wall_seconds;
	}
	Boot second(blobs, shutdown + 300); // five minutes later
	second.load(3600, 60);
	std::vector<HistoryPoint> after = second.all(0);
	REQUIRE(after.size() == before.size());
	for (size_t i = 0; i < after.size(); ++i) {
		CHECK(after[i].t == before[i].t);
		CHECK(after[i].last == before[i].last);
	}
	CHECK(capture.sink.contains(std::to_string(before.size()) + " samples restored"));
	// New samples continue the series.
	second.report(0, Value::of_real(30.0));
	CHECK(second.history.samples(0) == before.size() + 1);

	// Two hours later the restored samples are older than the retention.
	second.history.flush();
	Boot third(blobs, shutdown + 300 + 7200);
	third.load(3600, 60);
	CHECK(third.history.samples(0) == 0);
}

TEST_CASE("History: the save interval keeps the bytes written per hour within the limit") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(3600, 60); // 976 bytes: the minimum interval
	CHECK(boot.history.save_interval_s() == 900);
	boot.load(86400, 60, 65536); // 2 x 11528 bytes: 23056 * 3600 / 65536 = 1266.5 s
	CHECK(boot.history.save_interval_s() == 1267);
	boot.report(0, Value::of_real(1.0));
	boot.run(1266);
	CHECK(blobs.writes == 0);
	boot.run(1);
	CHECK(blobs.writes == 1);
	CHECK(capture.sink.contains("saved every 1267 s"));
}

TEST_CASE("History: a smaller ring keeps the newest saved samples; an image swap keeps the series") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(3600, 60);
	boot.run(2);
	for (int i = 0; i < 30; ++i) {
		boot.report(0, Value::of_real(i));
		boot.run(60);
	}
	size_t samples = boot.history.samples(0);
	REQUIRE(samples == 30);
	// The same image again (no reboot): the new samples are saved by the load and restored.
	boot.load(3600, 60);
	CHECK(boot.history.samples(0) == samples);
	// A shorter retention: the ring has room for 11 samples, but only the newest 10 are within the
	// 600 s retention. Sample i was taken at kWall + 2 + 60 i, the load runs at kWall + 1802:
	// sample 19 (kWall + 1142) is 660 s old and dropped, sample 20 (kWall + 1202) is 600 s old and kept.
	CHECK(boot.clock.wall_seconds == kWall + 1802);
	boot.load(600, 60);
	std::vector<HistoryPoint> points = boot.all(0);
	REQUIRE(points.size() == 10);
	CHECK(points.front().last == 20);
	CHECK(points.front().t == kWall + 1202);
	CHECK(points.back().last == 29);
	// Rings of datapoints no longer in the image are deleted.
	const DatapointSpec none[] = {{"Living.Temp", ValueType::real, 0}};
	boot.load_model(none, 1);
	REQUIRE(boot.history.load(nullptr, 0, 0) == HistoryStatus::ok);
	CHECK(blobs.blobs.count(blob_of("Living.Temp")) == 0);
}

TEST_CASE("History: a blob with unknown header fields and larger samples restores; a newer generation is ignored") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	int64_t now = kWall;
	std::vector<hc::HistorySample> samples;
	for (int i = 0; i < 5; ++i) {
		samples.push_back({static_cast<uint32_t>(now - 300 + i * 60), float_bits(10.0f + i)});
	}
	samples.push_back({static_cast<uint32_t>(now - 300), float_bits(99.0f)}); // out of order: dropped
	blobs.blobs[blob_of("Living.Temp")] =
		make_blob("Living.Temp", ValueType::real, samples, 12, {40, 3, 1, 2, 3});
	blobs.blobs[blob_of("Living.Light")] = make_blob("Living.Light", ValueType::boolean,
		{{static_cast<uint32_t>(now - 60), 1}}, 8, {}, History::kFormatVersion + 1);
	Boot boot(blobs, now);
	boot.load(3600, 60);
	std::vector<HistoryPoint> points = boot.all(0);
	REQUIRE(points.size() == 5);
	for (int i = 0; i < 5; ++i) {
		CHECK(points[i].last == 10.0 + i);
		CHECK(points[i].t == now - 300 + i * 60);
	}
	CHECK(boot.history.samples(1) == 0);
	CHECK(capture.sink.contains("history of 'Living.Light'"));
	CHECK(capture.sink.contains("written by a newer firmware"));
}

TEST_CASE("History: a damaged or foreign blob is ignored with the reason") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	std::vector<uint8_t> blob = make_blob("Living.Temp", ValueType::real, {{static_cast<uint32_t>(kWall - 60), 0}});
	blob[blob.size() - 6] ^= 0x10;
	blobs.blobs[blob_of("Living.Temp")] = blob;
	blobs.blobs[blob_of("Living.Light")] =
		make_blob("Living.Light", ValueType::signed_int, {{static_cast<uint32_t>(kWall - 60), 1}});
	Boot boot(blobs);
	boot.load(3600, 60);
	CHECK(boot.history.samples(0) == 0);
	CHECK(boot.history.samples(1) == 0);
	CHECK(capture.sink.contains("checksum mismatch"));
	CHECK(capture.sink.contains("history of 'Living.Light' discarded: the datapoint's type changed"));
}

TEST_CASE("History: when the wall clock goes back, samples from the future are dropped") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(3600, 60);
	boot.report(0, Value::of_real(1.0));
	boot.run(600);
	REQUIRE(boot.history.samples(0) == 11);
	boot.clock.wall_seconds -= 300;
	boot.report(0, Value::of_real(2.0));
	std::vector<HistoryPoint> points = boot.all(0);
	CHECK(points.back().t == boot.clock.wall_seconds);
	CHECK(points.back().last == 2.0);
	for (size_t i = 1; i < points.size(); ++i) {
		CHECK(points[i].t > points[i - 1].t);
	}
	CHECK(capture.sink.contains("samples dropped, the clock went back"));
}

TEST_CASE("History: a failing store is logged once and retried at the next save") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(3600, 60);
	boot.report(0, Value::of_real(1.0));
	blobs.fail_writes = true;
	boot.run(1800);
	CHECK(blobs.writes == 0);
	CHECK(capture.sink.count("cannot save the history of 'Living.Temp'") == 1);
	blobs.fail_writes = false;
	boot.run(900);
	CHECK(blobs.writes == 1);
	CHECK(capture.sink.contains("saving the history works again"));
}
