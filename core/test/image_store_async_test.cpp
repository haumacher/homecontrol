/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The runtime image's store off the loop (homecontrol #68, doc/image.md "Storing"): a deploy runs at
// once, the worker writes the received body while the loop goes on, the answer follows the store
// and reports its failure; a second deploy meanwhile is refused as busy; a restart at any moment of
// the store keeps the previous stored image.

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "fake_image_staging.h"
#include "fake_worker.h"
#include "hc/image/image_body.h"
#include "hc/image/image_service.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "web_test_support.h"

using hc::HttpMethod;
using hc::image::ImageBody;
using hc::image::ImageService;
using hc::image::Report;
using hc::image::Source;

namespace {

std::string fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

std::string replace(std::string text, const std::string& from, const std::string& to) {
	size_t at = text.find(from);
	REQUIRE_MESSAGE(at != std::string::npos, "no '" << from << "'");
	text.replace(at, from.size(), to);
	return text;
}

/// The example house at `revision`, padded with whitespace to `size` bytes (several store chunks).
std::string example(unsigned revision, size_t size = 0) {
	std::string text = replace(fixture("example.json"), "\"revision\": 7", "\"revision\": " + std::to_string(revision));
	if (text.size() < size) {
		text.append(size - text.size(), ' ');
	}
	return text;
}

/// A FakeBlobStore that keeps a copy of its blobs after every write of the stored image's blobs
/// (`image.*`): each copy is what the flash holds when the power goes right after that write.
class CutBlobStore : public hc_test::FakeBlobStore {
public:
	hc::BlobStatus write(const char* name, const hc::BlobChunk* chunks, size_t count) override {
		hc::BlobStatus status = FakeBlobStore::write(name, chunks, count);
		if (std::string(name).rfind("image.", 0) == 0) {
			cuts.push_back({name, blobs});
		}
		return status;
	}
	hc::BlobStatus remove(const char* name) override {
		hc::BlobStatus status = FakeBlobStore::remove(name);
		if (std::string(name).rfind("image.", 0) == 0) {
			cuts.push_back({std::string("-") + name, blobs});
		}
		return status;
	}

	struct Cut {
		std::string after; ///< The blob written ("-name": removed) just before the cut.
		std::map<std::string, std::vector<uint8_t>> blobs;
	};
	std::vector<Cut> cuts;
};

/// The device after a restart on `stored`: the image service loads what the flash holds.
struct Restart {
	hc_test::FakePlatform p;
	hc::Controller controller{p.platform()};
	hc::DatapointModel model{p.clock};
	hc_test::FakeUdpSocket socket{p.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), p.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), p.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	ImageService service{controller, model, blobs, adapter, &tunnel};

	explicit Restart(const std::map<std::string, std::vector<uint8_t>>& stored) {
		blobs.blobs = stored;
		controller.start();
		adapter.start();
		service.start();
	}
};

/// One deploy as a receiver makes it: the body in an ImageBody (the staging area first), handed to
/// the API as the request's holder, released when handle() returned; the answer in a buffer of its own.
struct Deploy {
	std::vector<char> out = std::vector<char>(8192);
	hc::WebResponse response{out.data(), out.size()};
	ImageBody body;
	bool staged = false;
};

struct AsyncFixture : hc_test::WebFixture {
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	CutBlobStore blobs;
	ImageService service{controller, model, blobs, adapter, &tunnel};
	hc_test::ManualWorker worker;
	hc_test::FakeImageStaging staging;
	std::string cookie;

	AsyncFixture() {
		service.set_staging(&staging);
		service.set_worker(&worker);
		service.start();
		api.set_image(&service);
		cookie = onboard();
	}

	void put(Deploy& d, const std::string& image) {
		REQUIRE(d.body.begin(image.size(), api.body_staging(HttpMethod::put, "/api/image")));
		d.staged = d.body.staged();
		REQUIRE(d.body.append(image.data(), image.size()));
		const char* text = d.body.finish();
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
		req.body_holder = &d.body;
		api.handle(req, d.response);
		d.body.release(); // as the receivers do: nothing left when the service took it
	}

