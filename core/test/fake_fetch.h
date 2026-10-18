/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Delivers a scripted response to a FetchReceiver the way the platforms do (issue #43): the begin,
// the body in pieces until the receiver aborts, the end with the result the platforms compute.
#ifndef HC_FAKE_FETCH_H
#define HC_FAKE_FETCH_H

#include <algorithm>
#include <cstddef>
#include <string>

#include "hc/platform/http_fetcher.h"

namespace hc_test {

/// How respond() hands the body over.
struct Delivery {
	/// The size of one piece.
	size_t piece = 1024;
	/// Whether the response announces its length (Content-Length); otherwise kUnknownLength.
	bool announce = true;
	/// The pieces delivered (filled in by respond()).
	size_t pieces = 0;
};

/// A response with `status` and `body`; returns the result handed to fetch_end().
inline hc::FetchResult respond(hc::FetchReceiver& receiver, int status, const std::string& body, Delivery& delivery) {
	static std::string error;
	size_t announced = delivery.announce ? body.size() : hc::FetchReceiver::kUnknownLength;
	bool go = receiver.fetch_begin(status, announced);
	size_t at = 0;
	delivery.pieces = 0;
	while (go && at < body.size()) {
		size_t n = std::min(delivery.piece, body.size() - at);
		++delivery.pieces;
		go = receiver.fetch_chunk(body.data() + at, n);
		if (go) {
			at += n;
		}
	}
	hc::FetchResult result;
	result.http_status = status;
	result.length = at;
	result.aborted = !go;
	result.complete = go;
	error = status != 200 ? "HTTP status " + std::to_string(status) : !go ? "aborted by the receiver" : "";
	result.ok = go && status == 200;
	result.error = error.c_str();
	receiver.fetch_end(result);
	return result;
}

inline hc::FetchResult respond(hc::FetchReceiver& receiver, int status, const std::string& body) {
	Delivery delivery;
	return respond(receiver, status, body, delivery);
}

/// A fetch without a response (no connection, TLS failure): only the end, with `error`.
inline void respond_failure(hc::FetchReceiver& receiver, const char* error) {
	hc::FetchResult result;
	result.error = error;
	receiver.fetch_end(result);
}

} // namespace hc_test

#endif // HC_FAKE_FETCH_H
