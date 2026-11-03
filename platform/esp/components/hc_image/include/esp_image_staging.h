/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_IMAGE_STAGING_H
#define HC_ESP_IMAGE_STAGING_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_partition.h"
#include "hc/platform/image_staging.h"

namespace hc {
namespace esp {

/// The image staging area of the device (homecontrol #66, doc/ota.md "The staging area"): the flash
/// of the updater partition (factory) behind the updater's image. The application never writes the
/// updater (it is flashed by USB only, doc/ota.md), so the space behind it is free; it is not the
/// running partition, so ordinary partition writes reach it.
///
/// start() verifies the updater's image (esp_image_verify, which also gives its length) once at
/// boot and puts the area one guard sector behind its end; if the verification fails the area stays
/// off for this boot (logged) and the receivers use the heap. A staging erases only the sectors the
/// body needs, as it arrives, and reads it back through esp_partition_mmap (one or a few of the
/// MMU's 64 KB pages). The content is transient: nothing reads it after a restart.
class EspImageStaging : public ImageStaging {
public:
	/// Kept free between the updater's image and the area.
	static constexpr size_t kGuardBytes = 4096;
	static constexpr size_t kSectorBytes = 4096;

	EspImageStaging() = default;

	EspImageStaging(const EspImageStaging&) = delete;
	EspImageStaging& operator=(const EspImageStaging&) = delete;

	/// Verifies the updater and sets the area up; logs the outcome. Before the image service starts.
	void start();

	// ImageStaging
	size_t capacity() const override { return capacity_; }
	bool begin(size_t size, const char** reason) override;
	bool append(const void* data, size_t length) override;
	const char* finish() override;
	void end() override;

	/// Why the area is off (nullptr when it is on).
	const char* off_reason() const { return off_reason_; }

private:
	const esp_partition_t* partition_ = nullptr;
	size_t offset_ = 0;   ///< Start of the area within the partition (sector aligned).
	size_t capacity_ = 0; ///< Bytes from offset_ to the partition's end.
	const char* off_reason_ = "not started";
	std::atomic<bool> busy_{false};
	size_t size_ = 0;     ///< Of the current staging.
	size_t written_ = 0;
	size_t erased_ = 0;   ///< Bytes from offset_ erased for the current staging.
	bool failed_ = false;
	esp_partition_mmap_handle_t map_ = 0;
	bool mapped_ = false;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_IMAGE_STAGING_H
