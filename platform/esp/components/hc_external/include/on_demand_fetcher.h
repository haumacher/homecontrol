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

/// An HttpFetcher that creates its EspHttpFetcher (about 3 KB with its 2 KB piece buffer) on the
/// heap for a fetch and frees it again once nothing is in flight: the language packs sync once per
/// boot and on a language change, the E-Home enrollment happens once (doc/ehome.md), the external
/// sources fetch every few minutes at most (issue #44), so none of them keeps a fetcher in RAM. The
/// release runs from the event loop a moment after the end was delivered (the fetcher is busy until
/// its receiver returned). The response's pieces pass through to the receiver.
class OnDemandFetcher : public HttpFetcher, public FetchReceiver {
public:
	/// Idle time after a result before the fetcher is freed.
	static constexpr uint32_t kReleaseDelayMs = 2000;

	OnDemandFetcher(EventLoop& loop, const Clock& clock, LoopQueue& queue);
	~OnDemandFetcher();

	OnDemandFetcher(const OnDemandFetcher&) = delete;
	OnDemandFetcher& operator=(const OnDemandFetcher&) = delete;

	bool fetch(const char* url, FetchReceiver& receiver, const char** error) override;
	bool post(const char* url, const char* body, size_t length, FetchReceiver& receiver, const char** error) override;
	bool busy() const override { return impl_ != nullptr && impl_->busy(); }

	/// Whether the fetcher is allocated (for `lang status`, `external status`).
	bool allocated() const { return impl_ != nullptr; }

	// FetchReceiver
	bool fetch_begin(int http_status, size_t content_length) override;
	bool fetch_chunk(const char* data, size_t size) override;
	void fetch_end(const FetchResult& result) override;

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
