/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// One fetch worker for every HTTP request (issue #45 item 13): the lanes' requests run one at a
// time, in priority order, each with its own timeout; a request waits at most its wait limit;
// cancellation drops a waiting request and aborts a running one; every request ends exactly once.

#include <string>
#include <vector>

#include "doctest.h"
#include "fake_fetch.h"
#include "hc/event_loop.h"
#include "hc/fetch_queue.h"
#include "test_support.h"

using hc::FetchQueue;
using hc::FetchReceiver;
using hc::FetchResult;

namespace {

/// The platform's fetcher, scripted: one request at a time, busy until its end returned (as the
/// ESP's and the POSIX fetchers are).
class Worker : public hc::HttpFetcher {
public:
	struct Request {
		std::string url;
		std::string body;
		bool post;
		uint32_t timeout_ms;
	};

	bool fetch(const char* url, FetchReceiver& receiver, const char** error) override {
		return request(url, nullptr, 0, 0, receiver, error);
	}
	bool post(const char* url, const char* body, size_t length, FetchReceiver& receiver, const char** error) override {
		return request(url, body, length, 0, receiver, error);
	}
	bool request(const char* url, const char* body, size_t length, uint32_t timeout_ms, FetchReceiver& receiver,
		const char** error) override {
		if (receiver_ != nullptr) {
			*error = "busy: a fetch is in flight";
			++overlaps;
			return false;
		}
		if (refuse_next) {
			refuse_next = false;
			*error = "cannot create the fetch task (memory)";
			return false;
		}
		requests.push_back({url, body != nullptr ? std::string(body, length) : std::string(), body != nullptr, timeout_ms});
		receiver_ = &receiver;
		return true;
	}
	bool busy() const override { return receiver_ != nullptr; }

	/// The running request answers (`status`, `body`); busy until the end returned.
	FetchResult answer(int status, const std::string& body) {
		REQUIRE(receiver_ != nullptr);
		FetchResult result = hc_test::respond(*receiver_, status, body);
		receiver_ = nullptr;
		return result;
	}
	/// The running request times out (no response within its timeout).
	void time_out() {
		REQUIRE(receiver_ != nullptr);
		hc_test::respond_failure(*receiver_, "cannot connect: ESP_ERR_HTTP_CONNECT");
		receiver_ = nullptr;
	}

	std::vector<Request> requests;
	int overlaps = 0;
	bool refuse_next = false;

private:
	FetchReceiver* receiver_ = nullptr;
};

/// A consumer: records what it got.
struct Consumer : FetchReceiver {
	explicit Consumer(std::vector<std::string>& order, const char* name) : order_(order), name_(name) {}
	bool fetch_begin(int status, size_t) override {
		begun = status;
		return !abort_at_begin;
	}
	bool fetch_chunk(const char* data, size_t size) override {
		body.append(data, size);
		return true;
	}
	void fetch_end(const FetchResult& result) override {
		++ends;
		ok = result.ok;
		aborted = result.aborted;
		error = result.error;
		order_.push_back(name_);
		if (again != nullptr) {
			hc::HttpFetcher* lane = again;
			again = nullptr;
			const char* refusal = nullptr;
			again_accepted = lane->fetch(again_url.c_str(), *this, &refusal);
		}
	}
	std::vector<std::string>& order_;
	const char* name_;
	int begun = 0;
	std::string body;
	int ends = 0;
	bool ok = false;
	bool aborted = false;
	std::string error;
	bool abort_at_begin = false;
	/// Fetch again from within the end (the update check's second document).
	hc::HttpFetcher* again = nullptr;
	std::string again_url;
	bool again_accepted = false;
};

struct Rig {
	hc_test::ScopedCapture capture;
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	Worker worker;
	FetchQueue queue{loop, clock, worker};
	// The device's lanes and order (platform/esp/components/hc_external, fetch_service.h).
	FetchQueue::Lane image{queue, "image", 0, 10000};
	FetchQueue::Lane update{queue, "update", 1, 10000};
	FetchQueue::Lane enrollment{queue, "enrollment", 2, 15000};
	FetchQueue::Lane external{queue, "external", 3, 15000, 30000};
	FetchQueue::Lane language{queue, "language", 4, 15000};
	std::vector<std::string> order;

