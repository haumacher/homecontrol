/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_IMAGE_SLOT_H
#define HC_PLATFORM_IMAGE_SLOT_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// The slot of the binary runtime image (spec/runtime-image.md 7, issue #87): one image at a time,
/// read in place through a mapping, written sector by sector as an image arrives. On the ESP the
/// flash partition `image` (type data, subtype 0x40), mapped through the cache; on POSIX the file
/// `image.bin` of the data directory, read into one heap block; in the tests a RAM block
/// (hc::image::MemoryImageSlot).
///
/// One writer at a time: begin_write(), write() in ascending order, then map() again. The image
/// service owns the slot; while a receive is open (ImageService::receive_begin()) the receiver may
/// call write() from its own thread, and nothing else touches the slot until the receive ends.
class ImageSlot {
public:
	/// The erase unit.
	static constexpr size_t kSector = 4096;

	/// The slot's size in bytes (0: there is none, e.g. the partition is missing; logged).
	virtual size_t size() const = 0;

	/// The slot's size() bytes, readable in place until unmap(), begin_write() or erase(); nullptr
	/// when they cannot be mapped (logged). An erased byte reads 0xFF.
	virtual const uint8_t* map() = 0;
	/// Ends the mapping (nothing may read through the pointer of map() afterwards).
	virtual void unmap() = 0;

	/// Starts writing an image from offset 0: write() erases each sector just before its first byte.
	virtual void begin_write() = 0;
	/// Writes `length` bytes at `offset` (after the bytes written before: offsets ascend), erasing
	/// the sectors it reaches first. False with `error` (a static text) when the flash refused.
	virtual bool write(size_t offset, const void* data, size_t length, const char** error) = 0;

	/// Erases the first `length` bytes (whole sectors): the slot then holds no image.
	virtual bool erase(size_t length, const char** error) = 0;

protected:
	~ImageSlot() = default;
};

} // namespace hc

#endif // HC_PLATFORM_IMAGE_SLOT_H
