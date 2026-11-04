/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_IMAGE_STAGING_H
#define HC_ESP_IMAGE_STAGING_H

#include <cstddef>
#include <cstdint>

#include "esp_partition.h"
#include "hc/platform/image_staging.h"

namespace hc {
namespace esp {

/// The image staging area of the device (homecontrol #66, doc/ota.md "The staging area"): the flash
/// of the updater partition (factory) behind the updater's image. The application never writes the
/// updater (it is flashed by USB only, doc/ota.md), so the space is free; it is not the running
/// partition, so ordinary partition writes reach it.
///
/// start() verifies the updater's image (esp_image_verify, which also gives its length) once at
/// boot, puts the area one guard sector behind its end and maps the whole area into the address
/// space once (esp_partition_mmap, a few of the MMU's 64 KB pages; the flash writes invalidate the
/// cache of the pages they reach). If any of it fails the area stays off for this boot (logged) and
/// the image service uses the heap.
///
/// Two slots share the area (homecontrol #71), each an ImageStaging of its own: received() for the
/// bodies of PUT /api/image, stored() for the stored image the service loads at boot and returns to
/// after a failed apply, which runs while the received image that failed is still staged. A slot's
/// begin() reserves the sectors it needs where the other slot's reservation leaves room (the area's
/// start, else behind the other); a slot erases only the sectors its body reaches, as it arrives,
/// and finish() gives its bytes in the area's mapping. The content is transient: nothing reads it
/// after a restart.
class EspImageStaging {
public:
	/// Kept free between the updater's image and the area.
	static constexpr size_t kGuardBytes = 4096;
	static constexpr size_t kSectorBytes = 4096;

	class Slot : public ImageStaging {
	public:
		Slot(EspImageStaging& area, const char* name) : area_(area), name_(name) {}

		Slot(const Slot&) = delete;
		Slot& operator=(const Slot&) = delete;

		// ImageStaging
		size_t capacity() const override { return area_.capacity_; }
		/// Half of the area's whole sectors (the other slot reserves as many beside it).
		size_t shared_capacity() const override { return area_.capacity_ / kSectorBytes / 2 * kSectorBytes; }
		bool begin(size_t size, const char** reason) override;
		bool append(const void* data, size_t length) override;
		const char* finish() override;
		void end() override;

	private:
		friend class EspImageStaging;

		EspImageStaging& area_;
		const char* name_;
		bool busy_ = false;   ///< Claimed (set and cleared under the area's lock).
		size_t start_ = 0;    ///< Offset of the reservation within the partition (sector aligned).
		size_t reserved_ = 0; ///< Bytes reserved from start_ (whole sectors).
		size_t size_ = 0;     ///< Of the current staging.
		size_t written_ = 0;
		size_t erased_ = 0;   ///< Bytes from start_ erased for the current staging.
		bool failed_ = false;
		bool finished_ = false;
	};

	EspImageStaging() = default;

	EspImageStaging(const EspImageStaging&) = delete;
	EspImageStaging& operator=(const EspImageStaging&) = delete;

	/// Verifies the updater, maps the area and sets it up; logs the outcome. Before the image service
	/// starts.
	void start();

	/// The slot of the received images (ImageService::set_staging()).
	ImageStaging& received() { return received_; }
	/// The slot of the stored image's loads (ImageService::set_stored_staging()).
	ImageStaging& stored() { return stored_; }

	/// Bytes of the area (0 when it is off).
	size_t capacity() const { return capacity_; }
	/// Why the area is off (nullptr when it is on).
	const char* off_reason() const { return off_reason_; }

private:
	/// Reserves the sectors of `size` bytes for `slot` beside the other slot's reservation.
	bool claim(Slot& slot, size_t size, const char** reason);
	void release(Slot& slot);

	const esp_partition_t* partition_ = nullptr;
	size_t offset_ = 0;   ///< Start of the area within the partition (sector aligned).
	size_t capacity_ = 0; ///< Bytes from offset_ to the partition's end.
	const char* off_reason_ = "not started";
	const char* base_ = nullptr; ///< The area's mapping (offset_ at base_).
	esp_partition_mmap_handle_t map_ = 0;
	Slot received_{*this, "received"};
	Slot stored_{*this, "stored"};
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_IMAGE_STAGING_H