	/// The platform's poll of a deferred answer: false while it waits.
	bool poll(Deploy& d) { return api.poll_deferred(d.response); }

	Result answer(Deploy& d) { return result(d.response); }

	/// GET /api/image, read in pieces as a platform does ("" with the status when none is stored).
	std::string stored_text(int& status) {
		hc::WebRequest req;
		req.method = HttpMethod::get;
		req.path = "/api/image";
		req.query = "";
		req.cookie = cookie.c_str();
		req.requested_with = "HomeControl";
		req.source = "192.0.2.7";
		hc::WebResponse resp(buffer, sizeof(buffer));
		api.handle(req, resp);
		status = resp.status;
		if (!resp.stream) {
			return "";
		}
		std::string out;
		char piece[1000];
		while (out.size() < resp.stream_length) {
			size_t length = 0;
			REQUIRE(api.read_stream(resp.stream_token, out.size(), piece, sizeof(piece), length));
			REQUIRE(length > 0);
			out.append(piece, length);
		}
		return out;
	}

	std::string stored_text() {
		int status = 0;
		return stored_text(status);
	}

	/// A deploy taken through to its answer.
	Result deploy(const std::string& image) {
		Deploy d;
		put(d, image);
		if (d.response.deferred) {
			worker.step();
			REQUIRE(poll(d));
		}
		return answer(d);
	}
};

void fired(void* context) { ++*static_cast<int*>(context); }

} // namespace

TEST_CASE("Image store off the loop: the image runs at once, the loop is free, the answer follows the store") {
	AsyncFixture f;
	std::string image = example(7, 40000); // three chunks
	Deploy d;
	f.put(d, image);

	// The image runs; the answer waits; the flash is not touched yet.
	REQUIRE(d.response.deferred);
	CHECK(d.response.deferred_kind == hc::WebApi::kDeferredImageStore);
	CHECK(f.service.loaded());
	CHECK(f.service.info().revision == 7);
	CHECK(f.service.store_pending());
	CHECK_FALSE(f.service.stored_ok());
	CHECK(std::string(f.service.store_error()) == "being stored");
	CHECK(f.worker.runs == 1);
	CHECK(f.blobs.writes_of.count("image.idx") == 0);
	CHECK(f.blobs.writes_of.count("image.a.0") == 0);
	CHECK(f.fake.sink.contains("store 0 ms (started off the loop)"));
	// The service holds the body (taken from the receiver) in the staging area until the store ended.
	CHECK(d.staged);
	CHECK_FALSE(d.body.active());
	CHECK(f.staging.busy);
	CHECK(f.staging.ends == 0);
	CHECK_FALSE(f.poll(d));

	// The loop goes on meanwhile: a timer fires, the API answers, the dashboard sees the new image.
	int ticks = 0;
	f.controller.loop().schedule_at(f.fake.clock.now_ms + 10, hc::Callback{&fired, &ticks});
	f.fake.clock.advance(10);
	f.controller.loop().run_once(f.fake.clock.now_ms);
	CHECK(ticks == 1);
	auto status = f.get("/api/status", f.cookie);
	CHECK(status.json["image"]["loaded"].boolean);
	CHECK(status.json["image"]["revision"].number == 7);
	auto model = f.get("/api/model", f.cookie);
	CHECK(model.status == 200);
	int code = 0;
	f.stored_text(code);
	CHECK(code == 404); // nothing stored before

	// The worker writes; until the loop has its end, the answer still waits.
	f.worker.run_job();
	CHECK(f.blobs.writes_of["image.idx"] == 1);
	CHECK(f.blobs.writes_of["image.a.2"] == 1);
	CHECK_FALSE(f.poll(d));
	CHECK(f.service.store_pending());

	f.worker.deliver();
	CHECK_FALSE(f.service.store_pending());
	CHECK(f.service.stored_ok());
	CHECK(f.staging.ends == 1); // released after the store, not before
	CHECK_FALSE(f.staging.busy);
	REQUIRE(f.poll(d));
	CHECK_FALSE(d.response.deferred);
	auto r = f.answer(d);
	CHECK(r.status == 200);
	CHECK(r.json["ok"].boolean);
	CHECK(r.json["stored"].boolean);
	CHECK_FALSE(r.json.has("storeError"));
	CHECK(r.json["image"]["revision"].number == 7);
	CHECK(r.json["image"]["bytes"].number == static_cast<double>(image.size()));
	CHECK(r.json["source"].text == "api");
	CHECK(r.json["counts"]["datapoints"].number == 21);
	CHECK(f.fake.sink.contains("I hc.image: image 'example-house' revision 7 stored (40000 bytes in"));
	// The stored bytes are the received ones (the staging area is overwritten after its end).
	CHECK(f.stored_text() == image);
	CHECK(f.get("/api/model", f.cookie).json["stored"].boolean);

	// After a restart the stored image loads at boot.
	Restart h(f.blobs.blobs);
	CHECK(h.service.loaded());
	CHECK(h.service.info().revision == 7);
	CHECK(h.service.info().bytes == image.size());
}

