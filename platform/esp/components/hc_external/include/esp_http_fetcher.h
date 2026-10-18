/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_HTTP_FETCHER_H
#define HC_ESP_HTTP_FETCHER_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hc/platform/http_fetcher.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The device's HttpFetcher (doc/external.md): esp_http_client with the IDF certificate bundle
/// (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) in a short-lived worker task per fetch (the pattern of the
/// OTA update check: the TLS handshake needs its stack only while it runs). The response goes to
/// the receiver piece by piece (issue #43): the task reads at most kChunkBytes into its buffer,
/// posts the piece to the event loop through the LoopQueue and waits until the loop ran the
/// receiver (the hand-over of hc_ehome's WebSocket client); the receiver's answer decides whether
/// it reads on. No body buffer beyond the one piece. At most one fetch in flight. A POST (the
/// E-Home enrollment, doc/ehome.md) sends a JSON body of at most kMaxPostBody bytes.
///
/// About 3 KB; the services create it on the heap for a fetch (OnDemandFetcher).
class EspHttpFetcher : public HttpFetcher {
public:
	/// One read of the body, handed to the loop as a piece.
	static constexpr size_t kChunkBytes = 2048;
	static constexpr int kTimeoutMs = 15000;
	/// Stack of the worker task: the TLS handshake and the HTTP client (the piece is not on it).
	static constexpr uint32_t kTaskStack = 8192;
	/// Largest POST body (the enrollment's request is about 200 bytes).
	static constexpr size_t kMaxPostBody = 384;

	explicit EspHttpFetcher(LoopQueue& queue);
	~EspHttpFetcher();

	EspHttpFetcher(const EspHttpFetcher&) = delete;
	EspHttpFetcher& operator=(const EspHttpFetcher&) = delete;

	bool fetch(const char* url, FetchReceiver& receiver, const char** error) override;
	bool post(const char* url, const char* body, size_t length, FetchReceiver& receiver, const char** error) override;
	bool busy() const override { return busy_.load(); }

private:
	/// The message's number: what the loop hands to the receiver.
	enum Event : int32_t { kBegin = 0, kChunk = 1, kEnd = 2 };

	bool start(const char* url, FetchReceiver& receiver, const char** error);
	static void task(void* context);
	void run();
	void read_body(void* client);
	/// Task side: posts `event` and, except for the end, waits until the loop ran the receiver;
	/// returns its answer.
	bool hand_over(Event event);
	/// Loop side.
	static void on_event(void* context, const LoopMessage& message);
	void deliver_end();

	LoopQueue& queue_;
	SemaphoreHandle_t consumed_ = nullptr;
	StaticSemaphore_t consumed_storage_;
	/// Set from fetch() until the end was delivered; the worker owns the fields below meanwhile
	/// (the loop reads them while the worker waits for it).
	std::atomic<bool> busy_{false};
	FetchReceiver* receiver_ = nullptr;
	char url_[kMaxUrl + 1] = {};
	/// The POST body; post_length_ 0 for a GET.
	char post_body_[kMaxPostBody] = {};
	size_t post_length_ = 0;
	// The response, piece by piece.
	int status_ = 0;
	size_t announced_ = FetchReceiver::kUnknownLength;
	char chunk_[kChunkBytes] = {};
	size_t chunk_length_ = 0;
	size_t delivered_ = 0;
	size_t pieces_ = 0;
	bool begun_ = false;
	bool aborted_ = false;
	bool complete_ = false;
	/// The receiver's answer to the last piece (written by the loop before it gives `consumed_`).
	bool answer_ = false;
	char error_[112] = {};
	uint32_t stack_unused_ = 0;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_HTTP_FETCHER_H
