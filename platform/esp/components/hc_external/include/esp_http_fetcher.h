/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_HTTP_FETCHER_H
#define HC_ESP_HTTP_FETCHER_H

#include <atomic>
#include <cstddef>

#include "hc/platform/http_fetcher.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The device's HttpFetcher (doc/external.md): esp_http_client with the IDF certificate bundle
/// (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) in a short-lived worker task per fetch (the pattern of the
/// OTA update check: the TLS handshake needs its stack only while it runs), the response in a
/// fixed buffer of kBufferBytes, the outcome posted to the event loop through the LoopQueue. A body
/// that announces or delivers more than the limit fails with the size named. At most one fetch in
/// flight. A POST (the E-Home enrollment, doc/ehome.md) sends a JSON body of at most kMaxPostBody
/// bytes and delivers the response body of any status.
class EspHttpFetcher : public HttpFetcher {
public:
	static constexpr size_t kBufferBytes = 12288;
	static constexpr int kTimeoutMs = 15000;
	/// Stack of the worker task: the TLS handshake and the HTTP client.
	static constexpr uint32_t kTaskStack = 8192;
	/// Largest POST body (the enrollment's request is about 200 bytes).
	static constexpr size_t kMaxPostBody = 384;

	explicit EspHttpFetcher(LoopQueue& queue);

	EspHttpFetcher(const EspHttpFetcher&) = delete;
	EspHttpFetcher& operator=(const EspHttpFetcher&) = delete;

	bool fetch(const char* url, size_t max_bytes, FetchReceiver& receiver, const char** error) override;
	bool post(const char* url, const char* body, size_t length, size_t max_bytes, FetchReceiver& receiver,
		const char** error) override;
	bool busy() const override { return busy_.load(); }

private:
	bool start(const char* url, size_t max_bytes, FetchReceiver& receiver, const char** error);
	void read_body(void* client);
	static void task(void* context);
	static void deliver(void* context, const LoopMessage& message);
	void run();

	LoopQueue& queue_;
	/// Set from fetch() until the result was delivered; the worker owns the fields below meanwhile.
	std::atomic<bool> busy_{false};
	FetchReceiver* receiver_ = nullptr;
	size_t max_bytes_ = 0;
	char url_[kMaxUrl + 1] = {};
	char body_[kBufferBytes + 1] = {};
	size_t length_ = 0;
	int status_ = 0;
	char error_[112] = {};
	/// The POST body; post_length_ 0 for a GET.
	char post_body_[kMaxPostBody] = {};
	size_t post_length_ = 0;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_HTTP_FETCHER_H
