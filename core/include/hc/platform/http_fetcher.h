/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_HTTP_FETCHER_H
#define HC_PLATFORM_HTTP_FETCHER_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// The outcome of one HttpFetcher::fetch() or post(), handed to FetchReceiver::fetch_end().
struct FetchResult {
	/// The response had HTTP status 200 and `complete`.
	bool ok = false;
	/// The HTTP status of the response; 0 when none arrived (no connection, TLS failure, timeout).
	int http_status = 0;
	/// The body arrived to its end (whatever the status) and the receiver took all of it: what it
	/// collected is the whole body.
	bool complete = false;
	/// Body bytes delivered to FetchReceiver::fetch_chunk().
	size_t length = 0;
	/// The receiver returned false from fetch_begin() or fetch_chunk().
	bool aborted = false;
	/// Why the fetch failed, in this order: the transport ("cannot connect: ESP_ERR_HTTP_CONNECT",
	/// "read error after 2048 bytes", "body ended after 100 of 300 bytes"), the status ("HTTP status
	/// 429"), the receiver's abort ("aborted by the receiver": the receiver knows its own reason);
	/// "" when `ok`. Valid during the call only.
	const char* error = "";
};

/// Receives a response piece by piece (issue #43): the body is never held as a whole by the
/// platform, so a consumer stores it (a language pack streams into a BlobWriter) or accumulates it
/// with a limit of its own (a JSON answer). Every call is made on the event loop's thread, never
/// from within HttpFetcher::fetch(); the fetcher's worker waits while the loop runs a call.
///
/// The sequence of an accepted fetch: fetch_begin() once a response arrived (after redirects),
/// fetch_chunk() for each piece of its body (of any status: a GET consumer that wants only 200
/// returns false from fetch_begin()), and always exactly one fetch_end() — also without a response
/// (then fetch_begin() was not called) and after an abort.
class FetchReceiver {
public:
	/// The response's status and headers arrived. `content_length` is the size the server announced,
	/// kUnknownLength without one (chunked transfer). False aborts: no chunk follows, the request is
	/// closed and fetch_end() reports it.
	virtual bool fetch_begin(int http_status, size_t content_length) = 0;

	/// The next `size` bytes of the body (`size` > 0, valid during the call only). False aborts as in
	/// fetch_begin().
	virtual bool fetch_chunk(const char* data, size_t size) = 0;

	/// The end of the fetch (see FetchResult).
	virtual void fetch_end(const FetchResult& result) = 0;

	/// The `content_length` of a response that announced none.
	static constexpr size_t kUnknownLength = SIZE_MAX;

protected:
	~FetchReceiver() = default;
};

/// An HTTP(S) GET (and POST) of the platform (doc/external.md): esp_http_client with the IDF
/// certificate bundle on the ESP, cpp-httplib on POSIX, a scripted fake in the tests. The request
/// runs off the event loop (a worker task or thread) and hands the response to the receiver on the
/// loop's thread piece by piece (FetchReceiver): the platform holds no body buffer beyond one read
/// (2 KB on the ESP), and every limit on a body is the receiver's.
///
/// At most one fetch is in flight: a second fetch() before the first one's fetch_end() returned is
/// refused. The platform bounds every request in time (a connect and read timeout), so the end
/// always follows an accepted fetch.
///
/// Every call is made on the event loop's thread.
class HttpFetcher {
public:
	/// Longest URL accepted, in bytes without the NUL.
	static constexpr size_t kMaxUrl = 383;

	/// Starts a GET of `url` (http:// or https://). Returns false with `error` pointing to the reason
	/// (static text) if the fetch cannot start: busy, URL refused, the scheme not supported by this
	/// platform, no memory for the worker.
	virtual bool fetch(const char* url, FetchReceiver& receiver, const char** error) = 0;

	/// Starts a POST of `body` (`length` bytes, Content-Type application/json) to `url` (http:// or
	/// https://), the E-Home enrollment (doc/ehome.md). The same rules as fetch() otherwise: one
	/// request in flight, bounded in time, the response on the loop. The default refuses (a platform
	/// without it).
	virtual bool post(const char* url, const char* body, size_t length, FetchReceiver& receiver, const char** error) {
		(void)url;
		(void)body;
		(void)length;
		(void)receiver;
		*error = "this platform's HTTP client cannot POST";
		return false;
	}

	/// A GET (`body` nullptr) or a POST of `body` like fetch() and post(), bounded by `timeout_ms` (the
	/// connect and each read; 0: the platform's own timeout): the request of a FetchQueue lane, which
	/// keeps each consumer's timeout on the one worker (issue #45 item 13). The default ignores the
	/// timeout.
	virtual bool request(const char* url, const char* body, size_t length, uint32_t timeout_ms, FetchReceiver& receiver,
		const char** error) {
		(void)timeout_ms;
		return body != nullptr ? post(url, body, length, receiver, error) : fetch(url, receiver, error);
	}

	/// Gives up the request in flight (its consumer timed out): a FetchQueue lane drops a waiting one
	/// and aborts a running one at its next piece; its fetch_end() still follows, never from within
	/// this call. False when there is none or the fetcher cannot (the default: the request runs to its
	/// end, the receiver's false aborts it).
	virtual bool cancel() { return false; }

	/// Whether a fetch is in flight (its fetch_end() not yet returned).
	virtual bool busy() const = 0;

protected:
	~HttpFetcher() = default;
};

} // namespace hc

#endif // HC_PLATFORM_HTTP_FETCHER_H
