/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/image/image_body.h"

#include <cstring>
#include <new>

namespace hc {
namespace image {

bool ImageBody::begin(size_t size, ImageStaging* staging, const char** staging_refusal) {
	release();
	if (staging != nullptr) {
		const char* reason = nullptr;
		// One byte more for the terminating NUL, which finish() appends.
		if (staging->begin(size + 1, &reason)) {
			staging_ = staging;
		} else if (staging_refusal != nullptr) {
			*staging_refusal = reason != nullptr ? reason : "the staging area declined";
		}
	}
	if (staging_ == nullptr) {
		heap_ = new (std::nothrow) char[size + 1];
		if (heap_ == nullptr) {
			return false;
		}
	}
	size_ = size;
	have_ = 0;
	active_ = true;
	return true;
}

bool ImageBody::append(const void* data, size_t length) {
	if (!active_ || length > size_ - have_) {
		return false;
	}
	if (staging_ != nullptr) {
		if (!staging_->append(data, length)) {
			return false;
		}
	} else if (length > 0) {
		std::memcpy(heap_ + have_, data, length);
	}
	have_ += length;
	return true;
}

const char* ImageBody::finish() {
	if (!active_ || have_ != size_) {
		return nullptr;
	}
	if (staging_ == nullptr) {
		heap_[size_] = '\0';
		return heap_;
	}
	const char nul = '\0';
	if (!staging_->append(&nul, 1)) {
		return nullptr;
	}
	return staging_->finish();
}

void ImageBody::release() {
	if (staging_ != nullptr) {
		staging_->end();
		staging_ = nullptr;
	}
	delete[] heap_;
	heap_ = nullptr;
	size_ = 0;
	have_ = 0;
	active_ = false;
}

} // namespace image
} // namespace hc