	void run() {
		for (int i = 0; i < 4; ++i) {
			loop.run_once(clock.now_ms);
		}
	}
	void advance(uint64_t ms) {
		clock.advance(ms);
		run();
	}
};

} // namespace

TEST_CASE("FetchQueue: requests run one at a time on the worker, in priority order, each with its own timeout") {
	Rig f;
	Consumer lang(f.order, "language");
	Consumer ext(f.order, "external");
	Consumer upd(f.order, "update");
	Consumer img(f.order, "image");
	const char* error = nullptr;
	// The worker is free: the language pack starts at once.
	REQUIRE(f.language.fetch("https://updates.example/i18n/de.json", lang, &error));
	REQUIRE(f.worker.requests.size() == 1);
	CHECK(f.language.running());
	// Three more arrive meanwhile: they wait (accepted), nothing overlaps on the worker.
	REQUIRE(f.external.fetch("http://weather.example/now", ext, &error));
	REQUIRE(f.update.fetch("https://updates.example/manifest.json", upd, &error));
	REQUIRE(f.image.fetch("http://192.0.2.5/image.json", img, &error));
	CHECK(f.queue.waiting() == 3);
	CHECK(f.external.waiting());
	CHECK(f.external.busy());
	// A lane takes one request at a time, as a fetcher does.
	CHECK_FALSE(f.external.fetch("http://weather.example/later", ext, &error));
	CHECK(std::string(error) == "busy: a fetch is in flight");
	f.run();
	CHECK(f.worker.requests.size() == 1);
	CHECK(f.capture.sink.contains("fetch 'image' waits for the worker (running: 'language'; 3 waiting)"));

	// Each end lets the next start: the image download, the update check, then the external values.
	f.worker.answer(200, "{\"de\": 1}");
	CHECK(lang.ok);
	CHECK(lang.body == "{\"de\": 1}");
	f.advance(10);
	REQUIRE(f.worker.requests.size() == 2);
	CHECK(f.worker.requests[1].url == "http://192.0.2.5/image.json");
	CHECK(f.worker.requests[1].timeout_ms == 10000);
	CHECK(f.capture.sink.contains("fetch 'image' runs after 10 ms waiting"));
	f.worker.answer(200, "{}");
	f.run();
	REQUIRE(f.worker.requests.size() == 3);
	CHECK(f.worker.requests[2].url == "https://updates.example/manifest.json");
	f.worker.time_out(); // its own timeout ran out on the worker
	CHECK_FALSE(upd.ok);
	CHECK(upd.error == "cannot connect: ESP_ERR_HTTP_CONNECT");
	f.run();
	REQUIRE(f.worker.requests.size() == 4);
	CHECK(f.worker.requests[3].url == "http://weather.example/now");
	CHECK(f.worker.requests[3].timeout_ms == 15000);
	f.worker.answer(200, "{\"t\": 3}");
	f.run();
	CHECK(f.worker.overlaps == 0);
	CHECK(f.order == std::vector<std::string>{"language", "image", "update", "external"});
	for (Consumer* c : {&lang, &ext, &upd, &img}) {
		CHECK(c->ends == 1);
	}
	CHECK_FALSE(f.queue.worker_busy());
	CHECK(f.queue.waiting() == 0);
	CHECK(f.queue.most_waiting() == 3);
}

TEST_CASE("FetchQueue: a fetch from within an end waits its turn by priority; a POST keeps its body") {
	Rig f;
	Consumer upd(f.order, "update");
	Consumer lang(f.order, "language");
	Consumer enr(f.order, "enrollment");
	const char* error = nullptr;
	REQUIRE(f.update.fetch("https://updates.example/manifest.json", upd, &error));
	REQUIRE(f.language.fetch("https://updates.example/i18n/de.json", lang, &error));
	REQUIRE(f.enrollment.post("https://ehome.example/enroll", "{\"code\":\"1234\"}", 15, enr, &error));
	// The manifest's end fetches the signature at once: it waits, and as the update check it runs
	// before the enrollment and the language pack, which came earlier.
	upd.again = &f.update;
	upd.again_url = "https://updates.example/manifest.sig";
	f.worker.answer(200, "{}");
	CHECK(upd.again_accepted);
	f.run();
	REQUIRE(f.worker.requests.size() == 2);
	CHECK(f.worker.requests[1].url == "https://updates.example/manifest.sig");
	f.worker.answer(404, "");
	f.run();
	REQUIRE(f.worker.requests.size() == 3);
	CHECK(f.worker.requests[2].url == "https://ehome.example/enroll");
	CHECK(f.worker.requests[2].post);
	CHECK(f.worker.requests[2].body == "{\"code\":\"1234\"}");
	f.worker.answer(200, "{\"ok\":true}");
	f.run();
	REQUIRE(f.worker.requests.size() == 4);
	f.worker.answer(200, "{}");
	f.run();
	CHECK(f.order == std::vector<std::string>{"update", "update", "enrollment", "language"});
	CHECK(upd.ends == 2);
	CHECK(upd.begun == 404);
}