TEST_CASE("Image store off the loop: a store failure is answered as such; the previous stored image stays") {
	AsyncFixture f;
	std::string a = example(7);
	REQUIRE(f.deploy(a).json["stored"].boolean);
	CHECK(f.stored_text() == a);

	std::string b = example(8, 30000);
	Deploy d;
	f.put(d, b);
	REQUIRE(d.response.deferred);
	CHECK(f.service.info().revision == 8); // runs
	f.blobs.fail_writes = true;            // the flash refuses
	f.worker.step();
	REQUIRE(f.poll(d));
	auto r = f.answer(d);
	CHECK(r.status == 200); // the image runs: the answer says it is not stored, and why
	CHECK(r.json["ok"].boolean);
	CHECK_FALSE(r.json["stored"].boolean);
	CHECK(r.json["storeError"].text == "the storage refused a write");
	CHECK(r.json["image"]["revision"].number == 8);
	CHECK_FALSE(f.service.stored_ok());
	CHECK(std::string(f.service.store_error()) == "the storage refused a write");
	CHECK(f.fake.sink.contains("E hc.image: the image runs but is not stored (the storage refused a write)"));
	CHECK_FALSE(f.staging.busy);
	f.blobs.fail_writes = false;

	// The stored image is still the previous one, complete; a restart loads it.
	CHECK(f.stored_text() == a);
	Restart h(f.blobs.blobs);
	CHECK(h.service.loaded());
	CHECK(h.service.info().revision == 7);

	// The next deploy stores again.
	CHECK(f.deploy(b).json["stored"].boolean);
	CHECK(f.stored_text() == b);
}

