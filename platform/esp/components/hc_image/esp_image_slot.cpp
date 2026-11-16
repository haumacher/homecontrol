/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_image_slot.h"

#include <cstdio>

#include "esp_err.h"
#include "hc/log.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.slot";

/// The text of the last flash error (one writer at a time, ImageSlot's contract).
char g_error[96];

const char* flash_error(const char* what, size_t offset, esp_err_t err) {
	std::snprintf(g_error, sizeof(g_error), "%s at 0x%x of the image partition failed: %s", what,
		static_cast<unsigned>(offset), esp_err_to_name(err));
	return g_error;
}

} // namespace

void EspImageSlot::start() {
	partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kSubtype, kLabel);
	if (partition_ == nullptr) {
		Log::error(kTag, "no partition '%s' (data, subtype 0x%x): the device takes no runtime image; flash the "
			"partition table of this firmware over USB (doc/hardware.md)", kLabel, static_cast<unsigned>(kSubtype));
		return;
	}
	if (partition_->address % 0x10000 != 0 || partition_->size % kSector != 0) {
		Log::error(kTag, "the partition '%s' at 0x%x of %u B is not aligned to 64 KB pages: no runtime image",
			kLabel, static_cast<unsigned>(partition_->address), static_cast<unsigned>(partition_->size));
		partition_ = nullptr;
		return;
	}
	Log::info(kTag, "image slot: partition '%s' at 0x%x, %u KB", kLabel, static_cast<unsigned>(partition_->address),
		static_cast<unsigned>(partition_->size / 1024));
}

const uint8_t* EspImageSlot::map() {
	if (mapped_ != nullptr) {
		return mapped_;
	}
	if (partition_ == nullptr) {
		return nullptr;
	}
	const void* base = nullptr;
	esp_err_t err = esp_partition_mmap(partition_, 0, partition_->size, ESP_PARTITION_MMAP_DATA, &base, &handle_);
	if (err != ESP_OK) {
		Log::error(kTag, "mapping the image partition (%u KB) failed: %s", static_cast<unsigned>(partition_->size / 1024),
			esp_err_to_name(err));
		return nullptr;
	}
	mapped_ = static_cast<const uint8_t*>(base);
	Log::debug(kTag, "image slot mapped at %p", base);
	return mapped_;
}

void EspImageSlot::unmap() {
	if (mapped_ != nullptr) {
		esp_partition_munmap(handle_);
		handle_ = 0;
		mapped_ = nullptr;
	}
}

void EspImageSlot::begin_write() {
	unmap();
	erased_ = 0;
}

bool EspImageSlot::erase_to(size_t end, const char** error) {
	while (erased_ < end) {
		esp_err_t err = esp_partition_erase_range(partition_, erased_, kSector);
		if (err != ESP_OK) {
			*error = flash_error("erasing the sector", erased_, err);
			Log::error(kTag, "%s", *error);
			return false;
		}
		erased_ += kSector;
	}
	return true;
}

bool EspImageSlot::write(size_t offset, const void* data, size_t length, const char** error) {
	if (partition_ == nullptr) {
		*error = "the device has no image partition";
		return false;
	}
	if (offset > partition_->size || length > partition_->size - offset) {
		*error = "the image is larger than the image partition";
		return false;
	}
	if (length == 0) {
		return true;
	}
	if (!erase_to(offset + length, error)) {
		return false;
	}
	esp_err_t err = esp_partition_write(partition_, offset, data, length);
	if (err != ESP_OK) {
		*error = flash_error("writing", offset, err);
		Log::error(kTag, "%s", *error);
		return false;
	}
	return true;
}

bool EspImageSlot::erase(size_t length, const char** error) {
	unmap();
	if (partition_ == nullptr) {
		return true; // nothing to erase: there is no slot
	}
	size_t end = (length + kSector - 1) / kSector * kSector;
	if (end > partition_->size) {
		end = partition_->size;
	}
	if (end == 0) {
		return true;
	}
	esp_err_t err = esp_partition_erase_range(partition_, 0, end);
	erased_ = 0;
	if (err != ESP_OK) {
		*error = flash_error("erasing", 0, err);
		Log::error(kTag, "%s", *error);
		return false;
	}
	Log::info(kTag, "image slot: erased %u KB", static_cast<unsigned>(end / 1024));
	return true;
}

} // namespace esp
} // namespace hc
