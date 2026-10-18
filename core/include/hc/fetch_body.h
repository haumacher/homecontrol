/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_FETCH_BODY_H
#define HC_FETCH_BODY_H

#include <cstddef>

#include "hc/platform/http_fetcher.h"

namespace hc {

/// The receiver's side of a JSON answer: collects the body into a buffer of the consumer's own size
/// (manifest, external source, enrollment answer: each with its limit and the reason that names it).
/// The buffer is the consumer's; FetchBody only fills it.
class FetchBody {
public:
	/// `what` names the body in the refusal ("the manifest", "the response"); static text.
	FetchBody(char* buffer, size_t capacity, const char* what) : buffer_(buffer), capacity_(capacity), what_(what) {}

	/// For FetchReceiver::fetch_begin(): starts over; refuses (false) a body announced larger than
	/// the buffer, before a byte arrives.
	bool begin(size_t content_length);

	/// For FetchReceiver::fetch_chunk(): appends; refuses (false) a piece that does not fit.
	bool append(const char* data, size_t size);

	const char* data() const { return buffer_; }
	size_t length() const { return length_; }
	size_t capacity() const { return capacity_; }
	/// Why begin() or append() refused ("the manifest is larger than 4096 bytes"); "" otherwise.
	const char* refusal() const { return refusal_; }

private:
	char* buffer_;
	size_t capacity_;
	const char* what_;
	size_t length_ = 0;
	char refusal_[96] = {};
};

} // namespace hc

#endif // HC_FETCH_BODY_H
