/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/technology.h"
#include "hc/value_store.h"
#include "test_support.h"

using hc::DatapointFlags;
using hc::DatapointModel;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::Quality;
using hc::Value;
using hc::ValueStore;
using hc::ValueStoreLimits;
using hc::ValueType;
using hc_test::FakeBlobStore;
using hc_test::FakeClock;
using hc_test::ScopedCapture;

namespace {

constexpr uint8_t kP = DatapointFlags::kPersist;
constexpr int64_t kWall = 1790000000; // 2026-09-21

class Bus : public hc::Technology {
public:
	const char* name() const override { return "bus"; }
	bool command(const hc::Datapoint&, const Value&) override { return true; }
};

/// One boot of the controller: a model, a value store, an event loop on a fake clock; the blob store
/// outlives the boots.
struct Boot {
	explicit Boot(FakeBlobStore& blobs, int64_t wall = kWall, const ValueStoreLimits& limits = ValueStoreLimits())
		: model(clock), store(loop, clock, model, blobs, limits) {
		clock.wall_seconds = wall;
		clock.synchronized = true;
	}

	void load(const DatapointSpec* specs, size_t count) {
		REQUIRE(model.load(specs, count) == DatapointStatus::ok);
		for (size_t i = 0; i < count; ++i) {
			REQUIRE(model.bind(static_cast<uint16_t>(i), bus) == DatapointStatus::ok);
		}
		REQUIRE(store.load() == DatapointStatus::ok);
	}

	void report(uint16_t id, const Value& value) {
		REQUIRE(model.report(bus, id, value) == DatapointStatus::ok);
		loop.run_once(clock.now_ms); // the posted write
	}

	/// Lets `seconds` pass, running the loop every second.
	void run(int seconds) {
		for (int i = 0; i < seconds; ++i) {
			clock.now_ms += 1000;
			clock.wall_seconds += 1;
			loop.run_once(clock.now_ms);
		}
	}

	FakeClock clock;
	hc::EventLoop loop;
	DatapointModel model;
	Bus bus;
	ValueStore store;
};

ValueStore::Record decode_blob(const FakeBlobStore& blobs, const char* datapoint) {
	char name[hc::persist::kBlobNameSize];
	hc::persist::blob_name("v.", datapoint, name);
	auto it = blobs.blobs.find(name);
	ValueStore::Record record{};
	REQUIRE(it != blobs.blobs.end());
	const char* error = nullptr;
	REQUIRE(ValueStore::decode(it->second.data(), it->second.size(), record, &error));
	return record;
}

std::string blob_of(const char* datapoint) {
	char name[hc::persist::kBlobNameSize];
	hc::persist::blob_name("v.", datapoint, name);
	return name;
}

/// Replaces the CRC of a hand-edited record.
void reseal(std::vector<uint8_t>& record) {
	record.resize(record.size() - 4);
	uint32_t crc = hc::persist::crc32(0, record.data(), record.size());
	for (int i = 0; i < 4; ++i) {
		record.push_back(static_cast<uint8_t>(crc >> (8 * i)));
	}
}

const DatapointSpec kSpecs[] = {
	{"Living.Temp", ValueType::real, kP},
	{"Living.Light", ValueType::boolean, kP},
	{"Living.Volatile", ValueType::real, 0},
};

} // namespace

TEST_CASE("ValueStore: a record is written at once, then coalesced to one write per interval") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(kSpecs, 3);

	boot.report(0, Value::of_real(20.5));
	CHECK(blobs.writes == 1);
	CHECK(decode_blob(blobs, "Living.Temp").value == Value::of_real(20.5));
	CHECK(decode_blob(blobs, "Living.Temp").wall_s == kWall);

	// Three changes within the interval: nothing written until the interval has passed.
	boot.run(10);
	boot.report(0, Value::of_real(21.0));
	boot.run(10);
	boot.report(0, Value::of_real(21.5));
	boot.run(30);
	boot.report(0, Value::of_real(22.0));
	CHECK(blobs.writes == 1);
	CHECK(boot.store.pending() == 1);
	boot.run(15); // 65 s after the first write: the tick (5 s) has written the latest value
	CHECK(blobs.writes == 2);
	CHECK(boot.store.pending() == 0);
	ValueStore::Record record = decode_blob(blobs, "Living.Temp");
	CHECK(record.value == Value::of_real(22.0));
	CHECK(record.wall_s == kWall + 50);

	// A datapoint without kPersist is never written.
	boot.report(2, Value::of_real(1.0));
	boot.run(120);
	CHECK(blobs.writes == 2);
	CHECK(boot.store.size() == 2);
}

