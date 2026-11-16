/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_MEMORY_IMAGE_SLOT_H
#define HC_IMAGE_MEMORY_IMAGE_SLOT_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "hc/platform/image_slot.h"

namespace hc {
namespace image {

/// The image slot in a RAM block (issue #87): the tests' slot and the POSIX build's mapping of its
/// file. Starts erased (0xFF). write() erases each sector of kSector just before its first byte, as
/// the flash does; a test may make the n-th write fail (fail_write_after()).
class MemoryImageSlot : public ImageSlot {
public:
	/// The POSIX build's slot size (spec/runtime-image.md 7).
	static constexpr size_t kDefaultSize = 524288;

	explicit MemoryImageSlot(size_t size = kDefaultSize) : size_(size) {
		bytes_ = new (std::nothrow) uint8_t[size];
		if (bytes_ == nullptr) {
			size_ = 0;
		} else {
			std::memset(bytes_, 0xFF, size);
		}
	}
	~MemoryImageSlot() { delete[] bytes_; }
	MemoryImageSlot(const MemoryImageSlot&) = delete;
	MemoryImageSlot& operator=(const MemoryImageSlot&) = delete;

	size_t size() const override { return size_; }
	const uint8_t* map() override {
		mapped_ = true;
		++maps_;
		return bytes_;
	}
	void unmap() override { mapped_ = false; }
	void begin_write() override {
		mapped_ = false;
		erased_ = 0;
	}
	bool write(size_t offset, const void* data, size_t length, const char** error) override {
		if (fail_after_ > 0 && --fail_after_ == 0) {
			*error = "the flash refused a write (test)";
			return false;
		}
		if (offset + length > size_) {
			*error = "beyond the slot";
			return false;
		}
		size_t end = offset + length;
		while (erased_ < end) {
			size_t sector = erased_ < size_ ? (size_ - erased_ < kSector ? size_ - erased_ : kSector) : 0;
			std::memset(bytes_ + erased_, 0xFF, sector);
			erased_ += sector;
			++erased_sectors_;
		}
		std::memcpy(bytes_ + offset, data, length);
		on_write(offset, length);
		return true;
	}
	bool erase(size_t length, const char** error) override {
		(void)error;
		mapped_ = false;
		size_t end = (length + kSector - 1) / kSector * kSector;
		end = end < size_ ? end : size_;
		std::memset(bytes_, 0xFF, end);
		erased_sectors_ += end / kSector;
		on_write(0, end);
		return true;
	}

	/// The slot's bytes (tests).
	uint8_t* bytes() { return bytes_; }
	bool mapped() const { return mapped_; }
	size_t maps() const { return maps_; }
	size_t erased_sectors() const { return erased_sectors_; }
	/// The `n`-th write from now on fails (0: none).
	void fail_write_after(size_t n) { fail_after_ = n; }

protected:
	/// Told after the bytes [offset, offset + length) changed (the POSIX file follows).
	virtual void on_write(size_t offset, size_t length) {
		(void)offset;
		(void)length;
	}

private:
	uint8_t* bytes_ = nullptr;
	size_t size_ = 0;
	size_t erased_ = 0;
	size_t erased_sectors_ = 0;
	size_t maps_ = 0;
	size_t fail_after_ = 0;
	bool mapped_ = false;
};

} // namespace image
} // namespace hc

#endif // HC_IMAGE_MEMORY_IMAGE_SLOT_H
