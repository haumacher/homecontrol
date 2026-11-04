/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The largest image the device can store and load again (homecontrol #52, spec/runtime-image.md 7):
// the blob store's room beside the stored image in LittleFS blocks, less the flash the history's
// archives still take, and the stored staging's room beside a failed image; `image.storable` in
// GET /api/status; a deploy above it runs and names the limit; the receive limit of the staging area.

#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "fake_image_staging.h"
#include "fake_worker.h"
#include "hc/image/image_body.h"
#include "hc/image/image_service.h"
#include "hc/image/image_store.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "web_test_support.h"

using hc::BlobStore;
using hc::HttpMethod;
using hc::image::ImageBody;
using hc::image::ImageService;
using hc::image::ImageStore;
using hc::image::kMaxImageBytes;
using hc::image::Source;

namespace {

constexpr size_t kBlock = 4096;
/// The board's blob store at boot (8 blobs, 32410 B, the test system's image among them).
constexpr size_t kBoardFree = 155648;
/// The board's staging area behind the updater (124 KB, 31 sectors) and what one slot holds beside
/// the other (15 sectors); a body takes one byte more there (the NUL after the text).
constexpr size_t kBoardStaging = 126976;
constexpr size_t kBoardShared = 61440;
constexpr size_t kBoardReceivable = kBoardStaging - 1;
constexpr size_t kBoardRestorable = kBoardShared - 1;

std::string fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

/// The example house at `revision`, padded with whitespace to `size` bytes.
std::string example(unsigned revision, size_t size = 0) {
	std::string text = fixture("example.json");
	size_t at = text.find("\"revision\": 7");
	REQUIRE(at != std::string::npos);
	text.replace(at, std::strlen("\"revision\": 7"), "\"revision\": " + std::to_string(revision));
	if (text.size() < size) {
		text.append(size - text.size(), ' ');
	}
	return text;
}

/// A FakeBlobStore that counts its free space in LittleFS blocks as the board's store does: a write
/// needs the new file's blocks beside the old one's (atomic replace), a removal frees them.
class BlockBlobStore : public hc_test::FakeBlobStore {
public:
	explicit BlockBlobStore(size_t free) { free_bytes = free; }

	size_t footprint(size_t bytes) const override { return littlefs_footprint(bytes, kBlock); }

	hc::BlobStatus write(const char* name, const hc::BlobChunk* chunks, size_t count) override {
		size_t bytes = 0;
		for (size_t i = 0; i < count; ++i) {
			bytes += chunks[i].size;
		}
		size_t need = footprint(bytes);
		if (check_name(name) == hc::BlobStatus::ok && bytes <= kMaxBlobSize && need > free_bytes) {
			return hc::BlobStatus::no_space;
		}
		auto old = blobs.find(name);
		size_t freed = old != blobs.end() ? footprint(old->second.size()) : 0;
		hc::BlobStatus status = FakeBlobStore::write(name, chunks, count);
		if (status == hc::BlobStatus::ok) {
			free_bytes = free_bytes - need + freed;
		}
		return status;
	}

	hc::BlobStatus remove(const char* name) override {
		auto it = blobs.find(name);
		size_t freed = it != blobs.end() ? footprint(it->second.size()) : 0;
		hc::BlobStatus status = FakeBlobStore::remove(name);
		if (status == hc::BlobStatus::ok) {
			free_bytes += freed;
		}
		return status;
	}
};

struct StorableFixture : hc_test::WebFixture {
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	BlockBlobStore blobs;
	ImageService service{controller, model, blobs, adapter, &tunnel};
	hc_test::ManualWorker worker;
	hc_test::FakeImageStaging received;
	hc_test::FakeImageStaging stored;
	std::string cookie;

	explicit StorableFixture(size_t free = kBoardFree, bool with_worker = true) : blobs(free) {
		received.capacity_bytes = kBoardStaging;
		received.shared_bytes = kBoardShared;
		stored.capacity_bytes = kBoardStaging;
		stored.shared_bytes = kBoardShared;
		service.set_staging(&received);
		service.set_stored_staging(&stored);
		if (with_worker) {
			service.set_worker(&worker);
		}
		service.start();
		api.set_image(&service);
		cookie = onboard();
	}

	/// A deploy as a receiver makes it, taken through to its answer.
	Result deploy(const std::string& image) {
		ImageBody body;
		REQUIRE(body.begin(image.size(), api.body_staging(HttpMethod::put, "/api/image")));
		REQUIRE(body.append(image.data(), image.size()));
		const char* text = body.finish();
		REQUIRE(text != nullptr);
		hc::WebRequest req;
		req.method = HttpMethod::put;
		req.path = "/api/image";
		req.query = "";
		req.cookie = cookie.c_str();
		req.requested_with = "HomeControl";
		req.source = "192.0.2.7";
		req.body = text;
		req.body_length = image.size();
		req.body_holder = &body;
		std::vector<char> out(8192);
		hc::WebResponse response(out.data(), out.size());
		api.handle(req, response);
		body.release();
		if (response.deferred) {
			worker.step();
			REQUIRE(api.poll_deferred(response));
		}
		return result(response);
	}
};

} // namespace

TEST_CASE("Storable (#52): a LittleFS file takes whole blocks along its skip list") {
	CHECK(BlobStore::littlefs_footprint(0, kBlock) == kBlock);
	CHECK(BlobStore::littlefs_footprint(30, kBlock) == kBlock);
	CHECK(BlobStore::littlefs_footprint(4096, kBlock) == kBlock);
	CHECK(BlobStore::littlefs_footprint(4097, kBlock) == 2 * kBlock);
	// Block 1 holds 4092 bytes (one pointer), block 2 4088 (two), block 3 4092, block 4 4084.
	CHECK(BlobStore::littlefs_footprint(8188, kBlock) == 2 * kBlock);
	CHECK(BlobStore::littlefs_footprint(8189, kBlock) == 3 * kBlock);
	CHECK(BlobStore::littlefs_footprint(12276, kBlock) == 3 * kBlock);
	CHECK(BlobStore::littlefs_footprint(16368, kBlock) == 4 * kBlock);
	// An image chunk of 16 KB takes five blocks.
	CHECK(BlobStore::littlefs_footprint(ImageStore::kChunkSize, kBlock) == 5 * kBlock);
	// The history's segment exactly one.
	CHECK(BlobStore::littlefs_footprint(hc::History::kSegmentBytes, kBlock) == kBlock);
	// The default store counts bytes.
	hc_test::FakeBlobStore plain;
	CHECK(plain.footprint(12345) == 12345);
}

TEST_CASE("Storable (#52): the image store takes what fits beside the stored image, chunk by chunk") {
	BlockBlobStore blocks(kBoardFree);
	ImageStore store(blocks);
	// The board at boot: 38 free blocks; a manifest's block, seven chunks of five, 8188 bytes in two.
	CHECK(store.storable(kBoardFree) == 7 * ImageStore::kChunkSize + 8188);
	CHECK(store.storable(kBoardFree) == 122876);
	CHECK(store.storable(0) == 0);
	CHECK(store.storable(kBlock) == 0);
	CHECK(store.storable(2 * kBlock) == 4096);
	CHECK(store.storable(1000 * kBlock) == kMaxImageBytes);
	hc_test::FakeBlobStore plain;
	ImageStore bytes(plain);
	CHECK(bytes.storable(100000) == 100000 - ImageStore::kManifestBytes);
	CHECK(bytes.storable(SIZE_MAX / 2) == kMaxImageBytes);

	// The limit is exact against the store's own accounting, also beside a stored image.
	std::string first(20000, 'a');
	const char* error = nullptr;
	REQUIRE(store.write(first.data(), first.size(), &error));
	size_t limit = store.storable(blocks.free_bytes);
	CHECK(limit < 122876);
	std::string over(limit + 1, 'c');
	CHECK_FALSE(store.write(over.data(), over.size(), &error));
	CHECK(std::string(error) == "the storage is full");
	size_t bytes_stored = 0;
	REQUIRE(store.stored(bytes_stored));
	CHECK(bytes_stored == first.size()); // unchanged
	std::string fits(limit, 'b');
	CHECK(store.write(fits.data(), fits.size(), &error));
	REQUIRE(store.stored(bytes_stored));
	CHECK(bytes_stored == limit);
}

TEST_CASE("Storable (#52): the service's limit is the least of the store, the history to come and the staging") {
	StorableFixture f;
	ImageService::Storable s = f.service.storable();
	CHECK(s.storage == 122876);
	CHECK(s.staging == kBoardRestorable);
	CHECK(s.bytes == kBoardRestorable);

	// An image with archives: their 15 segments not yet on flash stay free for the history.
	std::string text = fixture("cases/accept-history-archives.json");
	REQUIRE(f.service.load(text.data(), text.size(), Source::api, true));
	REQUIRE(f.service.stored_ok());
	CHECK(f.service.history().flash_to_come() == 15 * kBlock);
	ImageStore probe(f.blobs);
	s = f.service.storable();
	CHECK(s.storage == probe.storable(f.blobs.free_bytes - 15 * kBlock));
	CHECK(s.storage < probe.storable(f.blobs.free_bytes));

	// Without a stored staging (or one that is off) the loads take the heap: only the store bounds.
	f.stored.capacity_bytes = 0;
	CHECK(f.service.storable().staging == kMaxImageBytes);
	CHECK(f.service.storable().bytes == s.storage);
	f.service.set_stored_staging(nullptr);
	CHECK(f.service.storable().bytes == s.storage);
	// A store that cannot tell its free space: the format's limit.
	hc_test::FakePlatform p;
	hc::Controller controller{p.platform()};
	hc::DatapointModel model{p.clock};
	hc_test::FakeUdpSocket socket{p.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), p.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), p.clock, model, tunnel};
	hc_test::FakeBlobStore unbounded; // free_bytes SIZE_MAX
	ImageService service{controller, model, unbounded, adapter, &tunnel};
	CHECK(service.storable().bytes == kMaxImageBytes);
}