TEST_CASE("Image store off the loop: a deploy while the store runs is refused as busy; the pending one is untouched") {
	AsyncFixture f;
	std::string a = example(7, 20000);
	std::string b = example(8);
	Deploy first;
	f.put(first, a);
	REQUIRE(first.response.deferred);

	// The receivers ask before they take a body: refused with the reason, nothing read.
	const char* busy = f.api.body_refusal(HttpMethod::put, "/api/image");
	REQUIRE(busy != nullptr);
	CHECK(std::string(busy) == "the previous image is still being stored; deploy again in a moment");
	CHECK(f.api.body_refusal(HttpMethod::put, "/api/settings/tz") == nullptr);
	CHECK(f.api.body_refusal(HttpMethod::get, "/api/image") == nullptr);

	// One that came in anyway (the store began after the receiver asked) is refused by the API: 503
	// with Retry-After, the running image and the held staging area stay.
	Deploy second;
	f.put(second, b);
	CHECK_FALSE(second.staged); // the area is held for the first one's store: the heap took it
	CHECK_FALSE(second.response.deferred);
	auto r = f.answer(second);
	CHECK(r.status == 503);
	CHECK(r.json["code"].text == "image.busy");
	CHECK(r.retry_after == hc::WebApi::kBusyRetryS);
	CHECK(r.json["loaded"].boolean);
	CHECK(r.json["revision"].number == 7);
	CHECK(f.service.info().revision == 7);
	CHECK(f.service.report().kind == Report::Kind::busy);
	CHECK(f.fake.sink.contains("W hc.image: image from api refused: the previous one is still being stored"));
	CHECK(f.staging.busy);
	CHECK(f.worker.runs == 1);
	// The service refuses every load that would store, from any source, before looking at it.
	CHECK_FALSE(f.service.load(b.data(), b.size(), Source::console, true));
	CHECK(f.service.report().kind == Report::Kind::busy);
	// A removal waits too.
	auto removed = f.request(HttpMethod::del, "/api/image", "", f.cookie);
	CHECK(removed.status == 503);
	CHECK(removed.json["code"].text == "image.busy");
	CHECK(f.service.loaded());

	// The first store ends and is answered as stored; then the second deploy goes through.
	f.worker.step();
	REQUIRE(f.poll(first));
	auto ok = f.answer(first);
	CHECK(ok.status == 200);
	CHECK(ok.json["stored"].boolean);
	CHECK(ok.json["image"]["revision"].number == 7);
	CHECK(f.stored_text() == a);
	CHECK(f.api.body_refusal(HttpMethod::put, "/api/image") == nullptr);
	auto again = f.deploy(b);
	CHECK(again.status == 200);
	CHECK(again.json["stored"].boolean);
	CHECK(again.json["image"]["revision"].number == 8);
	CHECK(f.stored_text() == b);
}

TEST_CASE("Image store off the loop: an answer fetched after a later store began is superseded, never another's") {
	AsyncFixture f;
	Deploy first;
	f.put(first, example(7));
	f.worker.step(); // stored, but its answer is not fetched yet
	Deploy second;
	f.put(second, example(8));
	REQUIRE(second.response.deferred);
	REQUIRE(f.poll(first));
	auto r = f.answer(first);
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "image.superseded");
	f.worker.step();
	REQUIRE(f.poll(second));
	CHECK(f.answer(second).json["image"]["revision"].number == 8);
}

TEST_CASE("Image store off the loop: a restart at any moment of the store loads the previous image or the new one, whole") {
	AsyncFixture f;
	std::string a = example(7, 20000); // two chunks of generation a
	REQUIRE(f.deploy(a).json["stored"].boolean);
	std::string b = example(8, 50000); // four chunks of generation b
	f.blobs.cuts.clear();
	Deploy d;
	f.put(d, b);
	REQUIRE(d.response.deferred);
	{
		// The power goes before the worker began: the previous image.
		Restart h(f.blobs.blobs);
		CHECK(h.service.loaded());
		CHECK(h.service.info().revision == 7);
	}
	f.worker.step();
	REQUIRE(f.poll(d));
	REQUIRE(f.answer(d).json["stored"].boolean);
	REQUIRE(f.blobs.cuts.size() >= 6); // four chunks, the manifest, the old chunks' removal
	bool switched = false;
	size_t previous = 0;
	for (const CutBlobStore::Cut& cut : f.blobs.cuts) {
		INFO("cut after " << cut.after);
		if (cut.after == "image.idx") {
			switched = true;
		}
		Restart h(cut.blobs);
		REQUIRE(h.service.loaded());
		CHECK(h.service.info().revision == (switched ? 8u : 7u));
		CHECK(h.service.info().bytes == (switched ? b.size() : a.size()));
		CHECK_FALSE(h.p.sink.contains("cannot be read"));
		if (!switched) {
			++previous;
		}
	}
	CHECK(previous == 4); // after each of the new chunks: still the previous image
	CHECK(switched);
}

