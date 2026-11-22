/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The Zigbee device database behind the write queue (homecontrol #111): its saves are written by the
// queue's worker, never on the loop, without a copy in the queue's arena; and the move of the blob
// out of the NVS store of the firmware before.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "allocation_counter.h"
#include "doctest.h"
#include "fake_blob_store.h"
#include "fake_worker.h"
#include "hc/blob_queue.h"
#include "hc/zigbee/adapter.h"
#include "hc/zigbee/device_database.h"
#include "test_support.h"
#include "zigbee_test_support.h"

using hc::BlobQueue;
using hc::BlobQueueLimits;
using hc::BlobStatus;
using hc::zigbee::DeviceDatabase;
using hc::zigbee::Ieee;
using hc::zigbee::ZigbeeAdapter;
using hc::zigbee::ZigbeeDevice;

namespace {

constexpr const char* kBlob = DeviceDatabase::kBlobName;

std::string json_of(const DeviceDatabase& database) {
	std::vector<char> buffer(hc::BlobStore::kMaxBlobSize + 1);
	hc::JsonWriter json(buffer.data(), buffer.size());
	database.write_json(json);
	REQUIRE_FALSE(json.overflowed());
	return std::string(json.data(), json.length());
}

std::string text_of(const hc_test::FakeBlobStore& store) {
	auto it = store.blobs.find(kBlob);
	return it == store.blobs.end() ? std::string("<none>") : std::string(it->second.begin(), it->second.end());
}

/// `count` devices with every text at its longest and two endpoints of 12 + 12 clusters: about 470
/// bytes of JSON each (30 of them about 14 KB, more than the queue's arena of 8 KB).
void fill(DeviceDatabase& database, int count) {
	uint16_t clusters[] = {0x0000, 0x0001, 0x0003, 0x0004, 0x0005, 0x0006, 0x0008, 0x0300, 0x0402, 0x0405, 0x0b05, 0x1000};
	for (int i = 0; i < count; ++i) {
		ZigbeeDevice* device = database.add(0x00124b0000000100ull + static_cast<Ieee>(i), static_cast<uint16_t>(0x100 + i));
		REQUIRE(device != nullptr);
		std::strcpy(device->manufacturer, "A manufacturer of 32 characters!");
		std::strcpy(device->model, "A model name with 32 characters!");
		std::snprintf(device->label, sizeof(device->label), "Device number %d of thirty", i % 100);
		device->interviewed = true;
		device->receiver = hc::zigbee::Receiver::on_when_idle;
		device->last_seen_s = 1760000000 + i;
		device->endpoint_count = 2;
		for (size_t e = 0; e < 2; ++e) {
			device->endpoints[e].id = static_cast<uint8_t>(e + 1);
			device->endpoints[e].profile = 0x0104;
			device->endpoints[e].device = 0x0100;
			device->endpoints[e].set_clusters(clusters, 12, clusters, 12);
		}
	}
}

/// The queue as the C6 has it (8 KB arena, 24 entries) in front of a store in memory, its worker
/// stepped by the test.
struct Queue {
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc_test::FakeBlobStore store;
	hc_test::ManualWorker worker;
	BlobQueue queue;

	Queue() : queue(loop, clock, store, &worker, BlobQueueLimits()) {
		clock.now_ms = 1000;
		loop.set_clock(&clock);
	}

	void run_until(uint64_t until_ms) {
		for (;;) {
			loop.run_once(clock.now_ms);
			uint64_t deadline = loop.next_deadline();
			if (deadline > until_ms) {
				clock.now_ms = until_ms;
				loop.run_once(clock.now_ms);
				return;
			}
			if (deadline > clock.now_ms) {
				clock.now_ms = deadline;
			}
		}
	}

	void drain() {
		for (int i = 0; i < 20 && worker.pending; ++i) {
			worker.step();
		}
	}
};

/// One blob in a buffer reserved up front: its writes allocate nothing, so a test counts the
/// queue's allocations alone.
struct FixedStore : hc::BlobStore {
	FixedStore() { content.reserve(kMaxBlobSize); }

	BlobStatus write(const char* name, const hc::BlobChunk* chunks, size_t count) override {
		if (fail_writes) {
			return BlobStatus::storage_error;
		}
		REQUIRE(std::strcmp(name, kBlob) == 0);
		content.clear();
		for (size_t i = 0; i < count; ++i) {
			const uint8_t* p = static_cast<const uint8_t*>(chunks[i].data);
			content.insert(content.end(), p, p + chunks[i].size);
		}
		present = true;
		++writes;
		return BlobStatus::ok;
	}
	BlobStatus read(const char*, size_t offset, void* buffer, size_t size, size_t& length) const override {
		length = 0;
		if (!present) {
			return BlobStatus::not_found;
		}
		if (offset < content.size()) {
			length = content.size() - offset < size ? content.size() - offset : size;
			std::memcpy(buffer, content.data() + offset, length);
		}
		return BlobStatus::ok;
	}
	BlobStatus size(const char*, size_t& size) const override {
		size = content.size();
		return present ? BlobStatus::ok : BlobStatus::not_found;
	}
	BlobStatus remove(const char*) override {
		bool was = present;
		present = false;
		return was ? BlobStatus::ok : BlobStatus::not_found;
	}
	void visit(hc::BlobVisitor& visitor) const override {
		if (present) {
			visitor.blob(kBlob, content.size());
		}
	}

	std::vector<uint8_t> content;
	bool present = false;
	bool fail_writes = false;
	int writes = 0;
};

/// A store whose reads give back one byte changed: a copy that does not verify.
struct GarblingStore : hc_test::FakeBlobStore {
	BlobStatus read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const override {
		BlobStatus status = hc_test::FakeBlobStore::read(name, offset, buffer, size, length);
		if (status == BlobStatus::ok && length > 0 && offset == 0) {
			static_cast<char*>(buffer)[0] ^= 0x20;
		}
		return status;
	}
};

} // namespace

// ---- Saves off the loop ---------------------------------------------------------------------------

TEST_CASE("ZigbeeDatabaseStore: a save is written by the worker, not on the loop, and takes nothing of the arena") {
	Queue q;
	DeviceDatabase database(&q.queue);
	fill(database, 30);
	std::string json = json_of(database);
	MESSAGE("JSON of 30 devices: " << json.size() << " bytes");
	REQUIRE(json.size() > BlobQueueLimits().arena_bytes); // the case #101 sent to the flash on the loop
	REQUIRE(json.size() < hc::BlobStore::kMaxBlobSize);

	REQUIRE(database.save());
	CHECK(q.store.writes == 0); // nothing on the loop
	CHECK(q.queue.direct() == 0);
	CHECK(q.queue.arena_used() == 0);
	CHECK(q.worker.pending);
	// Read your writes: the queue answers from the database.
	size_t size = 0;
	REQUIRE(q.queue.size(kBlob, size) == BlobStatus::ok);
	CHECK(size == json.size());

	q.worker.step();
	CHECK(q.store.writes_of[kBlob] == 1);
	CHECK(text_of(q.store) == json);
	CHECK(q.queue.idle());

	DeviceDatabase copy(&q.queue);
	REQUIRE(copy.load());
	CHECK(copy.size() == 30);
	CHECK(json_of(copy) == json);
}

TEST_CASE("ZigbeeDatabaseStore: saves while one is written cost nothing and hold one buffer of the JSON at most") {
	hc_test::FakeClock clock;
	clock.now_ms = 1000;
	hc::EventLoop loop;
	loop.set_clock(&clock);
	FixedStore store;
	hc_test::ManualWorker worker;
	BlobQueue queue(loop, clock, store, &worker);
	DeviceDatabase database(&queue);
	fill(database, 30);
	size_t json = json_of(database).size();
	database.at(0).short_addr = 0x2002;
	size_t latest = json_of(database).size();
	database.at(0).short_addr = 0x100;
	{
		hc_test::AllocationScope counting;
		REQUIRE(database.save()); // produced at once: the worker is free
		CHECK(counting.live() == json + 1);
		// Three more changes while the worker writes: one waiting write, no copy.
		for (int i = 0; i < 3; ++i) {
			database.at(0).short_addr = static_cast<uint16_t>(0x2000 + i);
			REQUIRE(database.save());
		}
		CHECK(counting.live() == json + 1);
		CHECK(counting.count() == 1);
		CHECK(queue.pending() == 2);
		worker.step(); // the first done: the waiting one is produced now, with the latest content
		CHECK(counting.live() == latest + 1);
		CHECK(counting.count() == 2);
		worker.step();
		CHECK(counting.live() == 0);
		CHECK(counting.peak() <= latest + 1); // never two at once
	}
	CHECK(store.writes == 2);
	CHECK(queue.idle());
	DeviceDatabase copy(&store);
	REQUIRE(copy.load());
	CHECK(copy.at(0).short_addr == 0x2002);
}

TEST_CASE("ZigbeeDatabaseStore: a failed write is retried with the latest content and holds no buffer meanwhile") {
	hc_test::ScopedCapture capture;
	hc_test::FakeClock clock;
	clock.now_ms = 1000;
	hc::EventLoop loop;
	loop.set_clock(&clock);
	FixedStore store;
	hc_test::ManualWorker worker;
	BlobQueue queue(loop, clock, store, &worker);
	DeviceDatabase database(&queue);
	fill(database, 2);
	store.fail_writes = true;
	REQUIRE(database.save());
	worker.step();
	CHECK(capture.sink.contains("cannot write zigbee-devices.json: storage error; trying again in 30 s"));
	CHECK(queue.pending() == 1);
	size_t json = json_of(database).size();
	{
		hc_test::AllocationScope counting;
		// The retry, failing again (logged once per streak): the buffer lives for the attempt only.
		clock.now_ms += BlobQueueLimits().retry_ms;
		loop.run_once(clock.now_ms);
		REQUIRE(worker.pending);
		CHECK(counting.live() == json + 1);
		worker.step();
		CHECK(counting.live() == 0);
	}
	database.at(1).short_addr = 0x4242; // changed while the retry waits, without a save
	store.fail_writes = false;
	clock.now_ms += BlobQueueLimits().retry_ms;
	loop.run_once(clock.now_ms);
	REQUIRE(worker.pending);
	worker.step();
	CHECK(capture.sink.contains("writing the flash works again (write of zigbee-devices.json)"));
	CHECK(queue.idle());
	DeviceDatabase copy(&store);
	REQUIRE(copy.load());
	CHECK(copy.find(0x00124b0000000101ull)->short_addr == 0x4242);
}

TEST_CASE("ZigbeeDatabaseStore: a database that goes while its save waits leaves the content with the queue") {
	Queue q;
	std::string json;
	{
		DeviceDatabase database(&q.queue);
		fill(database, 3);
		REQUIRE(database.save()); // in flight
		database.at(2).short_addr = 0x3333;
		REQUIRE(database.save()); // waits
		json = json_of(database);
	}
	q.drain();
	CHECK(q.store.writes_of[kBlob] == 2);
	CHECK(text_of(q.store) == json);
	CHECK(q.queue.idle());
}

TEST_CASE("ZigbeeDatabaseStore: the adapter's changes are written by the worker; a restart reads the new store") {
	hc_test::FakeClock clock;
	clock.now_ms = 1000;
	hc::EventLoop loop;
	loop.set_clock(&clock);
	hc_test::FakeBlobStore direct;
	{
		hc::DatapointModel model{clock};
		hc_test::FakeZigbeeDriver driver;
		hc_test::ManualWorker worker;
		BlobQueue queue(loop, clock, direct, &worker);
		ZigbeeAdapter adapter(loop, clock, model, driver, &queue);
		hc_test::ZigbeeActions device(adapter);
		REQUIRE(adapter.start());
		adapter.network_state(true, 0x1a62, 15, 0xdddddddddddddddd);
		device.join_sensor(); // the interview's end saves
		CHECK(direct.writes == 0);
		REQUIRE(worker.pending);
		worker.step();
		CHECK(direct.writes_of[kBlob] == 1);
		CHECK(queue.direct() == 0);

		// A new short address (homecontrol #109) is saved with the next check, by the worker.
		adapter.device_announced(hc_test::ZigbeeActions::node(hc_test::kSensor, 0x7d31), 0x80);
		uint64_t until = clock.now_ms + ZigbeeAdapter::kCheckPeriodMs;
		while (clock.now_ms < until) {
			loop.run_once(clock.now_ms);
			uint64_t deadline = loop.next_deadline();
			clock.now_ms = deadline > until ? until : deadline;
		}
		loop.run_once(clock.now_ms);
		CHECK(direct.writes_of[kBlob] == 1);
		REQUIRE(worker.pending);
		worker.step();
		CHECK(direct.writes_of[kBlob] == 2);
		CHECK(queue.idle());
	}
	hc::DatapointModel model{clock};
	hc_test::FakeZigbeeDriver driver;
	hc_test::ManualWorker worker;
	BlobQueue queue(loop, clock, direct, &worker);
	ZigbeeAdapter adapter(loop, clock, model, driver, &queue);
	REQUIRE(adapter.start());
	REQUIRE(adapter.devices().find(hc_test::kSensor) != nullptr);
	CHECK(adapter.devices().find(hc_test::kSensor)->short_addr == 0x7d31);
	CHECK(adapter.devices().find(hc_test::kSensor)->interviewed);
}

// ---- The move out of the store of the firmware before ----------------------------------------------

TEST_CASE("ZigbeeDatabaseStore: the blob of the previous store is moved once, verified, then removed there") {
	hc_test::ScopedCapture capture;
	hc_test::FakeBlobStore legacy;
	{
		DeviceDatabase old(&legacy);
		fill(old, 2);
		REQUIRE(old.save());
	}
	std::string before = text_of(legacy);
	Queue q;
	{
		DeviceDatabase database(&q.queue);
		database.set_legacy(&legacy, &q.store);
		REQUIRE(database.load());
		CHECK(database.size() == 2);
		CHECK(json_of(database) == before);
	}
	CHECK(text_of(q.store) == before); // byte for byte
	CHECK(legacy.blobs.count(kBlob) == 0);
	CHECK(capture.sink.contains("device database moved to the blob store"));
	CHECK(q.queue.idle());

	// The next start: nothing left to move; the new store is read.
	DeviceDatabase again(&q.queue);
	again.set_legacy(&legacy, &q.store);
	REQUIRE(again.load());
	CHECK(json_of(again) == before);
	CHECK(q.store.writes_of[kBlob] == 1);
}

TEST_CASE("ZigbeeDatabaseStore: a move whose write fails keeps the old blob and runs on it; the next start moves it") {
	hc_test::ScopedCapture capture;
	hc_test::FakeBlobStore legacy;
	{
		DeviceDatabase old(&legacy);
		fill(old, 2);
		REQUIRE(old.save());
	}
	std::string before = text_of(legacy);
	Queue q;
	q.store.fail_writes = true;
	{
		DeviceDatabase database(&q.queue);
		database.set_legacy(&legacy, &q.store);
		REQUIRE(database.load());
		CHECK(database.size() == 2); // the devices are there
	}
	CHECK(text_of(legacy) == before); // kept
	CHECK(q.store.blobs.count(kBlob) == 0);
	CHECK(capture.sink.contains("W hc.zigbee: device database not moved to the blob store (storage error): the previous "
		"store keeps it"));

	q.store.fail_writes = false;
	DeviceDatabase database(&q.queue);
	database.set_legacy(&legacy, &q.store);
	REQUIRE(database.load());
	CHECK(json_of(database) == before);
	CHECK(text_of(q.store) == before);
	CHECK(legacy.blobs.count(kBlob) == 0);
}

TEST_CASE("ZigbeeDatabaseStore: a copy that reads back different is removed and the old blob kept") {
	hc_test::ScopedCapture capture;
	hc_test::FakeBlobStore legacy;
	{
		DeviceDatabase old(&legacy);
		fill(old, 1);
		REQUIRE(old.save());
	}
	GarblingStore target;
	DeviceDatabase database(&target);
	database.set_legacy(&legacy, &target);
	REQUIRE(database.load());
	CHECK(database.size() == 1);
	CHECK(legacy.blobs.count(kBlob) == 1);
	CHECK(target.blobs.count(kBlob) == 0);
	CHECK(capture.sink.contains("not moved to the blob store (the copy reads back different)"));
}

TEST_CASE("ZigbeeDatabaseStore: after a failed move the saves go to the new store, which wins at the next start") {
	hc_test::ScopedCapture capture;
	hc_test::FakeBlobStore legacy;
	{
		DeviceDatabase old(&legacy);
		fill(old, 2);
		REQUIRE(old.save());
	}
	std::string before = text_of(legacy);
	Queue q;
	q.store.fail_writes = true;
	{
		DeviceDatabase database(&q.queue);
		database.set_legacy(&legacy, &q.store);
		REQUIRE(database.load());
		q.store.fail_writes = false; // the store recovers
		database.at(0).short_addr = 0x5151;
		REQUIRE(database.save());
		q.drain();
	}
	CHECK(text_of(legacy) == before); // still the old one
	DeviceDatabase database(&q.queue);
	database.set_legacy(&legacy, &q.store);
	REQUIRE(database.load());
	CHECK(database.at(0).short_addr == 0x5151); // the newer one
	CHECK(legacy.blobs.count(kBlob) == 0);
	CHECK(capture.sink.contains("the device database of the previous store removed"));
}

TEST_CASE("ZigbeeDatabaseStore: a new blob that does not load leaves the old one loaded and kept") {
	hc_test::ScopedCapture capture;
	hc_test::FakeBlobStore legacy;
	{
		DeviceDatabase old(&legacy);
		fill(old, 2);
		REQUIRE(old.save());
	}
	hc_test::FakeBlobStore target;
	const char damaged[] = "{\"format\":1,\"devices\":[{\"ieee\":";
	target.blobs[kBlob].assign(damaged, damaged + std::strlen(damaged));
	DeviceDatabase database(&target);
	database.set_legacy(&legacy, &target);
	REQUIRE(database.load());
	CHECK(database.size() == 2);
	CHECK(legacy.blobs.count(kBlob) == 1);
	CHECK(capture.sink.contains("zigbee-devices.json unreadable"));
}

TEST_CASE("ZigbeeDatabaseStore: an old blob the load refuses is not moved") {
	hc_test::FakeBlobStore legacy;
	const char damaged[] = "{\"format\":2,\"devices\":[]}";
	legacy.blobs[kBlob].assign(damaged, damaged + std::strlen(damaged));
	hc_test::FakeBlobStore target;
	DeviceDatabase database(&target);
	database.set_legacy(&legacy, &target);
	const char* error = nullptr;
	CHECK_FALSE(database.load(&error));
	CHECK(error != nullptr);
	CHECK(target.blobs.count(kBlob) == 0);
	CHECK(legacy.blobs.count(kBlob) == 1);
}