TEST_CASE("Storable (#52): GET /api/status reports image.storable, the WebSocket's status does not") {
	StorableFixture f;
	auto r = f.get("/api/status", f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.json["image"]["storable"].number == static_cast<double>(kBoardRestorable));
	CHECK(r.json["image"]["loaded"].kind == hc_test::Json::Kind::boolean);
	char out[2048];
	hc::JsonWriter json(out, sizeof(out));
	json.begin_object();
	f.api.write_status(json);
	json.end_object();
	REQUIRE_FALSE(json.overflowed());
	CHECK(std::string(out, json.length()).find("storable") == std::string::npos);

	// The store bounds when it has less room than the staging: the value follows the free space.
	StorableFixture small(16 * kBlock);
	r = small.get("/api/status", small.cookie);
	CHECK(r.json["image"]["storable"].number == static_cast<double>(ImageStore(small.blobs).storable(16 * kBlock)));
	CHECK(r.json["image"]["storable"].number == 3 * ImageStore::kChunkSize); // a manifest and three chunks of five blocks
}

TEST_CASE("Storable (#52): an image larger than the store takes runs, and the answer names the limit") {
	for (bool off_loop : {true, false}) {
		CAPTURE(off_loop);
		// Eight free blocks: a manifest, one chunk of five and 8188 bytes in two.
		StorableFixture f(8 * kBlock, off_loop);
		CHECK(f.service.storable().bytes == 24572);
		auto r = f.deploy(example(8, 30000));
		CHECK(r.status == 200);
		CHECK(r.json["ok"].boolean);
		CHECK(r.json["image"]["revision"].number == 8);
		CHECK_FALSE(r.json["stored"].boolean);
		CHECK(r.json["storeError"].text == "the storage is full (it stores at most 24572 bytes)");
		CHECK(f.service.loaded());
		CHECK(f.service.info().revision == 8);
		CHECK(f.fake.sink.contains(
			"E hc.image: the image runs but is not stored (the storage is full (it stores at most 24572 bytes))"));
		size_t bytes = 0;
		CHECK_FALSE(f.service.stored(bytes));
		// One that fits is stored.
		r = f.deploy(example(9, 24572));
		CHECK(r.json["stored"].boolean);
	}
}

