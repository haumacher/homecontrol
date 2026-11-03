/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/image/image_body.h"

#include <cstdlib>
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
	if (text_ != nullptr) {
		return text_; // finished before (or adopted)
	}
	if (staging_ == nullptr) {
		heap_[size_] = '\0';
		text_ = heap_;
		return text_;
	}
	const char nul = '\0';
	if (!staging_->append(&nul, 1)) {
		return nullptr;
	}
	text_ = staging_->finish();
	return text_;
}

void ImageBody::release() {
	if (staging_ != nullptr) {
		staging_->end();
		staging_ = nullptr;
	}
	if (malloced_) {
		std::free(heap_);
	} else {
		delete[] heap_;
	}
	heap_ = nullptr;
	malloced_ = false;
	text_ = nullptr;
	size_ = 0;
	have_ = 0;
	active_ = false;
}

void ImageBody::take(ImageBody& other) {
	if (&other == this) {
		return;
	}
	release();
	staging_ = other.staging_;
	heap_ = other.heap_;
	malloced_ = other.malloced_;
	text_ = other.text_;
	size_ = other.size_;
	have_ = other.have_;
	active_ = other.active_;
	other.staging_ = nullptr;
	other.heap_ = nullptr;
	other.malloced_ = false;
	other.text_ = nullptr;
	other.size_ = 0;
	other.have_ = 0;
	other.active_ = false;
}

void ImageBody::adopt(char* block, size_t size) {
	release();
	heap_ = block;
	malloced_ = true;
	text_ = block;
	size_ = size;
	have_ = size;
	active_ = block != nullptr;
}

} // namespace image
} // namespace hc
