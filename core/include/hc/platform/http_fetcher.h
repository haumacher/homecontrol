/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_HTTP_FETCHER_H
#define HC_PLATFORM_HTTP_FETCHER_H

#include <cstddef>

namespace hc {

/// The outcome of one HttpFetcher::fetch().
struct FetchResult {
	/// The body arrived completely with HTTP status 200.
	bool ok = false;
	/// The HTTP status of the response; 0 when none arrived (no connection, TLS failure, timeout).
	int http_status = 0;
	/// The body (not NUL-terminated); valid during the FetchReceiver call only. Empty unless `ok`.
	const char* body = nullptr;
	size_t length = 0;
	/// Why the fetch failed ("cannot connect: ESP_ERR_HTTP_CONNECT", "HTTP status 429", "body larger
	/// than 8192 bytes"); "" when `ok`. Valid during the call only.
	const char* error = "";
};

/// Receives the outcome of a fetch. Always called on the event loop's thread, never from within
/// HttpFetcher::fetch().
class FetchReceiver {
public:
	virtual void fetch_done(const FetchResult& result) = 0;

protected:
	~FetchReceiver() = default;
};

/// An HTTP(S) GET of the platform (doc/external.md): esp_http_client with the IDF certificate
/// bundle on the ESP, cpp-httplib on POSIX, a scripted fake in the tests. The request runs off the
/// event loop (a worker task or thread); the outcome is handed to the receiver on the loop's
/// thread.
///
/// At most one fetch is in flight: a second fetch() before the first one's result was delivered is
/// refused. The platform bounds every request in time (a connect and read timeout), so a result
/// always follows an accepted fetch.
///
/// Every call is made on the event loop's thread.
class HttpFetcher {
public:
	/// Longest URL accepted, in bytes without the NUL.
	static constexpr size_t kMaxUrl = 383;

	/// Starts a GET of `url` (http:// or https://). A body longer than `max_bytes` (or than the
	/// platform's buffer, whichever is smaller) fails the fetch with the size named in the reason.
	/// Returns false with `error` pointing to the reason (static text) if the fetch cannot start:
	/// busy, URL refused, the scheme not supported by this platform, no memory for the worker.
	virtual bool fetch(const char* url, size_t max_bytes, FetchReceiver& receiver, const char** error) = 0;

	/// Whether a fetch is in flight (its result not yet delivered).
	virtual bool busy() const = 0;

protected:
	~HttpFetcher() = default;
};

} // namespace hc

#endif // HC_PLATFORM_HTTP_FETCHER_H
