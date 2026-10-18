/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/fetch_body.h"

#include <cstdio>
#include <cstring>

namespace hc {

bool FetchBody::begin(size_t content_length) {
	length_ = 0;
	refusal_[0] = '\0';
	if (content_length != FetchReceiver::kUnknownLength && content_length > capacity_) {
		std::snprintf(refusal_, sizeof(refusal_), "%s is larger than %zu bytes (%zu announced)", what_, capacity_,
			content_length);
		return false;
	}
	return true;
}

bool FetchBody::append(const char* data, size_t size) {
	if (size > capacity_ - length_) {
		std::snprintf(refusal_, sizeof(refusal_), "%s is larger than %zu bytes", what_, capacity_);
		return false;
	}
	std::memcpy(buffer_ + length_, data, size);
	length_ += size;
	return true;
}

} // namespace hc