TEST_CASE("FetchQueue: cancellation drops a waiting request and aborts a running one; each ends once") {
	Rig f;
	Consumer lang(f.order, "language");
	Consumer ext(f.order, "external");
	Consumer img(f.order, "image");
	const char* error = nullptr;
	REQUIRE(f.language.fetch("https://updates.example/i18n/de.json", lang, &error));
	REQUIRE(f.external.fetch("http://weather.example/now", ext, &error));
	CHECK_FALSE(f.image.cancel()); // nothing to cancel
	// The waiting one: dropped, its end follows on the loop (not within cancel()).
	CHECK(f.external.cancel());
	CHECK(ext.ends == 0);
	CHECK(f.external.busy());
	f.run();
	CHECK(ext.ends == 1);
	CHECK(ext.aborted);
	CHECK(ext.error == "cancelled");
	CHECK_FALSE(f.external.busy());
	// The running one: aborted at its next piece, its end says so; the worker never saw the other.
	CHECK(f.language.cancel());
	FetchResult result = f.worker.answer(200, "{\"de\": 1}");
	CHECK(result.aborted);
	CHECK(lang.ends == 1);
	CHECK(lang.begun == 0); // the receiver was not asked
	CHECK(lang.error == "cancelled");
	CHECK(f.worker.requests.size() == 1);
	// A receiver's own abort works as with any fetcher.
	img.abort_at_begin = true;
	REQUIRE(f.image.fetch("http://192.0.2.5/image.json", img, &error));
	result = f.worker.answer(200, "{}");
	CHECK(result.aborted);
	CHECK(img.ends == 1);
	CHECK(img.body.empty());
}

TEST_CASE("FetchQueue: a request waits at most its limit; a refusal of the worker ends it, the next runs") {
	Rig f;
	Consumer lang(f.order, "language");
	Consumer ext(f.order, "external");
	Consumer img(f.order, "image");
	Consumer upd(f.order, "update");
	const char* error = nullptr;
	REQUIRE(f.language.fetch("https://updates.example/i18n/de.json", lang, &error));
	REQUIRE(f.external.fetch("http://weather.example/now", ext, &error)); // waits at most 30 s
	f.advance(29999);
	CHECK(ext.ends == 0);
	f.advance(1);
	CHECK(ext.ends == 1);
	CHECK(ext.error == "waited 30 s for the fetch worker (busy with 'language')");
	CHECK(f.capture.sink.contains("fetch 'external' not run: waited 30 s"));
	CHECK_FALSE(f.external.busy());
	// The worker refuses the next start (no memory for its task): that request ends with the reason,
	// the one after it runs.
	REQUIRE(f.image.fetch("http://192.0.2.5/image.json", img, &error));
	REQUIRE(f.update.fetch("https://updates.example/manifest.json", upd, &error));
	f.worker.refuse_next = true;
	f.worker.answer(200, "{}");
	f.run();
	CHECK(img.ends == 1);
	CHECK(img.error == "cannot create the fetch task (memory)");
	REQUIRE(f.worker.requests.size() == 2);
	CHECK(f.worker.requests[1].url == "https://updates.example/manifest.json");
	f.worker.answer(200, "{}");
	CHECK(upd.ok);
	// With the worker free and nothing waiting a refusal is the lane's refusal, at once.
	f.worker.refuse_next = true;
	CHECK_FALSE(f.image.fetch("http://192.0.2.5/image.json", img, &error));
	CHECK(std::string(error) == "cannot create the fetch task (memory)");
	CHECK_FALSE(f.image.busy());
	CHECK_FALSE(f.image.fetch("ftp://192.0.2.5/image.json", img, &error));
	CHECK(std::string(error) == "not an http:// or https:// URL");
}
