/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_FETCH_SERVICE_H
#define HC_ESP_FETCH_SERVICE_H

#include <cstdint>

#include "hc/controller.h"
#include "hc/fetch_queue.h"
#include "loop_queue.h"
#include "on_demand_fetcher.h"

namespace hc {
namespace esp {

/// The one fetch worker of the device (issue #45 item 13, doc/external.md "One fetch worker"): every
/// HTTP(S) request of the controller runs on one OnDemandFetcher (its EspHttpFetcher, task and TLS
/// session exist only while a request runs), queued by the core's FetchQueue. Each consumer has a
/// lane with its priority and its timeout; the order, first to last:
///
/// | lane         | consumer                                   | timeout |
/// |---|---|---|
/// | `image`      | the console's `image load <url>` (hc_image) | 10 s    |
/// | `update`     | the OTA update check (hc_ota)               | 15 s    |
/// | `enrollment` | the E-Home enrollment (hc_ehome)            | 15 s    |
/// | `external`   | the external values (hc_external)          | 15 s    |
/// | `language`   | the language packs (hc_i18n)               | 15 s    |
///
/// So the fetches' peak is one task stack (EspHttpFetcher::kTaskStack) and one TLS session, however
/// many are due at once (before, each consumer had a task of its own: up to five at a time).
class FetchService {
public:
	static constexpr uint32_t kImageTimeoutMs = 10000;
	static constexpr uint32_t kUpdateTimeoutMs = 15000;
	static constexpr uint32_t kTimeoutMs = 15000;

	FetchService(Controller& controller, LoopQueue& queue)
		: worker_(controller.loop(), controller.clock(), queue),
		  queue_(controller.loop(), controller.clock(), worker_),
		  image_(queue_, "image", 0, kImageTimeoutMs),
		  update_(queue_, "update", 1, kUpdateTimeoutMs),
		  enrollment_(queue_, "enrollment", 2, kTimeoutMs),
		  external_(queue_, "external", 3, kTimeoutMs),
		  language_(queue_, "language", 4, kTimeoutMs) {}

	FetchService(const FetchService&) = delete;
	FetchService& operator=(const FetchService&) = delete;

	HttpFetcher& image() { return image_; }
	HttpFetcher& update() { return update_; }
	HttpFetcher& enrollment() { return enrollment_; }
	HttpFetcher& external() { return external_; }
	HttpFetcher& language() { return language_; }

	/// "free" / "allocated" / "running '<lane>', <n> waiting" for the consoles' status lines.
	const char* describe(char* buffer, size_t size) const;

private:
	OnDemandFetcher worker_;
	FetchQueue queue_;
	FetchQueue::Lane image_;
	FetchQueue::Lane update_;
	FetchQueue::Lane enrollment_;
	FetchQueue::Lane external_;
	FetchQueue::Lane language_;
};

/// The device's fetch service (created on first use, on the loop; main.cpp).
FetchService& fetch_service(Controller& controller, LoopQueue& queue);

} // namespace esp
} // namespace hc

#endif // HC_ESP_FETCH_SERVICE_H
