/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_image_staging.h"

#include "esp_image_format.h"
#include "esp_mmu_map.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hc/log.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.staging";

/// The slots' reservations (the HTTP server's task and the event loop claim at the same time).
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

size_t whole_sectors(size_t bytes) {
	return (bytes + EspImageStaging::kSectorBytes - 1) / EspImageStaging::kSectorBytes * EspImageStaging::kSectorBytes;
}

} // namespace

void EspImageStaging::start() {
	const esp_partition_t* updater =
		esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
	if (updater == nullptr) {
		off_reason_ = "the device has no updater partition";
		Log::info(kTag, "image staging off: %s; images are read into the heap", off_reason_);
		return;
	}
	int64_t begin = esp_timer_get_time();
	esp_partition_pos_t position = {updater->address, updater->size};
	esp_image_metadata_t metadata = {};
	esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata);
	unsigned took_ms = static_cast<unsigned>((esp_timer_get_time() - begin) / 1000);
	if (err != ESP_OK || metadata.image_len == 0 || metadata.image_len > updater->size) {
		off_reason_ = "the updater's image does not verify";
		Log::warn(kTag, "image staging off: %s (%s, %u ms); images are read into the heap", off_reason_,
			esp_err_to_name(err), took_ms);
		return;
	}
	size_t start = whole_sectors(metadata.image_len) + kGuardBytes;
	if (start >= updater->size) {
		off_reason_ = "the updater fills its partition";
		Log::warn(kTag, "image staging off: %s (image %u of %u B); images are read into the heap", off_reason_,
			static_cast<unsigned>(metadata.image_len), static_cast<unsigned>(updater->size));
		return;
	}
	size_t capacity = updater->size - start;
	// The whole area, once: the slots' texts are pointers into it, never unmapped while one is read
	// (two mappings of one MMU page share it, and unmapping the first would take the second's along).
	const void* base = nullptr;
	err = esp_partition_mmap(updater, start, capacity, ESP_PARTITION_MMAP_DATA, &base, &map_);
	if (err != ESP_OK) {
		off_reason_ = "the area cannot be mapped";
		Log::warn(kTag, "image staging off: mapping %u KB failed (%s); images are read into the heap",
			static_cast<unsigned>(capacity / 1024), esp_err_to_name(err));
		return;
	}
	partition_ = updater;
	offset_ = start;
	capacity_ = capacity;
	base_ = static_cast<const char*>(base);
	off_reason_ = nullptr;
	size_t mmu_free = 0;
	esp_mmu_map_get_max_consecutive_free_block_size(static_cast<mmu_mem_caps_t>(MMU_MEM_CAP_READ | MMU_MEM_CAP_8BIT),
		MMU_TARGET_FLASH0, &mmu_free);
	Log::info(kTag, "image staging: %u KB in partition '%s' at 0x%x, behind the updater's image (%u B, verified in "
		"%u ms) and a guard sector, mapped at %p for the received and the stored image; %u KB of the address space "
		"free for mapping",
		static_cast<unsigned>(capacity_ / 1024), updater->label, static_cast<unsigned>(updater->address + offset_),
		static_cast<unsigned>(metadata.image_len), took_ms, base, static_cast<unsigned>(mmu_free / 1024));
}

bool EspImageStaging::claim(Slot& slot, size_t size, const char** reason) {
	if (off_reason_ != nullptr) {
		*reason = off_reason_;
		return false;
	}
	if (size > capacity_) {
		*reason = "the image is larger than the staging area";
		return false;
	}
	size_t need = whole_sectors(size > 0 ? size : 1);
	Slot& other = &slot == &received_ ? stored_ : received_;
	const char* refusal = nullptr;
	taskENTER_CRITICAL(&g_lock);
	if (slot.busy_) {
		refusal = "another image is being received or stored";
	} else {
		size_t at = offset_;
		if (other.busy_ && offset_ + need > other.start_) {
			at = other.start_ + other.reserved_; // no room before the other's reservation: behind it
		}
		if (at + need > offset_ + capacity_) {
			refusal = "the staging area holds another image and has no room beside it";
		} else {
			slot.busy_ = true;
			slot.start_ = at;
			slot.reserved_ = need;
		}
	}
	taskEXIT_CRITICAL(&g_lock);
	if (refusal != nullptr) {
		*reason = refusal;
		return false;
	}
	slot.size_ = size;
	slot.written_ = 0;
	slot.erased_ = 0;
	slot.failed_ = false;
	slot.finished_ = false;
	return true;
}

void EspImageStaging::release(Slot& slot) {
	slot.size_ = 0;
	slot.written_ = 0;
	slot.erased_ = 0;
	slot.failed_ = false;
	slot.finished_ = false;
	taskENTER_CRITICAL(&g_lock);
	slot.busy_ = false;
	slot.start_ = 0;
	slot.reserved_ = 0;
	taskEXIT_CRITICAL(&g_lock);
}

bool EspImageStaging::Slot::begin(size_t size, const char** reason) {
	return area_.claim(*this, size, reason);
}

bool EspImageStaging::Slot::append(const void* data, size_t length) {
	if (!busy_ || failed_ || finished_ || length > size_ - written_) {
		return false;
	}
	// Erase the sectors this piece reaches, no more (and never outside the reservation).
	while (erased_ < written_ + length) {
		esp_err_t err = esp_partition_erase_range(area_.partition_, start_ + erased_, kSectorBytes);
		if (err != ESP_OK) {
			Log::error(kTag, "staging (%s): erasing the sector at 0x%x failed: %s", name_,
				static_cast<unsigned>(area_.partition_->address + start_ + erased_), esp_err_to_name(err));
			failed_ = true;
			return false;
		}
		erased_ += kSectorBytes;
	}
	esp_err_t err = esp_partition_write(area_.partition_, start_ + written_, data, length);
	if (err != ESP_OK) {
		Log::error(kTag, "staging (%s): writing %u bytes at 0x%x failed: %s", name_, static_cast<unsigned>(length),
			static_cast<unsigned>(area_.partition_->address + start_ + written_), esp_err_to_name(err));
		failed_ = true;
		return false;
	}
	written_ += length;
	return true;
}

const char* EspImageStaging::Slot::finish() {
	if (!busy_ || failed_ || written_ != size_) {
		return nullptr;
	}
	finished_ = true;
	// The writes invalidated the cache of the pages they reached (spi_flash): the mapping shows them.
	return area_.base_ + (start_ - area_.offset_);
}

void EspImageStaging::Slot::end() {
	area_.release(*this);
}

} // namespace esp
} // namespace hc
