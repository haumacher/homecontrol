/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "on_demand_fetcher.h"

#include <new>

namespace hc {
namespace esp {

namespace {

/// EspHttpFetcher has no virtual destructor (nothing deletes it through the interface): it is
/// created and destroyed here as its own type.
EspHttpFetcher* create_fetcher(LoopQueue& queue) {
	void* memory = ::operator new(sizeof(EspHttpFetcher), std::nothrow);
	return memory != nullptr ? new (memory) EspHttpFetcher(queue) : nullptr;
}

void destroy_fetcher(EspHttpFetcher* fetcher) {
	fetcher->~EspHttpFetcher();
	::operator delete(fetcher);
}

} // namespace

// ---- OnDemandFetcher --------------------------------------------------------------------------

OnDemandFetcher::OnDemandFetcher(EventLoop& loop, const Clock& clock, LoopQueue& queue)
	: loop_(loop), clock_(clock), queue_(queue) {}

OnDemandFetcher::~OnDemandFetcher() {
	loop_.cancel(release_timer_);
	if (impl_ != nullptr && !impl_->busy()) {
		destroy_fetcher(impl_);
	}
}

bool OnDemandFetcher::prepare(FetchReceiver& receiver, const char** error) {
	if (impl_ == nullptr) {
		impl_ = create_fetcher(queue_);
		if (impl_ == nullptr) {
			*error = "no memory for the HTTP fetcher (8 KB)";
			return false;
		}
	}
	loop_.cancel(release_timer_);
	release_timer_ = TimerHandle();
	receiver_ = &receiver;
	return true;
}

bool OnDemandFetcher::fetch(const char* url, size_t max_bytes, FetchReceiver& receiver, const char** error) {
	if (!prepare(receiver, error)) {
		return false;
	}
	if (!impl_->fetch(url, max_bytes, *this, error)) {
		receiver_ = nullptr;
		return false;
	}
	return true;
}

bool OnDemandFetcher::post(const char* url, const char* body, size_t length, size_t max_bytes, FetchReceiver& receiver,
	const char** error) {
	if (!prepare(receiver, error)) {
		return false;
	}
	if (!impl_->post(url, body, length, max_bytes, *this, error)) {
		receiver_ = nullptr;
		return false;
	}
	return true;
}

void OnDemandFetcher::fetch_done(const FetchResult& result) {
	FetchReceiver* receiver = receiver_;
	receiver_ = nullptr;
	if (receiver != nullptr) {
		receiver->fetch_done(result);
	}
	// Still busy here (until this call returns): free it from the loop a little later, unless the
	// receiver fetches again meanwhile.
	loop_.cancel(release_timer_);
	release_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kReleaseDelayMs,
		Callback::member<OnDemandFetcher, &OnDemandFetcher::release>(this));
}

void OnDemandFetcher::release() {
	release_timer_ = TimerHandle();
	if (impl_ == nullptr) {
		return;
	}
	if (impl_->busy()) {
		release_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kReleaseDelayMs,
			Callback::member<OnDemandFetcher, &OnDemandFetcher::release>(this));
		return;
	}
	destroy_fetcher(impl_);
	impl_ = nullptr;
}

} // namespace esp
} // namespace hc
