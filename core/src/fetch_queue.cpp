/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/fetch_queue.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "hc/log.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.fetch";
/// When the worker still reports busy after its end (it is freed right after the end returns).
constexpr uint32_t kRetryMs = 20;

char* copy_of(const char* data, size_t length, bool terminate) {
	char* copy = new (std::nothrow) char[length + (terminate ? 1 : 0) + (length == 0 ? 1 : 0)];
	if (copy != nullptr) {
		if (length > 0) {
			std::memcpy(copy, data, length);
		}
		if (terminate) {
			copy[length] = '\0';
		}
	}
	return copy;
}

} // namespace

// ---- Lane -------------------------------------------------------------------------------------

FetchQueue::Lane::Lane(FetchQueue& queue, const char* name, uint8_t priority, uint32_t timeout_ms, uint32_t wait_ms)
	: queue_(queue), name_(name), priority_(priority), timeout_ms_(timeout_ms), wait_ms_(wait_ms) {
	if (!queue_.add(*this)) {
		Log::error(kTag, "fetch lane '%s' not registered: more than %u lanes", name, static_cast<unsigned>(kMaxLanes));
	}
}

FetchQueue::Lane::~Lane() {
	if (queue_.running_ == this) {
		queue_.running_ = nullptr; // the worker's remaining calls are answered with an abort
	}
	free_copies();
	queue_.remove(*this);
}

void FetchQueue::Lane::free_copies() {
	delete[] url_;
	delete[] body_;
	url_ = nullptr;
	body_ = nullptr;
	body_length_ = 0;
	has_body_ = false;
}

bool FetchQueue::Lane::fetch(const char* url, FetchReceiver& receiver, const char** error) {
	return submit(url, nullptr, 0, receiver, error);
}

bool FetchQueue::Lane::post(const char* url, const char* body, size_t length, FetchReceiver& receiver,
	const char** error) {
	if (body == nullptr) {
		*error = "a POST needs a body";
		return false;
	}
	return submit(url, body, length, receiver, error);
}

bool FetchQueue::Lane::submit(const char* url, const char* body, size_t length, FetchReceiver& receiver,
	const char** error) {
	if (state_ != State::idle) {
		*error = "busy: a fetch is in flight";
		return false;
	}
	size_t url_length = std::strlen(url);
	if (url_length > kMaxUrl) {
		*error = "URL longer than 383 bytes";
		return false;
	}
	if (std::strncmp(url, "https://", 8) != 0 && std::strncmp(url, "http://", 7) != 0) {
		*error = "not an http:// or https:// URL";
		return false;
	}
	cancelled_ = false;
	bool has_body = body != nullptr;
	if (!queue_.worker_busy_ && !queue_.worker_.busy() && queue_.waiting() == 0) {
		queue_.asking_ = name_; // the gate's pause line names it
		bool admitted = queue_.gate_admits(url);
		queue_.asking_ = nullptr;
		if (admitted) {
			return queue_.start(*this, url, body, length, has_body, receiver, error);
		}
	}
	url_ = copy_of(url, url_length, true);
	body_ = has_body ? copy_of(body, length, false) : nullptr;
	if (url_ == nullptr || (has_body && body_ == nullptr)) {
		free_copies();
		*error = "no memory to queue the fetch";
		return false;
	}
	body_length_ = length;
	has_body_ = has_body;
	receiver_ = &receiver;
	state_ = State::waiting;
	since_ms_ = queue_.clock_.monotonic_ms();
	sequence_ = ++queue_.sequence_;
	++waits_;
	queue_.wait(*this);
	return true;
}

bool FetchQueue::Lane::cancel() {
	if (state_ == State::waiting) {
		free_copies();
		state_ = State::ending;
		std::snprintf(end_error_, sizeof(end_error_), "cancelled");
		Log::info(kTag, "fetch '%s' cancelled while it waited for the worker", name_);
		queue_.kick();
		return true;
	}
	if (state_ == State::running) {
		cancelled_ = true;
		Log::info(kTag, "fetch '%s' cancelled while it runs: aborted at its next piece", name_);
		return true;
	}
	return false;
}

// ---- FetchQueue -------------------------------------------------------------------------------

FetchQueue::FetchQueue(EventLoop& loop, const Clock& clock, HttpFetcher& worker)
	: loop_(loop), clock_(clock), worker_(worker) {}

FetchQueue::~FetchQueue() {
	release_gate();
	loop_.cancel(expiry_);
	loop_.cancel(retry_);
}

bool FetchQueue::add(Lane& lane) {
	for (Lane*& slot : lanes_) {
		if (slot == nullptr) {
			slot = &lane;
			return true;
		}
	}
	return false;
}

void FetchQueue::remove(Lane& lane) {
	for (Lane*& slot : lanes_) {
		if (slot == &lane) {
			slot = nullptr;
		}
	}
}

size_t FetchQueue::waiting() const {
	size_t n = 0;
	for (const Lane* lane : lanes_) {
		n += lane != nullptr && lane->state_ == Lane::State::waiting ? 1 : 0;
	}
	return n;
}

