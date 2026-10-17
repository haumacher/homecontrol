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

/// The POSIX HttpFetcher (doc/external.md): cpp-httplib's client on a worker thread; the result is
/// handed to the receiver on the event loop's thread through the LoopExecutor the web server uses
/// (the main loop already wakes for it). HTTPS needs a build with OpenSSL (CMake finds its
/// development files); without, https:// URLs are refused with that reason and plain http:// works
/// (the development loop can serve spec/fixtures/external with `python3 -m http.server`).
class HttplibFetcher : public HttpFetcher {
public:
	/// The response buffer: the same bound as the device's.
	static constexpr size_t kBufferBytes = 8192;
	static constexpr int kConnectTimeoutS = 10;
	static constexpr int kReadTimeoutS = 15;

	explicit HttplibFetcher(LoopExecutor& executor);
	/// Stops the worker (waits for a request in flight to end; its result is not delivered).
	~HttplibFetcher();

	HttplibFetcher(const HttplibFetcher&) = delete;
	HttplibFetcher& operator=(const HttplibFetcher&) = delete;

	/// Whether this build can fetch https:// URLs.
	static bool https_supported();

	bool fetch(const char* url, size_t max_bytes, FetchReceiver& receiver, const char** error) override;
	bool busy() const override { return busy_.load(); }

private:
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
	size_t max_bytes_ = 0;
	FetchReceiver* receiver_ = nullptr;
	char body_[kBufferBytes] = {};
	char error_[160] = {};
};

} // namespace posix
} // namespace hc

#endif // HC_POSIX_HTTPLIB_FETCHER_H