TEST_CASE("ValueStore: the hourly budget bounds the writes; the rest wait and are written later") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	ValueStoreLimits limits;
	limits.writes_per_hour = 10;
	Boot boot(blobs, kWall, limits);
	std::vector<std::string> names;
	std::vector<DatapointSpec> specs;
	for (int i = 0; i < 20; ++i) {
		names.push_back("Sensor." + std::to_string(i));
	}
	for (const std::string& name : names) {
		specs.push_back(DatapointSpec{name.c_str(), ValueType::signed_int, kP});
	}
	boot.load(specs.data(), specs.size());

	for (uint16_t i = 0; i < 20; ++i) {
		REQUIRE(boot.model.report(boot.bus, i, Value::of_signed(i)) == DatapointStatus::ok);
	}
	boot.loop.run_once(boot.clock.now_ms);
	CHECK(blobs.writes == 10); // the full bucket
	CHECK(boot.store.pending() == 10);
	CHECK(capture.sink.contains("write budget of 10 per hour exhausted: 10 values wait"));

	// The bucket refills at 10 per hour: one write every 6 minutes.
	boot.run(359);
	CHECK(blobs.writes == 10);
	boot.run(6);
	CHECK(blobs.writes == 11);
	boot.run(3600);
	CHECK(blobs.writes == 20);
	CHECK(boot.store.pending() == 0);
	CHECK(capture.sink.contains("write budget recovered"));
	// Over the whole time no more than the budget: 10 + 10 per hour.
	for (const auto& entry : blobs.writes_of) {
		CHECK(entry.second == 1);
	}
}

TEST_CASE("ValueStore: an unchanged value is re-written only to refresh an old saved time") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(kSpecs, 3);
	boot.report(1, Value::of_bool(true));
	CHECK(blobs.writes == 1);
	for (int i = 0; i < 59; ++i) {
		boot.run(60);
		boot.report(1, Value::of_bool(true)); // a push button repeating "on"
	}
	CHECK(blobs.writes == 1);
	boot.run(60);
	boot.report(1, Value::of_bool(true)); // the saved time is an hour old now
	CHECK(blobs.writes == 2);
	CHECK(decode_blob(blobs, "Living.Light").wall_s == kWall + 3600);
}

TEST_CASE("ValueStore: after a reboot the last value is restored stale, with its saved time") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	{
		Boot first(blobs);
		first.load(kSpecs, 3);
		first.report(0, Value::of_real(20.5));
		first.run(30);
		first.report(1, Value::of_bool(true));
		first.report(2, Value::of_real(7.0));
	}
	Boot second(blobs, kWall + 3 * 3600); // the power was off for 3 hours
	second.load(kSpecs, 3);
	const hc::Datapoint* temp = second.model.get(0);
	CHECK(temp->quality() == Quality::stale);
	CHECK(temp->value() == Value::of_real(20.5));
	CHECK(temp->updated_ms() == 0);
	CHECK(second.store.wall_time(0) == kWall);
	CHECK(second.model.get(1)->value() == Value::of_bool(true));
	CHECK(second.store.wall_time(1) == kWall + 30);
	// Not persisted: no value.
	CHECK(second.model.get(2)->quality() == Quality::unknown);
	CHECK_FALSE(second.store.wall_time(2).has_value());
	CHECK(blobs.writes == 2); // restoring writes nothing
	CHECK(capture.sink.contains("2 persisted datapoints, 2 restored"));

	// The bus confirms the same value: good again, and the old saved time is refreshed.
	second.report(0, Value::of_real(20.5));
	CHECK(temp->quality() == Quality::good);
	CHECK(blobs.writes == 3);
	CHECK(second.store.wall_time(0) == kWall + 3 * 3600);
}

TEST_CASE("ValueStore: a datapoint that never had a value stays unknown after a reboot") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	{
		Boot first(blobs);
		first.load(kSpecs, 3);
		first.report(0, Value::of_real(20.5));
	}
	Boot second(blobs);
	second.load(kSpecs, 3);
	CHECK(second.model.get(0)->quality() == Quality::stale);
	CHECK(second.model.get(1)->quality() == Quality::unknown);
	CHECK(second.model.get(1)->value().empty());
}

