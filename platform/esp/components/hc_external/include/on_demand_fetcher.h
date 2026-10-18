/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_ON_DEMAND_FETCHER_H
#define HC_ESP_ON_DEMAND_FETCHER_H

#include "esp_http_fetcher.h"
#include "hc/event_loop.h"
#include "hc/platform/clock.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// An HttpFetcher that creates its EspHttpFetcher (an 8 KB response buffer) on the heap for a fetch
/// and frees it again once nothing is in flight: the language packs sync once per boot and on a
/// language change, the E-Home enrollment happens once (doc/ehome.md), so the buffer is not worth
/// keeping in RAM. The release runs from the event loop a moment after a result was delivered (the
/// fetcher is busy until its receiver returned).
class OnDemandFetcher : public HttpFetcher, public FetchReceiver {
public:
	/// Idle time after a result before the fetcher is freed.
	static constexpr uint32_t kReleaseDelayMs = 2000;

	OnDemandFetcher(EventLoop& loop, const Clock& clock, LoopQueue& queue);
	~OnDemandFetcher();

	OnDemandFetcher(const OnDemandFetcher&) = delete;
	OnDemandFetcher& operator=(const OnDemandFetcher&) = delete;

	bool fetch(const char* url, size_t max_bytes, FetchReceiver& receiver, const char** error) override;
	bool post(const char* url, const char* body, size_t length, size_t max_bytes, FetchReceiver& receiver,
		const char** error) override;
	bool busy() const override { return impl_ != nullptr && impl_->busy(); }

	/// Whether the fetcher is allocated (for `lang status`).
	bool allocated() const { return impl_ != nullptr; }

	// FetchReceiver
	void fetch_done(const FetchResult& result) override;

private:
	bool prepare(FetchReceiver& receiver, const char** error);
	void release();

	EventLoop& loop_;
	const Clock& clock_;
	LoopQueue& queue_;
	EspHttpFetcher* impl_ = nullptr;
	FetchReceiver* receiver_ = nullptr;
	TimerHandle release_timer_;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_ON_DEMAND_FETCHER_H