bool FetchQueue::start(Lane& lane, const char* url, const char* body, size_t length, bool has_body,
	FetchReceiver& receiver, const char** error) {
	running_ = &lane;
	worker_busy_ = true;
	lane.state_ = Lane::State::running;
	lane.receiver_ = &receiver;
	if (!worker_.request(url, has_body ? body : nullptr, length, lane.timeout_ms_, *this, error)) {
		running_ = nullptr;
		worker_busy_ = false;
		lane.state_ = Lane::State::idle;
		lane.receiver_ = nullptr;
		release_gate();
		return false;
	}
	++lane.runs_;
	return true;
}

void FetchQueue::wait(Lane& lane) {
	size_t n = waiting();
	if (n > most_waiting_) {
		most_waiting_ = n;
	}
	Log::info(kTag, "fetch '%s' waits for the worker (running: '%s'; %u waiting)", lane.name_,
		running_ != nullptr ? running_->name_ : "-", static_cast<unsigned>(n));
	arm_expiry();
	kick(); // the worker may be free already (a request that came within another's end)
}

void FetchQueue::kick() {
	if (kicked_) {
		return;
	}
	kicked_ = loop_.post(Callback::member<FetchQueue, &FetchQueue::run_next>(this, "fetch_queue.run_next"));
	if (!kicked_) {
		retry_ = loop_.schedule_at(clock_.monotonic_ms() + kRetryMs, Callback::member<FetchQueue, &FetchQueue::run_next>(this, "fetch_queue.run_next"));
		kicked_ = retry_.valid();
		if (!kicked_) {
			Log::error(kTag, "the fetch queue cannot schedule its next request: it starts with the next fetch");
		}
	}
}

void FetchQueue::deliver_ends() {
	for (Lane* lane : lanes_) {
		if (lane == nullptr || lane->state_ != Lane::State::ending) {
			continue;
		}
		FetchReceiver* receiver = lane->receiver_;
		char error[sizeof(lane->end_error_)];
		std::memcpy(error, lane->end_error_, sizeof(error));
		lane->receiver_ = nullptr;
		lane->state_ = Lane::State::idle;
		lane->cancelled_ = false;
		FetchResult result;
		result.aborted = true;
		result.error = error;
		if (receiver != nullptr) {
			receiver->fetch_end(result);
		}
	}
}

void FetchQueue::run_next() {
	kicked_ = false;
	retry_ = TimerHandle();
	deliver_ends();
	while (!worker_busy_ && running_ == nullptr) {
		Lane* next = nullptr;
		for (Lane* lane : lanes_) {
			if (lane != nullptr && lane->state_ == Lane::State::waiting &&
				(next == nullptr || lane->priority_ < next->priority_ ||
					(lane->priority_ == next->priority_ && lane->sequence_ < next->sequence_))) {
				next = lane;
			}
		}
		if (next == nullptr) {
			release_gate(); // admitted (or waiting) for a request that is gone
			break;
		}
		if (!gate_admits(next->url_)) {
			break; // tls_gate_opened() runs the queue again
		}
		if (worker_.busy()) {
			// Freed right after the end it is delivering returns: a moment later.
			retry_ = loop_.schedule_at(clock_.monotonic_ms() + kRetryMs,
				Callback::member<FetchQueue, &FetchQueue::run_next>(this, "fetch_queue.run_next"));
			kicked_ = retry_.valid();
			return;
		}
		char* url = next->url_;
		char* body = next->body_;
		size_t length = next->body_length_;
		bool has_body = next->has_body_;
		FetchReceiver* receiver = next->receiver_;
		next->url_ = nullptr;
		next->body_ = nullptr;
		next->has_body_ = false;
		uint32_t waited = static_cast<uint32_t>(clock_.monotonic_ms() - next->since_ms_);
		const char* error = nullptr;
		bool ok = start(*next, url, body, length, has_body, *receiver, &error);
		delete[] url;
		delete[] body;
		if (ok) {
			Log::info(kTag, "fetch '%s' runs after %u ms waiting", next->name_, static_cast<unsigned>(waited));
			break;
		}
		// Refused by the worker: its end now (on the loop, not within its fetch()).
		Log::warn(kTag, "fetch '%s' refused by the worker: %s", next->name_, error != nullptr ? error : "?");
		FetchResult result;
		result.error = error != nullptr ? error : "the fetch worker refused the request";
		receiver->fetch_end(result);
	}
	arm_expiry();
}

void FetchQueue::arm_expiry() {
	loop_.cancel(expiry_);
	expiry_ = TimerHandle();
	uint64_t earliest = 0;
	bool any = false;
	for (const Lane* lane : lanes_) {
		if (lane != nullptr && lane->state_ == Lane::State::waiting) {
			uint64_t due = lane->since_ms_ + lane->wait_ms_;
			if (!any || due < earliest) {
				earliest = due;
				any = true;
			}
		}
	}
	if (any) {
		expiry_ = loop_.schedule_at(earliest, Callback::member<FetchQueue, &FetchQueue::expire>(this, "fetch_queue.expire"));
	}
}