TEST_CASE("Image store off the loop: without a worker, a holder, or when the worker cannot start, load() stores in the call") {
	SUBCASE("the worker refuses") {
		AsyncFixture f;
		f.worker.refuse = "cannot create the worker task (memory)";
		Deploy d;
		f.put(d, example(7));
		CHECK_FALSE(d.response.deferred);
		auto r = f.answer(d);
		CHECK(r.status == 200);
		CHECK(r.json["stored"].boolean);
		CHECK(f.fake.sink.contains(
			"W hc.image: the image is stored on the loop: the worker did not start (cannot create the worker task (memory))"));
		CHECK_FALSE(f.service.store_pending());
		CHECK(f.staging.ends == 1); // released by the receiver as before
	}
	SUBCASE("no holder (a body that ends with the request)") {
		AsyncFixture f;
		std::string image = example(7);
		auto r = f.request(HttpMethod::put, "/api/image", image, f.cookie);
		CHECK(r.status == 200);
		CHECK(r.json["stored"].boolean);
		CHECK(f.worker.runs == 0);
		CHECK(f.stored_text() == image);
	}
	SUBCASE("no worker") {
		AsyncFixture f;
		f.service.set_worker(nullptr);
		Deploy d;
		f.put(d, example(7));
		CHECK_FALSE(d.response.deferred);
		CHECK(f.answer(d).json["stored"].boolean);
	}
}

TEST_CASE("ImageBody: take() moves the body and its staging; adopt() frees a malloc block") {
	hc_test::FakeImageStaging staging;
	ImageBody receiver;
	std::string text = "{\"image\": 1}";
	REQUIRE(receiver.begin(text.size(), &staging));
	REQUIRE(receiver.append(text.data(), text.size()));
	const char* read = receiver.finish();
	REQUIRE(read != nullptr);
	CHECK(receiver.text() == read);
	ImageBody keeper;
	keeper.take(receiver);
	CHECK_FALSE(receiver.active());
	CHECK(receiver.text() == nullptr);
	receiver.release();
	CHECK(staging.ends == 0); // the receiver's release frees nothing now
	CHECK(keeper.active());
	CHECK(keeper.staged());
	CHECK(keeper.text() == read);
	CHECK(std::string(keeper.text()) == text);
	keeper.release();
	CHECK(staging.ends == 1);

	char* block = static_cast<char*>(std::malloc(4));
	std::memcpy(block, "abcd", 4);
	ImageBody adopted;
	adopted.adopt(block, 4);
	CHECK(adopted.active());
	CHECK(adopted.text() == block);
	CHECK(adopted.size() == 4);
	CHECK(adopted.finish() == block);
	adopted.release(); // std::free (the allocation counter would flag a mismatched delete)
	CHECK_FALSE(adopted.active());
}

TEST_CASE("Image store off the loop (review probe): an answer nobody fetches does not block the next deploy; heap and staging bodies alternate") {
	AsyncFixture f;
	// A deploy over the heap fallback (the staging is off) whose client went away: its answer is never polled.
	f.staging.off = "probe: staging off";
	std::string a = example(11, 30000);
	{
		Deploy d;
		f.put(d, a);
		REQUIRE(d.response.deferred);
		CHECK_FALSE(d.staged);
		f.worker.step(); // the store completes; nobody polls
	}
	CHECK(f.service.stored_ok());
	CHECK(f.stored_text() == a);
	// The next deploy, over the staging area, is accepted and stored: no leftover busy state.
	f.staging.off = nullptr;
	std::string b = example(12, 30000);
	Deploy d2;
	f.put(d2, b);
	REQUIRE(d2.response.deferred);
	CHECK(d2.staged);
	f.worker.step();
	REQUIRE(f.poll(d2));
	auto r = f.answer(d2);
	CHECK(r.status == 200);
	CHECK(r.json["stored"].boolean);
	CHECK(r.json["image"]["revision"].number == 12);
	CHECK(f.stored_text() == b);
	CHECK_FALSE(f.staging.busy);
	// And a third, back on the heap, right after.
	f.staging.off = "probe: staging off";
	CHECK(f.deploy(example(13, 30000)).json["stored"].boolean);
	CHECK(f.service.info().revision == 13);
	Restart h(f.blobs.blobs);
	CHECK(h.service.info().revision == 13);
}
