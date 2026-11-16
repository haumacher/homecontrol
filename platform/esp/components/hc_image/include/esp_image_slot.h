/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_IMAGE_SLOT_H
#define HC_ESP_IMAGE_SLOT_H

#include <cstddef>
#include <cstdint>

#include "esp_partition.h"
#include "hc/platform/image_slot.h"

namespace hc {
namespace esp {

/// The runtime image's slot on the device (issue #87, spec/runtime-image.md 7): the flash partition
/// `image` (type data, subtype 0x40, found by its name; platform/esp/partitions-<size>.csv), mapped
/// whole through the cache (esp_partition_mmap) and written sector by sector, each 4 KB sector erased
/// just before its first byte is written. Without the partition (a device flashed with an older
/// table) size() is 0 (logged once at start()): the device then takes no image.
///
/// Threading as hc::ImageSlot: the image service on the event loop, write() also from the HTTP
/// server's task while a receive is open; never two at a time.
class EspImageSlot : public ImageSlot {
public:
	/// The partition's subtype (spec/runtime-image.md 7).
	static constexpr esp_partition_subtype_t kSubtype = static_cast<esp_partition_subtype_t>(0x40);
	static constexpr const char* kLabel = "image";

	EspImageSlot() = default;
	EspImageSlot(const EspImageSlot&) = delete;
	EspImageSlot& operator=(const EspImageSlot&) = delete;

	/// Finds the partition (logged). Call once on the event loop before the image service starts.
	void start();

	// ImageSlot
	size_t size() const override { return partition_ != nullptr ? partition_->size : 0; }
	const uint8_t* map() override;
	void unmap() override;
	void begin_write() override;
	bool write(size_t offset, const void* data, size_t length, const char** error) override;
	bool erase(size_t length, const char** error) override;

private:
	/// Erases the sectors up to `end` that were not erased since begin_write().
	bool erase_to(size_t end, const char** error);

	const esp_partition_t* partition_ = nullptr;
	esp_partition_mmap_handle_t handle_ = 0;
	const uint8_t* mapped_ = nullptr;
	/// Bytes from the slot's start erased since begin_write() (whole sectors).
	size_t erased_ = 0;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_IMAGE_SLOT_H