void FetchQueue::expire() {
	expiry_ = TimerHandle();
	uint64_t now = clock_.monotonic_ms();
	for (Lane* lane : lanes_) {
		if (lane == nullptr || lane->state_ != Lane::State::waiting || now - lane->since_ms_ < lane->wait_ms_) {
			continue;
		}
		lane->free_copies();
		lane->state_ = Lane::State::ending;
		if (running_ == nullptr && gate_ != nullptr && gate_->waits(*this) && gate_->memory_reason()[0] != '\0') {
			std::snprintf(lane->end_error_, sizeof(lane->end_error_), "waited %u s for memory for TLS (%s)",
				static_cast<unsigned>(lane->wait_ms_ / 1000), gate_->memory_reason());
		} else if (running_ == nullptr && gate_ != nullptr && gate_->waits(*this)) {
			std::snprintf(lane->end_error_, sizeof(lane->end_error_), "waited %u s for another TLS session to start",
				static_cast<unsigned>(lane->wait_ms_ / 1000));
		} else {
			std::snprintf(lane->end_error_, sizeof(lane->end_error_), "waited %u s for the fetch worker (busy with '%s')",
				static_cast<unsigned>(lane->wait_ms_ / 1000), running_ != nullptr ? running_->name_ : "-");
		}
		Log::warn(kTag, "fetch '%s' not run: %s", lane->name_, lane->end_error_);
	}
	deliver_ends();
	if (waiting() == 0 && running_ == nullptr) {
		release_gate();
	}
	arm_expiry();
}

// ---- The TLS gate -----------------------------------------------------------------------------

bool FetchQueue::gate_admits(const char* url) {
	if (gate_ == nullptr || url == nullptr || std::strncmp(url, "https://", 8) != 0) {
		return true;
	}
	return gate_->acquire(*this, TlsGate::Kind::session);
}

void FetchQueue::release_gate() {
	if (gate_ != nullptr) {
		gate_->release(*this);
	}
}

void FetchQueue::tls_gate_describe(char* out, size_t size) const {
	size_t n = 0;
	out[0] = '\0';
	if (asking_ != nullptr) {
		int r = std::snprintf(out, size, "%s", asking_);
		n = r > 0 ? static_cast<size_t>(r) : 0;
	}
	for (const Lane* lane : lanes_) {
		if (lane != nullptr && lane->state_ == Lane::State::waiting && n + 1 < size) {
			int r = std::snprintf(out + n, size - n, "%s%s", n > 0 ? ", " : "", lane->name_);
			n += r > 0 ? static_cast<size_t>(r) : 0;
		}
	}
	if (n == 0) {
		std::snprintf(out, size, "a fetch");
	}
}

void FetchQueue::tls_gate_opened() {
	kick(); // the waiting request starts (or, gone meanwhile, the gate goes back)
}

// ---- The worker's receiver --------------------------------------------------------------------

bool FetchQueue::fetch_begin(int http_status, size_t content_length) {
	Lane* lane = running_;
	if (lane == nullptr || lane->cancelled_ || lane->receiver_ == nullptr) {
		return false;
	}
	return lane->receiver_->fetch_begin(http_status, content_length);
}

bool FetchQueue::fetch_chunk(const char* data, size_t size) {
	Lane* lane = running_;
	if (lane == nullptr || lane->cancelled_ || lane->receiver_ == nullptr) {
		return false;
	}
	return lane->receiver_->fetch_chunk(data, size);
}

void FetchQueue::fetch_end(const FetchResult& result) {
	Lane* lane = running_;
	running_ = nullptr;
	worker_busy_ = false;
	release_gate(); // the session is over; the next request asks again (the E-Home link may go first)
	if (lane != nullptr) {
		FetchReceiver* receiver = lane->receiver_;
		lane->receiver_ = nullptr;
		lane->state_ = Lane::State::idle; // the receiver may fetch again from its end (it then waits)
		FetchResult end = result;
		if (lane->cancelled_) {
			end.ok = false;
			end.aborted = true;
			end.error = "cancelled";
		}
		lane->cancelled_ = false;
		if (receiver != nullptr) {
			receiver->fetch_end(end);
		}
	}
	if (waiting() > 0) {
		// Back in line at once (also with a request its receiver made from its end): a pause of the
		// E-Home link goes on with it, and a waiting link still goes first.
		const Lane* next = nullptr;
		for (const Lane* l : lanes_) {
			if (l != nullptr && l->state_ == Lane::State::waiting &&
				(next == nullptr || l->priority_ < next->priority_ ||
					(l->priority_ == next->priority_ && l->sequence_ < next->sequence_))) {
				next = l;
			}
		}
		if (next != nullptr) {
			gate_admits(next->url_);
		}
		kick();
	}
}

} // namespace hc