TEST_CASE("Storable (#52): an image the store takes but the staging cannot restore beside a failed one is stored with a warning") {
	StorableFixture f;
	auto r = f.deploy(example(8, kBoardRestorable + 1));
	CHECK(r.json["stored"].boolean);
	CHECK(f.fake.sink.contains("W hc.image: the image (61440 bytes) is stored, but larger than the 61439 bytes the "
		"device is sure to load again beside a failed image"));
	// Stored, it leaves less room for the next one: the store bounds now (19 blocks and a manifest's taken).
	ImageService::Storable s = f.service.storable();
	CHECK(s.storage == ImageStore(f.blobs).storable(f.blobs.free_bytes));
	CHECK(s.storage < kBoardRestorable);
	CHECK(s.bytes == s.storage);
	// Up to the limit: stored, no warning.
	f.fake.sink.lines.clear();
	r = f.deploy(example(9, s.bytes));
	CHECK(r.json["stored"].boolean);
	CHECK_FALSE(f.fake.sink.contains("is sure to load again"));
	// Beside that one: one byte more than the limit runs, is not stored, and the limit is named.
	size_t limit = f.service.storable().storage;
	r = f.deploy(example(10, limit + 1));
	CHECK_FALSE(r.json["stored"].boolean);
	CHECK(r.json["storeError"].text == "the storage is full (it stores at most " + std::to_string(limit) + " bytes)");
	CHECK(f.service.storable().storage == limit); // the previous image stays stored
	CHECK(f.service.info().revision == 10);
}

