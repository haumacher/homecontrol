/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_IMAGE_BODY_H
#define HC_IMAGE_IMAGE_BODY_H

#include <cstddef>

#include "hc/platform/image_staging.h"

namespace hc {
namespace image {

/// The body of a request as it arrives, read afterwards as one NUL-terminated text: in the
/// platform's ImageStaging when one is given and takes it (a runtime image, homecontrol #66),
/// otherwise in a heap buffer. The receivers (the E-Home connection, the device's HTTP server)
/// use it for every body; they pass the staging only for PUT /api/image.
class ImageBody {
public:
	ImageBody() = default;
	~ImageBody() { release(); }

	ImageBody(const ImageBody&) = delete;
	ImageBody& operator=(const ImageBody&) = delete;

	/// Room for `size` bytes. With `staging` it is tried first; when it declines, the reason goes
	/// to `staging_refusal` (if given) and the heap is tried. False when the heap has no block of
	/// `size` + 1 bytes either.
	bool begin(size_t size, ImageStaging* staging, const char** staging_refusal = nullptr);

	/// Appends `length` bytes; false beyond the size given to begin() or when the staging failed.
	bool append(const void* data, size_t length);

	/// When all bytes arrived: the text (NUL-terminated after `size()` bytes), valid until
	/// release(); nullptr when the staging cannot be read.
	const char* finish();

	/// Frees the buffer or ends the staging.
	void release();

	/// Takes over what `other` holds (homecontrol #68: the image service keeps a received image while
	/// it stores it off the loop): this body releases what it held, gets `other`'s buffer or staging
	/// with its state and text, and `other` is left inactive (its release() frees nothing). The
	/// staging then ends on the thread that releases this body.
	void take(ImageBody& other);

	/// Adopts a finished body of `size` bytes in a block from std::malloc() (the console's download):
	/// release() frees it with std::free(); text() is `block`. It needs no NUL after the bytes.
	void adopt(char* block, size_t size);

	/// The text finish() returned (or adopt() was given), nullptr before.
	const char* text() const { return text_; }

	bool active() const { return active_; }
	bool staged() const { return staging_ != nullptr; }
	size_t size() const { return size_; }
	size_t have() const { return have_; }

private:
	ImageStaging* staging_ = nullptr;
	char* heap_ = nullptr;
	/// heap_ came from std::malloc (adopt()), not from new[].
	bool malloced_ = false;
	const char* text_ = nullptr;
	size_t size_ = 0;
	size_t have_ = 0;
	bool active_ = false;
};

} // namespace image
} // namespace hc

#endif // HC_IMAGE_IMAGE_BODY_H
