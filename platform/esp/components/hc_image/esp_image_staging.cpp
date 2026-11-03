/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_image_staging.h"

#include "esp_image_format.h"
#include "esp_mmu_map.h"
#include "esp_timer.h"
#include "hc/log.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.staging";

} // namespace

void EspImageStaging::start() {
	const esp_partition_t* updater =
		esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
	if (updater == nullptr) {
		off_reason_ = "the device has no updater partition";
		Log::info(kTag, "image staging off: %s; images are received into the heap", off_reason_);
		return;
	}
	int64_t begin = esp_timer_get_time();
	esp_partition_pos_t position = {updater->address, updater->size};
	esp_image_metadata_t metadata = {};
	esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata);
	unsigned took_ms = static_cast<unsigned>((esp_timer_get_time() - begin) / 1000);
	if (err != ESP_OK || metadata.image_len == 0 || metadata.image_len > updater->size) {
		off_reason_ = "the updater's image does not verify";
		Log::warn(kTag, "image staging off: %s (%s, %u ms); images are received into the heap", off_reason_,
			esp_err_to_name(err), took_ms);
		return;
	}
	size_t end = (metadata.image_len + kSectorBytes - 1) / kSectorBytes * kSectorBytes;
	size_t start = end + kGuardBytes;
	if (start >= updater->size) {
		off_reason_ = "the updater fills its partition";
		Log::warn(kTag, "image staging off: %s (image %u of %u B); images are received into the heap", off_reason_,
			static_cast<unsigned>(metadata.image_len), static_cast<unsigned>(updater->size));
		return;
	}
	partition_ = updater;
	offset_ = start;
	capacity_ = updater->size - start;
	off_reason_ = nullptr;
	size_t mmu_free = 0;
	esp_mmu_map_get_max_consecutive_free_block_size(static_cast<mmu_mem_caps_t>(MMU_MEM_CAP_READ | MMU_MEM_CAP_8BIT),
		MMU_TARGET_FLASH0, &mmu_free);
	Log::info(kTag, "image staging: %u KB in partition '%s' at 0x%x, behind the updater's image (%u B, verified in "
		"%u ms) and a guard sector; %u KB of the address space free for mapping",
		static_cast<unsigned>(capacity_ / 1024), updater->label, static_cast<unsigned>(updater->address + offset_),
		static_cast<unsigned>(metadata.image_len), took_ms, static_cast<unsigned>(mmu_free / 1024));
}

bool EspImageStaging::begin(size_t size, const char** reason) {
	if (off_reason_ != nullptr) {
		*reason = off_reason_;
		return false;
	}
	if (size > capacity_) {
		*reason = "the image is larger than the staging area";
		return false;
	}
	bool expected = false;
	if (!busy_.compare_exchange_strong(expected, true)) {
		*reason = "another image is being received";
		return false;
	}
	size_ = size;
	written_ = 0;
	erased_ = 0;
	failed_ = false;
	return true;
}

bool EspImageStaging::append(const void* data, size_t length) {
	if (!busy_.load() || failed_ || mapped_ || length > size_ - written_) {
		return false;
	}
	// Erase the sectors this piece reaches, no more (and never below offset_).
	while (erased_ < written_ + length) {
		esp_err_t err = esp_partition_erase_range(partition_, offset_ + erased_, kSectorBytes);
		if (err != ESP_OK) {
			Log::error(kTag, "staging: erasing the sector at 0x%x failed: %s",
				static_cast<unsigned>(partition_->address + offset_ + erased_), esp_err_to_name(err));
			failed_ = true;
			return false;
		}
		erased_ += kSectorBytes;
	}
	esp_err_t err = esp_partition_write(partition_, offset_ + written_, data, length);
	if (err != ESP_OK) {
		Log::error(kTag, "staging: writing %u bytes at 0x%x failed: %s", static_cast<unsigned>(length),
			static_cast<unsigned>(partition_->address + offset_ + written_), esp_err_to_name(err));
		failed_ = true;
		return false;
	}
	written_ += length;
	return true;
}

const char* EspImageStaging::finish() {
	if (!busy_.load() || failed_ || written_ != size_) {
		return nullptr;
	}
	if (mapped_) {
		esp_partition_munmap(map_);
		mapped_ = false;
	}
	const void* text = nullptr;
	esp_err_t err = esp_partition_mmap(partition_, offset_, size_, ESP_PARTITION_MMAP_DATA, &text, &map_);
	if (err != ESP_OK) {
		Log::error(kTag, "staging: mapping %u bytes failed: %s", static_cast<unsigned>(size_), esp_err_to_name(err));
		return nullptr;
	}
	mapped_ = true;
	return static_cast<const char*>(text);
}

void EspImageStaging::end() {
	if (mapped_) {
		esp_partition_munmap(map_);
		mapped_ = false;
	}
	size_ = 0;
	written_ = 0;
	erased_ = 0;
	failed_ = false;
	busy_.store(false);
}

} // namespace esp
} // namespace hc