TEST_CASE("ValueStore: a record with an unknown field restores; a newer generation or a bad checksum is ignored") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	uint8_t buffer[ValueStore::kMaxRecord];
	size_t length = ValueStore::encode("Living.Temp", Value::of_real(19.25), kWall - 600, buffer, sizeof(buffer));
	REQUIRE(length > 0);
	std::vector<uint8_t> record(buffer, buffer + length);
	// A field of a newer firmware (tag 9, three bytes) in the middle of the record.
	std::vector<uint8_t> extended(record.begin(), record.begin() + 3);
	extended.insert(extended.end(), {9, 3, 0xAA, 0xBB, 0xCC});
	extended.insert(extended.end(), record.begin() + 3, record.end());
	reseal(extended);
	blobs.blobs[blob_of("Living.Temp")] = extended;

	std::vector<uint8_t> newer = record;
	newer[2] = ValueStore::kRecordVersion + 1;
	reseal(newer);
	blobs.blobs[blob_of("Living.Light")] = newer;

	Boot boot(blobs);
	boot.load(kSpecs, 3);
	CHECK(boot.model.get(0)->value() == Value::of_real(19.25));
	CHECK(boot.model.get(0)->quality() == Quality::stale);
	CHECK(boot.store.wall_time(0) == kWall - 600);
	CHECK(boot.model.get(1)->quality() == Quality::unknown);
	CHECK(capture.sink.contains("written by a newer firmware"));

	// A flipped bit.
	std::vector<uint8_t> corrupt = record;
	corrupt[8] ^= 0x01;
	ValueStore::Record out{};
	const char* error = nullptr;
	CHECK_FALSE(ValueStore::decode(corrupt.data(), corrupt.size(), out, &error));
	CHECK(std::string(error) == "checksum mismatch");
	// Truncated.
	CHECK_FALSE(ValueStore::decode(record.data(), 5, out, &error));
}

TEST_CASE("ValueStore: every value type survives encode and decode") {
	hc::DateTime dt;
	dt.year = 2026;
	dt.month = 10;
	dt.day = 17;
	dt.day_of_week = 6;
	dt.hour = 13;
	dt.minute = 45;
	dt.second = 30;
	dt.flags = hc::DateTime::kSummerTime | hc::DateTime::kClockQuality;
	const uint8_t raw[] = {1, 2, 3, 0, 255};
	const Value values[] = {
		Value::of_bool(true),
		Value::of_bool(false),
		Value::of_unsigned(4294967295u),
		Value::of_signed(-2147483647 - 1),
		Value::of_real(-273.15),
		Value::of_real(1e300),
		Value::of_enum(7),
		*Value::of_string("Grüße aus München"),
		*Value::of_string(""),
		*Value::of_raw(raw, sizeof(raw)),
		Value::of_time(hc::TimeOfDay{2, 23, 59, 58}),
		Value::of_date(hc::Date{2099, 12, 31}),
		Value::of_date_time(dt),
		Value::of_controlled(true, 7),
		Value::of_color(hc::Color{10, 20, 30, 40, hc::Color::kAll}),
	};
	for (const Value& value : values) {
		uint8_t buffer[ValueStore::kMaxRecord];
		std::string name(hc::Datapoint::kMaxName, 'n');
		size_t length = ValueStore::encode(name.c_str(), value, std::nullopt, buffer, sizeof(buffer));
		REQUIRE(length > 0);
		ValueStore::Record record{};
		const char* error = nullptr;
		REQUIRE(ValueStore::decode(buffer, length, record, &error));
		char text[64];
		INFO(value.format(text, sizeof(text)));
		CHECK(record.value == value);
		CHECK(record.name == name);
		CHECK_FALSE(record.wall_s.has_value());
	}
}

TEST_CASE("ValueStore: a record whose type no longer converts is not restored") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	{
		const DatapointSpec text[] = {{"Living.Temp", ValueType::string, kP}};
		Boot first(blobs);
		first.load(text, 1);
		first.report(0, *Value::of_string("warm"));
	}
	{
		const DatapointSpec integer[] = {{"Living.Temp", ValueType::signed_int, kP}};
		Boot second(blobs);
		second.load(integer, 1);
		CHECK(second.model.get(0)->quality() == Quality::unknown);
		CHECK(capture.sink.contains("refusing restore of 'Living.Temp': a string value for a signed datapoint"));
		second.report(0, Value::of_signed(21));
	}
	// An exact conversion restores (signed 21 into a real datapoint).
	Boot third(blobs);
	third.load(kSpecs, 3);
	CHECK(third.model.get(0)->value() == Value::of_real(21.0));
}

