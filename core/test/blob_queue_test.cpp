/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The write queue in front of the blob store (homecontrol #101): the loop's flash writes on a worker.
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "fake_worker.h"
#include "hc/blob_queue.h"
#include "hc/datapoint.h"
#include "hc/technology.h"
#include "hc/value_store.h"
#include "hc/event_loop.h"
#include "hc/shutdown.h"
#include "test_support.h"

using hc::BlobChunk;
using hc::BlobQueue;
using hc::BlobQueueLimits;
using hc::BlobStatus;

namespace {

struct Bench {
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc_test::FakeBlobStore store;
	hc_test::ManualWorker worker;
	BlobQueue queue;

	explicit Bench(BlobQueueLimits limits = small()) : queue(loop, clock, store, &worker, limits) {
		clock.now_ms = 1000;
		loop.set_clock(&clock);
	}

	static BlobQueueLimits small() {
		BlobQueueLimits l;
		l.arena_bytes = 64;
		l.entries = 4;
		l.retry_ms = 1000;
		l.attempts = 3;
		return l;
	}

	BlobStatus put(const char* name, const std::string& text) {
		BlobChunk chunk{text.data(), text.size()};
		return queue.write(name, &chunk, 1);
	}

	std::string get(const char* name, BlobStatus* status = nullptr) {
		char buffer[128];
		size_t length = 0;
		BlobStatus s = queue.read(name, 0, buffer, sizeof(buffer), length);
		if (status != nullptr) {
			*status = s;
		}
		return std::string(buffer, length);
	}

	std::string stored(const char* name) {
		auto it = store.blobs.find(name);
		return it == store.blobs.end() ? std::string("<none>") : std::string(it->second.begin(), it->second.end());
	}

	/// Runs the worker until the queue is idle (or `limit` jobs).
	void drain(int limit = 20) {
		for (int i = 0; i < limit && worker.pending; ++i) {
			worker.step();
		}
	}
};

struct Names : hc::BlobVisitor {
	std::map<std::string, size_t> seen;
	void blob(const char* name, size_t size) override { seen[name] = size; }
};

} // namespace

TEST_CASE("BlobQueue.AWriteReturnsAtOnceAndReachesTheStoreOnTheWorker") {
	Bench b;
	CHECK(b.put("v.1", "hello") == BlobStatus::ok);
	CHECK(b.store.writes == 0); // nothing on the loop
	CHECK(b.worker.pending);
	CHECK(b.get("v.1") == "hello"); // read your writes
	size_t size = 0;
	CHECK(b.queue.size("v.1", size) == BlobStatus::ok);
	CHECK(size == 5);
	b.drain();
	CHECK(b.store.writes == 1);
	CHECK(b.stored("v.1") == "hello");
	CHECK(b.queue.idle());
	CHECK(b.get("v.1") == "hello");
	CHECK(b.queue.performed() == 1);
}

TEST_CASE("BlobQueue.AWaitingWriteIsReplacedNotTheOneInFlight") {
	Bench b;
	b.put("v.1", "one");   // in flight at once
	b.put("v.1", "two");   // waits
	b.put("v.1", "three"); // replaces "two"
	CHECK(b.queue.pending() == 2);
	CHECK(b.get("v.1") == "three");
	b.worker.run_job(); // "one" on the store
	CHECK(b.stored("v.1") == "one");
	CHECK(b.get("v.1") == "three");
	b.drain();
	CHECK(b.stored("v.1") == "three");
	CHECK(b.store.writes_of["v.1"] == 2);
}

TEST_CASE("BlobQueue.ARemovalHidesTheBlobAtOnceAndVisitMergesTheQueue") {
	Bench b;
	b.store.blobs["old"] = {'x', 'y'};
	b.store.blobs["kept"] = {'k'};
	CHECK(b.queue.remove("old") == BlobStatus::ok);
	CHECK(b.queue.remove("missing") == BlobStatus::not_found);
	CHECK(b.put("new", "abc") == BlobStatus::ok);
	BlobStatus status = BlobStatus::ok;
	b.get("old", &status);
	CHECK(status == BlobStatus::not_found);
	CHECK(b.queue.remove("old") == BlobStatus::not_found); // gone already, in the queue's view
	Names names;
	b.queue.visit(names);
	CHECK(names.seen == std::map<std::string, size_t>{{"kept", 1}, {"new", 3}});
	b.drain();
	CHECK(b.store.blobs.count("old") == 0);
	CHECK(b.stored("new") == "abc");
}

TEST_CASE("BlobQueue.PatchesGoIntoAWaitingCopyOrOverlayTheStore") {
	Bench b;
	b.store.blobs["seg"] = {'a', 'a', 'a', 'a', 'a', 'a'};
	BlobChunk x{"XY", 2};
	CHECK(b.queue.patch("seg", 1, &x, 1) == BlobStatus::ok);
	BlobChunk z{"Z", 1};
	CHECK(b.queue.patch("seg", 2, &z, 1) == BlobStatus::ok); // a later patch wins where they overlap
	CHECK(b.get("seg") == "aXZaaa");
	CHECK(b.queue.patch("seg", 5, &x, 1) == BlobStatus::too_large);
	CHECK(b.queue.patch("none", 0, &x, 1) == BlobStatus::not_found);
	b.drain();
	CHECK(b.stored("seg") == "aXZaaa");
	CHECK(b.store.patches == 2);

	// A patch of a waiting write changes its copy: one write reaches the store.
	b.put("w", "first");     // in flight
	b.put("w", "000000");    // waits
	CHECK(b.queue.patch("w", 2, &x, 1) == BlobStatus::ok);
	CHECK(b.get("w") == "00XY00");
	b.drain();
	CHECK(b.stored("w") == "00XY00");
	CHECK(b.store.patches == 2);
}

TEST_CASE("BlobQueue.WhenFullTheChangeGoesToTheStoreOnTheLoopVisibly") {
	hc_test::ScopedCapture capture;
	Bench b; // 64 bytes, 4 changes
	CHECK(b.put("a", std::string(40, 'a')) == BlobStatus::ok);
	CHECK(b.put("b", std::string(40, 'b')) == BlobStatus::ok); // does not fit beside "a": at once
	CHECK(b.store.writes == 1);
	CHECK(b.stored("b") == std::string(40, 'b'));
	CHECK(b.queue.direct() == 1);
	CHECK(capture.sink.contains("write queue full"));
	// The same blob in flight and no room: busy, the writer retries.
	CHECK(b.put("a", std::string(40, 'c')) == BlobStatus::busy);
	b.drain();
	CHECK(b.stored("a") == std::string(40, 'a'));
	CHECK(b.put("a", std::string(40, 'c')) == BlobStatus::ok);
	b.drain();
	CHECK(b.stored("a") == std::string(40, 'c'));
	CHECK(b.queue.arena_used() == 0);
}

TEST_CASE("BlobQueue.HasRoomAndSettledTellAWriterWhatTheQueueWouldDo") {
	// homecontrol #118: a writer that must not write on the loop asks for room first, and a writer that
	// deletes what its write replaces waits until the write is settled.
	Bench b; // 64 bytes, 4 changes
	CHECK(b.queue.has_room(64));
	CHECK_FALSE(b.queue.has_room(65));
	CHECK(b.queue.settled("a"));
	CHECK(b.put("a", std::string(40, 'a')) == BlobStatus::ok);
	CHECK_FALSE(b.queue.settled("a"));
	CHECK(b.queue.settled("b"));
	CHECK(b.queue.has_room(24));
	CHECK_FALSE(b.queue.has_room(25)); // would go to the store on the loop
	CHECK(b.queue.has_room(0));        // a removal
	CHECK(b.put("b", std::string(24, 'b')) == BlobStatus::ok);
	CHECK(b.queue.direct() == 0);
	CHECK_FALSE(b.queue.has_room(1));
	b.worker.run_job(); // in flight: still not settled
	CHECK_FALSE(b.queue.settled("a"));
	b.worker.deliver();
	CHECK(b.queue.settled("a"));
	CHECK_FALSE(b.queue.settled("b"));
	b.drain();
	CHECK(b.queue.settled("b"));
	CHECK(b.queue.has_room(64));
	// The entries count too: four removals fill the table.
	for (const char* name : {"a", "b"}) {
		CHECK(b.queue.remove(name) == BlobStatus::ok);
	}
	CHECK(b.put("c", "c") == BlobStatus::ok);
	CHECK(b.put("d", "d") == BlobStatus::ok);
	CHECK_FALSE(b.queue.has_room(0));
	// A failed write that is dropped is settled: the store shows that it is missing.
	b.drain();
	b.store.fail_writes = true;
	CHECK(b.put("e", "e") == BlobStatus::ok);
	for (int i = 0; i < 3; ++i) {
		b.worker.deliver();
		b.clock.now_ms += 1000;
		b.loop.run_once(b.clock.now_ms);
	}
	CHECK(b.queue.dropped() == 1);
	CHECK(b.queue.settled("e"));
	size_t size = 0;
	CHECK(b.queue.size("e", size) == BlobStatus::not_found);
}

TEST_CASE("BlobQueue.AFailedWriteIsRetriedAfterTheOthersAndDroppedAfterItsAttempts") {
	hc_test::ScopedCapture capture;
	Bench b;
	b.store.fail_writes = true;
	b.put("v.1", "x");
	b.drain();
	CHECK(capture.sink.contains("cannot write v.1: storage error; trying again in 1 s"));
	CHECK(b.queue.pending() == 1);
	CHECK(b.get("v.1") == "x"); // still the queue's
	b.store.fail_writes = false;
	b.put("v.2", "y"); // another blob does not wait for the retry
	b.drain();
	CHECK(b.stored("v.2") == "y");
	CHECK(b.stored("v.1") == "<none>");
	b.clock.advance(1000);
	b.loop.run_once(b.clock.now_ms); // the retry timer
	b.drain();
	CHECK(b.stored("v.1") == "x");
	CHECK(capture.sink.contains("writing the flash works again"));

	b.store.fail_writes = true;
	b.put("v.3", "z");
	for (int i = 0; i < 5; ++i) {
		b.drain();
		b.clock.advance(1000);
		b.loop.run_once(b.clock.now_ms);
	}
	CHECK(b.queue.dropped() == 1);
	CHECK(capture.sink.contains("write of v.3 dropped after 3 attempts: storage error"));
	CHECK(b.queue.idle());
	CHECK(b.loop.timer_count() == 0);
}

TEST_CASE("BlobQueue.TheChangesOfOneBlobReachTheStoreInOrder") {
	Bench b;
	b.store.blobs["s"] = {'0', '0', '0', '0'};
	b.put("t", "x");                 // in flight
	BlobChunk one{"1", 1};
	BlobChunk two{"2", 1};
	b.queue.patch("s", 0, &one, 1);  // waits
	b.queue.remove("s");             // replaces the patch
	CHECK(b.queue.pending() == 2);
	b.put("s", "new");               // replaces the removal
	b.queue.patch("s", 0, &two, 1);  // into the waiting copy
	CHECK(b.get("s") == "2ew");
	b.drain();
	CHECK(b.stored("s") == "2ew");
	CHECK(b.store.patches == 0);
}

TEST_CASE("BlobQueue.APlannedRestartWaitsForTheQueue") {
	Bench b;
	hc::Shutdown shutdown(b.loop, b.clock);
	shutdown.add(b.queue);
	bool restarted = false;
	struct Then {
		bool* flag;
		void run() { *flag = true; }
	} then{&restarted};
	b.put("v.1", "a");
	b.put("v.2", "b");
	CHECK(shutdown.begin("test", hc::Callback::member<Then, &Then::run>(&then, "test.then")));
	CHECK_FALSE(restarted);
	b.worker.step();
	CHECK_FALSE(restarted);
	b.worker.step();
	CHECK(restarted);
	CHECK(b.stored("v.2") == "b");
}

TEST_CASE("BlobQueue.WithoutAWorkerEveryCallGoesThrough") {
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc_test::FakeBlobStore store;
	BlobQueue queue(loop, clock, store, nullptr);
	CHECK_FALSE(queue.allocated());
	BlobChunk chunk{"abc", 3};
	CHECK(queue.write("x", &chunk, 1) == BlobStatus::ok);
	CHECK(store.writes == 1);
	CHECK(queue.has_room(1 << 20)); // nothing to wait for: the call writes at once
	CHECK(queue.settled("x"));
}

TEST_CASE("BlobQueue.AStreamingWriteWaitsForTheQueuedChangesOfItsBlob") {
	Bench b;
	b.put("lang.de", "old");
	BlobStatus status = BlobStatus::ok;
	CHECK(b.queue.open_writer("lang.de", 3, status) == nullptr);
	CHECK(status == BlobStatus::busy);
	b.drain();
	hc::BlobWriter* writer = b.queue.open_writer("lang.de", 3, status);
	REQUIRE(writer != nullptr);
	writer->discard();
}

namespace {

class Bus : public hc::Technology {
public:
	const char* name() const override { return "bus"; }
	bool command(const hc::Datapoint&, const hc::Value&) override { return true; }
};

} // namespace

TEST_CASE("BlobQueue.TheValueStoreWritesOffTheLoopAndReadsItsRecordsBack") {
	// A persisted datapoint's record: the loop only queues it; a reload of the image before the
	// worker wrote it restores it from the queue; after a reboot from the store.
	hc_test::FakeBlobStore blobs;
	const hc::DatapointSpec specs[] = {{"Living.Temp", hc::ValueType::real, hc::DatapointFlags::kPersist}};
	{
		Bench b;
		b.clock.wall_seconds = 1790000000;
		b.clock.synchronized = true;
		hc::BlobQueueLimits limits;
		hc::BlobQueue queue(b.loop, b.clock, blobs, &b.worker, limits);
		hc::DatapointModel model(b.clock);
		Bus bus;
		hc::ValueStore store(b.loop, b.clock, model, queue);
		REQUIRE(model.load(specs, 1) == hc::DatapointStatus::ok);
		REQUIRE(model.bind(0, bus) == hc::DatapointStatus::ok);
		REQUIRE(store.load() == hc::DatapointStatus::ok);
		REQUIRE(model.report(bus, 0, hc::Value::of_real(21.5)) == hc::DatapointStatus::ok);
		b.loop.run_once(b.clock.now_ms); // the store's posted write
		CHECK(store.writes() == 1);
		CHECK(blobs.writes == 0); // not on the loop
		CHECK(queue.pending() == 1);

		// The same image again (a deploy): the record comes back from the queue.
		hc::DatapointModel model2(b.clock);
		hc::ValueStore store2(b.loop, b.clock, model2, queue);
		REQUIRE(model2.load(specs, 1) == hc::DatapointStatus::ok);
		REQUIRE(store2.load() == hc::DatapointStatus::ok);
		CHECK(model2.get(0)->value() == hc::Value::of_real(21.5));

		b.drain();
		CHECK(blobs.writes == 1);
		CHECK(queue.idle());
	}
	hc_test::FakeClock clock;
	clock.wall_seconds = 1790000000;
	clock.synchronized = true;
	hc::EventLoop loop;
	hc::DatapointModel model(clock);
	hc::ValueStore store(loop, clock, model, blobs);
	REQUIRE(model.load(specs, 1) == hc::DatapointStatus::ok);
	REQUIRE(store.load() == hc::DatapointStatus::ok);
	CHECK(model.get(0)->value() == hc::Value::of_real(21.5));
}

TEST_CASE("BlobQueueProbe.ARemovalAndAWriteBehindAPatchInFlight") {
	Bench b;
	b.store.blobs["seg"] = {'a', 'a', 'a', 'a'};
	BlobChunk x{"X", 1};
	REQUIRE(b.queue.patch("seg", 0, &x, 1) == BlobStatus::ok); // in flight
	CHECK(b.get("seg") == "Xaaa");                             // the store's, the patch overlaid
	CHECK(b.queue.remove("seg") == BlobStatus::ok);
	BlobStatus status = BlobStatus::ok;
	b.get("seg", &status);
	CHECK(status == BlobStatus::not_found);
	CHECK(b.queue.patch("seg", 0, &x, 1) == BlobStatus::not_found); // nothing to patch in the view
	CHECK(b.put("seg", "new") == BlobStatus::ok);                    // replaces the removal
	CHECK(b.get("seg") == "new");
	b.drain();
	CHECK(b.stored("seg") == "new");
	CHECK(b.store.patches == 1);
	CHECK(b.queue.idle());
}

TEST_CASE("BlobQueueProbe.APlannedRestartDoesNotWaitOutTheRetryTime") {
	hc_test::ScopedCapture capture;
	Bench b;
	hc::Shutdown shutdown(b.loop, b.clock);
	shutdown.add(b.queue);
	b.store.fail_writes = true;
	b.put("v.1", "a");
	b.drain(); // failed once, waits 1 s for its retry
	CHECK(b.queue.pending() == 1);
	struct Then {
		bool done = false;
		void run() { done = true; }
	} then;
	CHECK(shutdown.begin("test", hc::Callback::member<Then, &Then::run>(&then, "test.then")));
	b.drain(); // the remaining attempts at once, then dropped
	CHECK(then.done);
	CHECK(b.queue.dropped() == 1);
}

TEST_CASE("BlobQueueProbe.ANewWriteReplacesOneWaitingForItsRetryAndTheOldNeverLands") {
	hc_test::ScopedCapture capture;
	Bench b;
	b.store.fail_writes = true;
	b.put("v.1", "old");
	b.drain(); // fails, waits for its retry
	CHECK(b.queue.pending() == 1);
	b.store.fail_writes = false;
	CHECK(b.put("v.1", "new") == BlobStatus::ok); // replaces the one waiting for its retry
	CHECK(b.get("v.1") == "new");
	b.drain();
	CHECK(b.stored("v.1") == "new");
	b.clock.advance(5000);
	b.loop.run_once(b.clock.now_ms); // a stale retry timer must not bring "old" back
	b.drain();
	CHECK(b.stored("v.1") == "new");
	CHECK(b.queue.idle());
	CHECK(b.queue.dropped() == 0);
	CHECK(b.loop.timer_count() == 0);
}

TEST_CASE("BlobQueueProbe.ARemovalWhileTheWriteOfTheBlobIsInFlightLeavesNoBlob") {
	Bench b;
	b.put("v.1", "x");
	b.loop.run_once(b.clock.now_ms); // the write goes to the worker
	CHECK(b.queue.remove("v.1") == BlobStatus::ok);
	BlobStatus status = BlobStatus::ok;
	b.get("v.1", &status);
	CHECK(status != BlobStatus::ok);
	b.drain();
	CHECK(b.stored("v.1") == "<none>");
	CHECK(b.queue.idle());
}

// ---- write_from(): the content asked for when the worker takes it (homecontrol #111) ----

namespace {

/// A source whose content the test changes; counts how often it is asked.
struct TextSource : hc::BlobSource {
	std::string text;
	mutable int asked = 0;
	bool refuse = false;

	size_t content_size() const override { return text.size(); }
	bool content(uint8_t* buffer, size_t size) const override {
		++asked;
		if (refuse || size != text.size()) {
			return false;
		}
		std::memcpy(buffer, text.data(), size);
		buffer[size] = 0;
		return true;
	}
};

} // namespace

TEST_CASE("BlobQueue.AWriteFromIsProducedWhenTheWorkerTakesItAndAnsweredFromTheSourceMeanwhile") {
	Bench b;
	b.put("a", "first"); // in flight: the source's write waits behind it
	TextSource source;
	source.text = "one";
	CHECK(b.queue.write_from("db", source) == BlobStatus::ok);
	CHECK(b.queue.write_from("db", source) == BlobStatus::ok); // replaces the waiting one
	CHECK(b.queue.pending() == 2);
	CHECK(b.queue.arena_used() == 5); // only "first"
	source.text = "longer content than the arena of 64 bytes holds, which a copy could not take at all";
	CHECK(source.asked == 0);
	size_t size = 0;
	REQUIRE(b.queue.size("db", size) == BlobStatus::ok);
	CHECK(size == source.text.size());
	char piece[8] = {};
	size_t length = 0;
	REQUIRE(b.queue.read("db", 7, piece, 7, length) == BlobStatus::ok);
	CHECK(std::string(piece, length) == "content");
	Names names;
	b.queue.visit(names);
	CHECK(names.seen["db"] == source.text.size());
	int asked = source.asked;
	b.drain();
	CHECK(source.asked == asked + 1); // once, when taken
	CHECK(b.stored("db") == source.text);
	CHECK(b.queue.direct() == 0);
	CHECK(b.queue.idle());
}

TEST_CASE("BlobQueue.AWriteFromWhoseSourceRefusesIsRetriedAndDroppedLikeAFailedWrite") {
	hc_test::ScopedCapture capture;
	Bench b;
	TextSource source;
	source.text = "x";
	source.refuse = true;
	CHECK(b.queue.write_from("db", source) == BlobStatus::ok);
	CHECK(b.queue.pending() == 1);
	CHECK_FALSE(b.worker.pending); // nothing to write
	for (int i = 0; i < 3; ++i) {
		b.clock.advance(1000);
		b.loop.run_once(b.clock.now_ms);
	}
	CHECK(b.queue.idle());
	CHECK(b.queue.dropped() == 1);
	CHECK(capture.sink.contains("write of db dropped after 3 attempts"));
	CHECK(b.stored("db") == "<none>");
}

TEST_CASE("BlobQueue.AWriteFromGoesToTheStoreOnTheLoopWhenTheEntriesAreFull") {
	hc_test::ScopedCapture capture;
	Bench b; // 4 entries
	b.put("a", "1");
	b.put("b", "2");
	b.put("c", "3");
	b.put("d", "4");
	TextSource source;
	source.text = "now";
	CHECK(b.queue.write_from("db", source) == BlobStatus::ok);
	CHECK(b.stored("db") == "now");
	CHECK(b.queue.direct() == 1);
	CHECK(capture.sink.contains("write queue full"));
}

TEST_CASE("BlobQueue.WithoutAWorkerAWriteFromGoesThrough") {
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc_test::FakeBlobStore store;
	BlobQueue queue(loop, clock, store, nullptr);
	TextSource source;
	source.text = "direct";
	CHECK(queue.write_from("db", source) == BlobStatus::ok);
	CHECK(source.asked == 1);
	CHECK(std::string(store.blobs["db"].begin(), store.blobs["db"].end()) == "direct");
}
