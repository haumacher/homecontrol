/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_FETCH_QUEUE_H
#define HC_FETCH_QUEUE_H

#include <cstddef>
#include <cstdint>

#include "hc/event_loop.h"
#include "hc/platform/clock.h"
#include "hc/platform/http_fetcher.h"

namespace hc {

/// One fetch worker for every HTTP(S) request of the controller (issue #45 item 13, doc/external.md
/// "One fetch worker"): the OTA update check, the console's image download, the E-Home enrollment,
/// the external values and the language packs each have a Lane, an HttpFetcher of their own, and
/// the lanes' requests run one at a time on the one `worker` (the platform's fetcher: on the ESP a
/// task and a TLS session that exist only while a request runs). The peak of the fetches is one
/// worker stack and one TLS session, however many are due at once.
///
/// - A lane takes one request at a time (busy() until its fetch_end() returned), as an HttpFetcher
///   does. When the worker is free and nothing waits, the request starts at once (a refusal of the
///   worker is the lane's refusal); otherwise it waits, with a copy of its URL and body on the heap
///   (a request is no steady-state path), and fetch() returns true.
/// - When the worker is free, the waiting request of the lane with the lowest `priority` starts, in
///   the order of their arrival among equals. The device's order: the image download and the update
///   check first, then the enrollment, the external values, the language packs.
/// - Each request keeps its own timeout on the worker (Lane's `timeout_ms`, HttpFetcher::request()),
///   and waits at most `wait_ms` for the worker: then it ends without having run, its error saying so.
/// - Cancellation: a receiver that returns false aborts its request as with any fetcher; cancel()
///   drops a waiting request or aborts a running one at its next piece. Either way exactly one
///   fetch_end() follows, never from within fetch(), post() or cancel().
///
/// Threading: the event loop's thread, as every HttpFetcher.
class FetchQueue : private FetchReceiver {
public:
	static constexpr size_t kMaxLanes = 8;
	/// How long a request waits for the worker by default (the others' timeouts bound the wait anyway).
	static constexpr uint32_t kDefaultWaitMs = 120000;

	class Lane : public HttpFetcher {
	public:
		/// `name` (static text) for the log; `priority`: lower runs first; `timeout_ms`: the request's
		/// timeout on the worker (0: the platform's); `wait_ms`: the longest wait for the worker.
		Lane(FetchQueue& queue, const char* name, uint8_t priority, uint32_t timeout_ms = 0,
			uint32_t wait_ms = kDefaultWaitMs);
		~Lane();

		Lane(const Lane&) = delete;
		Lane& operator=(const Lane&) = delete;

		bool fetch(const char* url, FetchReceiver& receiver, const char** error) override;
		bool post(const char* url, const char* body, size_t length, FetchReceiver& receiver, const char** error) override;
		bool busy() const override { return state_ != State::idle; }

		/// Ends the lane's request: a waiting one is dropped (its end follows on the loop, "cancelled"),
		/// a running one is aborted at its next piece. False when there is none.
		bool cancel() override;

		const char* name() const { return name_; }
		uint8_t priority() const { return priority_; }
		bool waiting() const { return state_ == State::waiting; }
		bool running() const { return state_ == State::running; }
		/// Requests of this lane that ran on the worker, and that waited for it first.
		uint32_t runs() const { return runs_; }
		uint32_t waits() const { return waits_; }

	private:
		friend class FetchQueue;
		enum class State : uint8_t { idle, waiting, running, ending };

		bool submit(const char* url, const char* body, size_t length, FetchReceiver& receiver, const char** error);
		void free_copies();

		FetchQueue& queue_;
		const char* name_;
		uint8_t priority_;
		uint32_t timeout_ms_;
		uint32_t wait_ms_;
		State state_ = State::idle;
		FetchReceiver* receiver_ = nullptr;
		char* url_ = nullptr;  ///< The waiting request's copies (heap).
		char* body_ = nullptr;
		size_t body_length_ = 0;
		bool has_body_ = false;
		uint64_t since_ms_ = 0;
		uint32_t sequence_ = 0;
		bool cancelled_ = false;
		/// Why an ending (unrun) request ends: "cancelled" or the wait's text.
		char end_error_[80] = {};
		uint32_t runs_ = 0;
		uint32_t waits_ = 0;
	};

	FetchQueue(EventLoop& loop, const Clock& clock, HttpFetcher& worker);
	~FetchQueue();

	FetchQueue(const FetchQueue&) = delete;
	FetchQueue& operator=(const FetchQueue&) = delete;

	/// The lane whose request runs (nullptr: none), and how many wait.
	const Lane* running() const { return running_; }
	size_t waiting() const;
	/// Whether a request runs on the worker.
	bool worker_busy() const { return worker_busy_; }
	/// The most requests that waited at once since the start.
	size_t most_waiting() const { return most_waiting_; }

private:
	bool add(Lane& lane);
	void remove(Lane& lane);
	/// Starts `lane`'s request on the worker; false with `error` when the worker refuses it.
	bool start(Lane& lane, const char* url, const char* body, size_t length, bool has_body, FetchReceiver& receiver,
		const char** error);
	/// Queues `lane`'s waiting request (its copies are made).
	void wait(Lane& lane);
	/// Posts run_next() (and the ends of dropped requests).
	void kick();
	void run_next();
	void expire();
	void arm_expiry();
	void deliver_ends();

	// FetchReceiver (the worker's): forwarded to the running lane's receiver.
	bool fetch_begin(int http_status, size_t content_length) override;
	bool fetch_chunk(const char* data, size_t size) override;
	void fetch_end(const FetchResult& result) override;

	EventLoop& loop_;
	const Clock& clock_;
	HttpFetcher& worker_;
	Lane* lanes_[kMaxLanes] = {};
	Lane* running_ = nullptr;
	bool worker_busy_ = false;
	bool kicked_ = false;
	uint32_t sequence_ = 0;
	size_t most_waiting_ = 0;
	TimerHandle expiry_;
	TimerHandle retry_;
};

} // namespace hc

#endif // HC_FETCH_QUEUE_H
