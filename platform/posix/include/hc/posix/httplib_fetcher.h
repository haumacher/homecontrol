/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_HTTPLIB_FETCHER_H
#define HC_POSIX_HTTPLIB_FETCHER_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

#include "hc/platform/http_fetcher.h"
#include "hc/posix/web_server.h"

namespace hc {
namespace posix {

/// The POSIX HttpFetcher (doc/external.md): cpp-httplib's client on a worker thread; the response is
/// handed to the receiver on the event loop's thread piece by piece (httplib's response handler and
/// content receiver, each piece through the LoopExecutor the web server uses, which waits until the
/// loop ran it: the main loop already wakes for it). No body buffer. HTTPS needs a build with OpenSSL (CMake finds its
/// development files); without, https:// URLs are refused with that reason and plain http:// works
/// (the development loop can serve spec/fixtures/external with `python3 -m http.server`).
class HttplibFetcher : public HttpFetcher {
public:
	static constexpr int kConnectTimeoutS = 10;
	static constexpr int kReadTimeoutS = 15;

	explicit HttplibFetcher(LoopExecutor& executor);
	/// Stops the worker (waits for a request in flight to end; its result is not delivered).
	~HttplibFetcher();

	HttplibFetcher(const HttplibFetcher&) = delete;
	HttplibFetcher& operator=(const HttplibFetcher&) = delete;

	/// Whether this build can fetch https:// URLs.
	static bool https_supported();

	bool fetch(const char* url, FetchReceiver& receiver, const char** error) override;
	/// The E-Home enrollment's POST (doc/ehome.md).
	bool post(const char* url, const char* body, size_t length, FetchReceiver& receiver, const char** error) override;
	bool busy() const override { return busy_.load(); }

	/// Longest POST body.
	static constexpr size_t kMaxPostBody = 1024;

private:
	bool start(const char* url, FetchReceiver& receiver, const char** error);
	void worker();
	void run_request();

	LoopExecutor& executor_;
	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable wake_;
	bool pending_ = false;
	bool stop_ = false;
	std::atomic<bool> busy_{false};

	// The request, owned by the worker while busy_.
	char url_[kMaxUrl + 1] = {};
	FetchReceiver* receiver_ = nullptr;
	char error_[160] = {};
	bool post_ = false;
	char post_body_[kMaxPostBody] = {};
	size_t post_length_ = 0;
};

} // namespace posix
} // namespace hc

#endif // HC_POSIX_HTTPLIB_FETCHER_H
