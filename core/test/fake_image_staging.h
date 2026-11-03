/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// A staging area in a std::string for the tests of homecontrol #66: it records what the receivers
// did with it and can decline or fail on request.
#ifndef HC_TEST_FAKE_IMAGE_STAGING_H
#define HC_TEST_FAKE_IMAGE_STAGING_H

#include <string>

#include "hc/platform/image_staging.h"

namespace hc_test {

class FakeImageStaging : public hc::ImageStaging {
public:
	size_t capacity() const override { return capacity_bytes; }

	bool begin(size_t size, const char** reason) override {
		++begins;
		if (off != nullptr) {
			*reason = off;
			return false;
		}
		if (size > capacity_bytes) {
			*reason = "the image is larger than the staging area";
			return false;
		}
		if (busy) {
			*reason = "another image is being received";
			return false;
		}
		busy = true;
		finished = false;
		expected = size;
		staged_appends = 0;
		content.clear();
		return true;
	}

	bool append(const void* data, size_t length) override {
		++appends;
		++staged_appends;
		if (!busy || finished || content.size() + length > expected || (fail_after > 0 && staged_appends > fail_after)) {
			return false;
		}
		content.append(static_cast<const char*>(data), length);
		return true;
	}

	const char* finish() override {
		if (!busy || content.size() != expected || refuse_read) {
			return nullptr;
		}
		finished = true;
		return content.data();
	}

	void end() override {
		++ends;
		busy = false;
		finished = false;
		content.assign(content.size(), 'X'); // the text is gone: a reader after end() sees garbage
	}

	size_t capacity_bytes = 262144;
	const char* off = nullptr; ///< Set: begin() declines with this reason.
	size_t fail_after = 0;     ///< > 0: append() fails after this many calls of a staging.
	bool refuse_read = false;
	bool busy = false;
	bool finished = false;
	size_t expected = 0;
	std::string content;
	int begins = 0;
	size_t appends = 0;
	size_t staged_appends = 0;
	int ends = 0;
};

} // namespace hc_test

#endif // HC_TEST_FAKE_IMAGE_STAGING_H
