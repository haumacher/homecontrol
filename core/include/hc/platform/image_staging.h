/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_IMAGE_STAGING_H
#define HC_PLATFORM_IMAGE_STAGING_H

#include <cstddef>

namespace hc {

/// A place outside the heap where a received runtime image is put together and then read as one
/// contiguous text (homecontrol #66): a deploy then needs no heap block of the image's size, which
/// a fragmented heap may not have, and the image does not count against the heap while it is
/// parsed and applied. On the ESP it is the flash behind the updater's image, read through the
/// flash cache (doc/ota.md "The staging area"); a platform without one has none (the receivers then
/// take a heap buffer, hc::image::ImageBody).
///
/// One staging at a time: begin() .. append() .. finish() .. end(). The content is transient:
/// nothing reads it across a restart, and begin() starts over.
///
/// Threading: the E-Home connection stages on the event loop, the ESP's HTTP server in its own
/// task. begin() claims the area atomically (a second begin() is refused as busy until end()); the
/// claimant alone calls the other functions, on its thread. The claim may be handed over (the image
/// service takes a received body for its store off the loop, homecontrol #68, and ends the staging
/// on the loop when the store is done): one holder at a time.
class ImageStaging {
public:
	/// Bytes the area holds at most (0: none, e.g. it was not verified at start).
	virtual size_t capacity() const = 0;

	/// Bytes this staging holds at most while the other staging of its area holds as many (homecontrol
	/// #52): a restore reads the stored image in one while the received image that failed is still in
	/// the other (ImageService::set_stored_staging()), so an image up to this size is sure to load again
	/// beside a failed one of up to its size. The default: capacity() (an area of its own).
	virtual size_t shared_capacity() const { return capacity(); }

	/// Starts a staging of `size` bytes. False when it is in use, smaller than `size` or failed;
	/// `reason` (if given) then says why.
	virtual bool begin(size_t size, const char** reason) = 0;

	/// Appends `length` bytes (in order; at most the size given to begin() in all). False when the
	/// flash refused (logged); the staging must then be ended.
	virtual bool append(const void* data, size_t length) = 0;

	/// After the last byte: the staged bytes as one read-only text, valid until end(); nullptr when
	/// it cannot be read (logged).
	virtual const char* finish() = 0;

	/// Ends the staging (also one that failed or was never finished); the text of finish() is gone.
	virtual void end() = 0;

protected:
	~ImageStaging() = default;
};

} // namespace hc

#endif // HC_PLATFORM_IMAGE_STAGING_H