TEST_CASE("ValueStore: records of datapoints no longer persisted are deleted at load") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	{
		Boot first(blobs);
		first.load(kSpecs, 3);
		first.report(0, Value::of_real(20.5));
		first.report(1, Value::of_bool(true));
	}
	blobs.blobs["h.12345678"] = {1, 2, 3}; // someone else's prefix stays
	const DatapointSpec fewer[] = {{"Living.Temp", ValueType::real, kP}, {"Living.Light", ValueType::boolean, 0}};
	Boot second(blobs);
	second.load(fewer, 2);
	CHECK(blobs.blobs.count(blob_of("Living.Temp")) == 1);
	CHECK(blobs.blobs.count(blob_of("Living.Light")) == 0);
	CHECK(blobs.blobs.count("h.12345678") == 1);
	CHECK(capture.sink.contains("deleted " + blob_of("Living.Light")));
}

TEST_CASE("ValueStore: flush() writes everything pending; a reload writes the previous image's pending values") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	ValueStoreLimits limits;
	limits.writes_per_hour = 1;
	Boot boot(blobs, kWall, limits);
	boot.load(kSpecs, 3);
	boot.report(0, Value::of_real(1.0));
	boot.report(1, Value::of_bool(true));
	CHECK(blobs.writes == 1);
	CHECK(boot.store.pending() == 1);
	boot.store.flush();
	CHECK(blobs.writes == 2);

	boot.run(120);
	boot.report(0, Value::of_real(2.0)); // waits for the budget
	CHECK(boot.store.pending() == 1);
	// An image swap: the model is replaced, the store writes the pending value from its own copy.
	const DatapointSpec swapped[] = {{"Other", ValueType::real, 0}, {"Living.Temp", ValueType::real, kP}};
	REQUIRE(boot.model.load(swapped, 2) == DatapointStatus::ok);
	REQUIRE(boot.store.load() == DatapointStatus::ok);
	CHECK(decode_blob(blobs, "Living.Temp").value == Value::of_real(2.0));
	CHECK(boot.model.get(1)->value() == Value::of_real(2.0));
	CHECK(boot.model.get(1)->quality() == Quality::stale);
}

TEST_CASE("ValueStore: a failing store is retried after the interval and logged once") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.load(kSpecs, 3);
	blobs.fail_writes = true;
	boot.report(0, Value::of_real(1.0));
	boot.run(200);
	CHECK(blobs.writes == 0);
	CHECK(capture.sink.count("cannot save 'Living.Temp'") == 1);
	blobs.fail_writes = false;
	boot.run(60);
	CHECK(blobs.writes == 1);
	CHECK(capture.sink.contains("saving values works again"));
}

TEST_CASE("ValueStore: without a synchronized clock the time is derived once the clock is known") {
	ScopedCapture capture;
	FakeBlobStore blobs;
	Boot boot(blobs);
	boot.clock.synchronized = false;
	boot.load(kSpecs, 3);
	boot.report(0, Value::of_real(5.0));
	CHECK(blobs.writes == 1);
	CHECK_FALSE(decode_blob(blobs, "Living.Temp").wall_s.has_value());
	CHECK_FALSE(boot.store.wall_time(0).has_value());
	boot.run(100);
	boot.clock.synchronized = true;
	CHECK(boot.store.wall_time(0) == kWall); // 100 s before now
	// The next write carries the derived time.
	boot.report(0, Value::of_real(6.0));
	CHECK(decode_blob(blobs, "Living.Temp").wall_s == kWall + 100);
}

TEST_CASE("ValueStore: restore() of the model refuses a datapoint that has a value already") {
	ScopedCapture capture;
	FakeClock clock;
	DatapointModel model(clock);
	Bus bus;
	REQUIRE(model.load(kSpecs, 3) == DatapointStatus::ok);
	REQUIRE(model.bind(0, bus) == DatapointStatus::ok);
	REQUIRE(model.report(bus, 0, Value::of_real(1.0)) == DatapointStatus::ok);
	CHECK(model.restore(0, Value::of_real(2.0)) == DatapointStatus::busy);
	CHECK(model.get(0)->value() == Value::of_real(1.0));
	CHECK(model.restore(9, Value::of_real(2.0)) == DatapointStatus::unknown_id);
	CHECK(model.restore(1, Value::of_real(2.0)) == DatapointStatus::wrong_type);
	CHECK(model.restore(1, Value::of_bool(true)) == DatapointStatus::ok);
	CHECK(model.get(1)->quality() == Quality::stale);
}
