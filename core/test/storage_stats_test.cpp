/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The storage figures off the loop (homecontrol #74): measured once at start and again after a
// change of the blob store, on a worker of their own; the polled endpoints read the last
// measurement and walk nothing. (The runtime image lives in its slot, not in the blob store, since
// issue #87: it has no storable to compute any more.)

#include <string>

#include "doctest.h"
#include "fake_blob_store.h"
#include "fake_worker.h"
#include "hc/image/image_service.h"
#include "hc/image/memory_image_slot.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "hc/storage_stats.h"
#include "image_support.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"
#include "web_test_support.h"

using hc::HttpMethod;
using hc::StorageStats;
using hc::image::ImageService;
using hc::image::Source;

namespace {

void put(hc_test::FakeBlobStore& blobs, const char* name, size_t size) {
	std::string data(size, 'x');
	hc::BlobChunk chunk{data.data(), data.size()};
	REQUIRE(blobs.write(name, &chunk, 1) == hc::BlobStatus::ok);
}

/// The board's partition: 204 KB, of which 52 KB used.
void board(hc_test::FakeBlobStore& blobs) {
	blobs.has_usage = true;
	blobs.total_bytes = 208896;
	blobs.used_bytes = 53248;
}

std::string fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

} // namespace

TEST_CASE("StorageStats: measured once at start, then served without a walk until the store changes") {
	hc_test::FakeBlobStore blobs;
	board(blobs);
	put(blobs, "values", 10);
	put(blobs, "image", 30);
	StorageStats stats(blobs);
	hc_test::ManualWorker worker;
	stats.set_worker(&worker);
	CHECK_FALSE(stats.figures().measured); // never measured: one starts on the worker
	CHECK(worker.runs == 1);
	worker.deliver();
	CHECK(stats.measurements() == 1);

	int walks = blobs.walks();
	for (int i = 0; i < 20; ++i) {
		const StorageStats::Figures& f = stats.figures();
		CHECK(f.measured);
		CHECK(f.files == 2);
		CHECK(f.bytes == 40);
	}
	CHECK(blobs.walks() == walks);
	CHECK(worker.runs == 1);
	CHECK(stats.measurements() == 1);

	// The figures: usage gives the free space as their difference (one walk, not two).
	const StorageStats::Figures& f = stats.figures();
	CHECK(f.has_usage);
	CHECK(f.total == 208896);
	CHECK(f.used == 53248);
	CHECK(f.has_free);
	CHECK(f.free == 208896 - 53248);
	CHECK(blobs.free_space_calls == 0);

	// measure_now() at start runs on the calling thread.
	hc_test::FakeBlobStore other;
	StorageStats direct(other);
	direct.measure_now();
	CHECK(direct.figures().measured);
	CHECK_FALSE(direct.figures().has_usage);
	CHECK(direct.figures().has_free); // from free_space(): the store has no usage()
	CHECK(other.free_space_calls == 1);
}

TEST_CASE("StorageStats: a change starts one measurement on the worker; until it is done the old figures stand") {
	hc_test::FakeBlobStore blobs;
	board(blobs);
	StorageStats stats(blobs);
	hc_test::ManualWorker worker;
	stats.set_worker(&worker);
	stats.measure_now();
	CHECK(stats.figures().files == 0);

	put(blobs, "hist.0", 968);
	int walks = blobs.walks();
	CHECK(stats.figures().files == 0); // stale, but no walk on the loop
	CHECK(blobs.walks() == walks);
	CHECK(worker.runs == 1);
	CHECK(stats.measuring());
	CHECK(stats.figures().files == 0); // a second request does not start a second one
	CHECK(worker.runs == 1);

	worker.run_job(); // the worker's thread walks
	CHECK(blobs.walks() > walks);
	CHECK(stats.figures().files == 0); // not yet handed over
	worker.deliver();
	CHECK_FALSE(stats.measuring());
	CHECK(stats.figures().files == 1);
	CHECK(stats.figures().bytes == 968);
	CHECK(worker.runs == 1);

	// Every kind of change counts: a patch, a removal, a streaming write's commit.
	auto measured_after = [&](auto change) {
		int runs = worker.runs;
		change();
		stats.figures();
		bool started = worker.runs == runs + 1;
		worker.deliver();
		return started;
	};
	CHECK(measured_after([&] {
		char slot[4] = {1, 2, 3, 4};
		hc::BlobChunk chunk{slot, sizeof(slot)};
		REQUIRE(blobs.patch("hist.0", 8, &chunk, 1) == hc::BlobStatus::ok);
	}));
	CHECK(measured_after([&] { REQUIRE(blobs.remove("hist.0") == hc::BlobStatus::ok); }));
	CHECK(measured_after([&] {
		hc::BlobStatus status = hc::BlobStatus::ok;
		hc::BlobWriter* writer = blobs.open_writer("i18n.de.json.gz", 0, status);
		REQUIRE(writer != nullptr);
		REQUIRE(writer->append("abc", 3) == hc::BlobStatus::ok);
		REQUIRE(writer->commit() == hc::BlobStatus::ok);
	}));
	CHECK(stats.figures().files == 1);
	CHECK(stats.figures().bytes == 3);
	// A refused write changes nothing and measures nothing.
	CHECK_FALSE(measured_after([&] { CHECK(blobs.remove("absent") == hc::BlobStatus::not_found); }));
}

TEST_CASE("StorageStats: a change while the worker measures is measured again") {
	hc_test::FakeBlobStore blobs;
	StorageStats stats(blobs);
	hc_test::ManualWorker worker;
	stats.set_worker(&worker);
	stats.measure_now();
	put(blobs, "a", 1);
	stats.figures();
	worker.run_job(); // measures "a"
	put(blobs, "b", 2); // after the walk began: not in this measurement
	worker.deliver();
	CHECK(stats.figures().files == 1); // what was measured; the figures() call starts the next
	CHECK(worker.runs == 2);
	worker.deliver();
	CHECK(stats.figures().files == 2);
	CHECK(worker.runs == 2);

	// refresh() starts one as figures() does, and nothing without a change.
	stats.refresh();
	CHECK(worker.runs == 2);
	put(blobs, "c", 3);
	stats.refresh();
	CHECK(worker.runs == 3);
	worker.deliver();
	CHECK(stats.figures().files == 3);
}

TEST_CASE("StorageStats: without a worker the measurement runs within figures(), only after a change") {
	hc_test::FakeBlobStore blobs;
	StorageStats stats(blobs);
	CHECK(stats.figures().measured); // the first call measures
	int walks = blobs.walks();
	for (int i = 0; i < 5; ++i) {
		stats.figures();
	}
	CHECK(blobs.walks() == walks);
	put(blobs, "a", 5);
	CHECK(stats.figures().files == 1);
	CHECK(blobs.walks() > walks);
	CHECK(stats.measurements() == 2);
}

TEST_CASE("StorageStats: a worker that cannot start keeps the old figures, says so once and is asked again") {
	hc_test::ScopedCapture capture;
	hc_test::FakeBlobStore blobs;
	StorageStats stats(blobs);
	hc_test::ManualWorker worker;
	stats.set_worker(&worker);
	stats.measure_now();
	worker.refuse = "cannot create the worker task (memory)";
	put(blobs, "a", 1);
	int walks = blobs.walks();
	CHECK(stats.figures().files == 0);
	CHECK(stats.figures().files == 0);
	CHECK(blobs.walks() == walks); // never on the loop
	CHECK(capture.sink.count("not measured again (cannot create the worker task (memory))") == 1);
	worker.refuse = nullptr;
	stats.figures();
	CHECK(worker.runs == 1);
	worker.deliver();
	CHECK(stats.figures().files == 1);
}

namespace {

/// The controller with an image service and the storage figures as main() wires them on the device.
struct PolledFixture : hc_test::WebFixture {
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::MemoryImageSlot slot;
	ImageService service{controller, model, blobs, slot, adapter, &tunnel};
	StorageStats stats{blobs};
	hc_test::ManualWorker stats_worker;
	std::string cookie;

	PolledFixture() {
		board(blobs);
		blobs.free_bytes = 208896 - 53248;
		stats.set_worker(&stats_worker);
		stats.measure_now();
		service.start();
		api.set_image(&service);
		api.set_storage_stats(&stats);
		cookie = onboard();
	}
};

/// What the device UI polls while a page is open (web/ui/dist: the System page's diagnostics and
/// blocks every 5 s, the status of the Settings page and the E-Home link, the log every 3 s, the
/// rules every 10 s, the schedules every 30 s, the settings, the session).
const char* const kPolled[] = {"/api/status", "/api/system/diagnostics", "/api/blocks", "/api/log", "/api/rules",
	"/api/schedules", "/api/settings", "/api/auth/session"};

} // namespace

TEST_CASE("StorageStats: the polled endpoints walk the file system on no request") {
	PolledFixture f;
	REQUIRE_MESSAGE(hc_test::deploy(f.service, fixture("example.json")), hc_test::describe(f.service.report()));
	f.stats_worker.deliver(); // a change of the blob store by the load (values, history) measured once
	uint32_t measured = f.stats.measurements();
	int walks = f.blobs.walks();
	for (int round = 0; round < 10; ++round) {
		for (const char* path : kPolled) {
			auto r = f.get(path, f.cookie);
			CHECK_MESSAGE(r.status == 200, path << ": " << r.body);
		}
	}
	CHECK(f.blobs.walks() == walks);
	CHECK(f.stats.measurements() == measured);
	int runs = f.stats_worker.runs;
	for (const char* path : kPolled) {
		f.get(path, f.cookie);
	}
	CHECK(f.stats_worker.runs == runs);

	// What they report is the measurement: the status, the System page.
	auto status = f.get("/api/status", f.cookie);
	CHECK(status.json["storage"]["total"].number == 208896);
	CHECK(status.json["storage"]["used"].number == 53248);
	CHECK(status.json["storage"]["files"].number == static_cast<double>(f.blobs.blobs.size()));
	CHECK(status.json["image"]["slot"].number == hc::image::MemoryImageSlot::kDefaultSize);
	CHECK_FALSE(status.json["image"].has("storable")); // the image is in its slot, not in the blob store
	auto diagnostics = f.get("/api/system/diagnostics", f.cookie);
	CHECK(diagnostics.json["storage"]["available"].boolean);
	CHECK(diagnostics.json["storage"]["free"].number == 208896 - 53248);
	CHECK(f.blobs.walks() == walks);

	// A change shows with the measurement after it: the request that notices it starts one.
	f.blobs.used_bytes += 4096;
	f.blobs.touch();
	CHECK(f.get("/api/status", f.cookie).json["storage"]["used"].number == 53248);
	f.stats_worker.deliver();
	CHECK(f.get("/api/status", f.cookie).json["storage"]["used"].number == 53248 + 4096);
	CHECK(f.blobs.walks() > walks); // on the worker's thread
}