TEST_CASE("Storable (#52): the receive limit is the staging area; larger images are refused naming it") {
	StorableFixture f;
	CHECK(f.service.receivable() == kBoardReceivable);
	CHECK(f.api.body_limit(HttpMethod::put, "/api/image") == kBoardReceivable);
	CHECK(f.api.body_limit(HttpMethod::put, "/api/settings/tz") == hc::WebApi::kMaxBody);
	char text[128];
	f.api.body_too_long(HttpMethod::put, "/api/image", 130000, text, sizeof(text));
	CHECK(std::string(text) == "the image has 130000 bytes; the device receives at most 126975 (its staging area)");
	f.api.body_too_long(HttpMethod::post, "/api/datapoints/0/command", 5000, text, sizeof(text));
	CHECK(std::string(text) == "the request body is longer than 1024 bytes");
	// An image of the limit is received into the area (the stored slot is free then); one byte more
	// would not fit it and would need a heap block of its size.
	ImageBody body;
	CHECK(body.begin(kBoardReceivable, f.api.body_staging(HttpMethod::put, "/api/image")));
	CHECK(body.staged());
	body.release();
	const char* refusal = nullptr;
	CHECK(body.begin(kBoardReceivable + 1, f.api.body_staging(HttpMethod::put, "/api/image"), &refusal));
	CHECK_FALSE(body.staged());
	CHECK(std::string(refusal) == "the image is larger than the staging area");
	body.release();
	// The stored image's load of the limit fits its slot beside a received one of the same size.
	CHECK(f.service.storable().staging + 1 <= f.stored.shared_capacity());
	// Without a staging area (or one that is off) the heap takes it, up to the format's limit.
	f.received.capacity_bytes = 0;
	CHECK(f.api.body_limit(HttpMethod::put, "/api/image") == kMaxImageBytes);
	f.api.body_too_long(HttpMethod::put, "/api/image", kMaxImageBytes + 1, text, sizeof(text));
	CHECK(std::string(text) == "the image has 262145 bytes; the device receives at most 262144");
	f.service.set_staging(nullptr);
	CHECK(f.api.body_limit(HttpMethod::put, "/api/image") == kMaxImageBytes);
}
