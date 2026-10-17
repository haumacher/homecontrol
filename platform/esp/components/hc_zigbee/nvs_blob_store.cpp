/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "nvs_blob_store.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "esp_log.h"
#include "nvs_flash.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.blob";

/// The NVS key of a blob name: "b" and the FNV-1a hash in 8 hex digits.
void key_of(const char* name, char key[16]) {
	uint32_t hash = 2166136261u;
	for (const char* p = name; *p != '\0'; ++p) {
		hash ^= static_cast<uint8_t>(*p);
		hash *= 16777619u;
	}
	std::snprintf(key, 16, "b%08lx", static_cast<unsigned long>(hash));
}

BlobStatus status_of(esp_err_t err) {
	switch (err) {
		case ESP_OK: return BlobStatus::ok;
		case ESP_ERR_NVS_NOT_FOUND: return BlobStatus::not_found;
		case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return BlobStatus::no_space;
		default: return BlobStatus::storage_error;
	}
}

} // namespace

bool NvsBlobStore::open(const char* label, const char* name_space) {
	std::snprintf(label_, sizeof(label_), "%s", label);
	std::snprintf(name_space_, sizeof(name_space_), "%s", name_space);
	esp_err_t err = nvs_flash_init_partition(label_);
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_LOGW(kTag, "partition %s unusable (%s): erasing it", label_, esp_err_to_name(err));
		err = nvs_flash_erase_partition(label_);
		if (err == ESP_OK) {
			err = nvs_flash_init_partition(label_);
		}
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "partition %s not available (%s): flash the partition table of this build", label_,
			esp_err_to_name(err));
		return false;
	}
	nvs_handle_t handle = 0;
	err = nvs_open_from_partition(label_, name_space_, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "cannot open %s/%s: %s", label_, name_space_, esp_err_to_name(err));
		return false;
	}
	handle_ = handle;
	return true;
}

BlobStatus NvsBlobStore::load(const char* name, uint8_t*& data, size_t& total) const {
	data = nullptr;
	total = 0;
	char key[16];
	key_of(name, key);
	size_t stored = 0;
	esp_err_t err = nvs_get_blob(handle_, key, nullptr, &stored);
	if (err != ESP_OK) {
		return status_of(err);
	}
	uint8_t* buffer = new (std::nothrow) uint8_t[stored > 0 ? stored : 1];
	if (buffer == nullptr) {
		ESP_LOGE(kTag, "out of memory reading %s (%u bytes)", name, static_cast<unsigned>(stored));
		return BlobStatus::storage_error;
	}
	err = nvs_get_blob(handle_, key, buffer, &stored);
	size_t name_length = std::strlen(name);
	if (err != ESP_OK) {
		delete[] buffer;
		return status_of(err);
	}
	if (stored <= name_length || std::memcmp(buffer, name, name_length + 1) != 0) {
		ESP_LOGE(kTag, "%s: key %s holds another blob (hash collision)", name, key);
		delete[] buffer;
		return BlobStatus::storage_error;
	}
	data = buffer;
	total = stored;
	return BlobStatus::ok;
}

BlobStatus NvsBlobStore::write(const char* name, const BlobChunk* chunks, size_t count) {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		return status;
	}
	if (handle_ == 0) {
		return BlobStatus::storage_error;
	}
	size_t content = 0;
	for (size_t i = 0; i < count; ++i) {
		content += chunks[i].size;
	}
	if (content > kMaxBlobSize) {
		return BlobStatus::too_large;
	}
	size_t name_length = std::strlen(name);
	size_t total = name_length + 1 + content;
	uint8_t* buffer = new (std::nothrow) uint8_t[total];
	if (buffer == nullptr) {
		ESP_LOGE(kTag, "out of memory writing %s (%u bytes)", name, static_cast<unsigned>(total));
		return BlobStatus::storage_error;
	}
	std::memcpy(buffer, name, name_length + 1);
	size_t offset = name_length + 1;
	for (size_t i = 0; i < count; ++i) {
		if (chunks[i].size > 0) {
			std::memcpy(buffer + offset, chunks[i].data, chunks[i].size);
			offset += chunks[i].size;
		}
	}
	char key[16];
	key_of(name, key);
	esp_err_t err = nvs_set_blob(handle_, key, buffer, total);
	delete[] buffer;
	if (err == ESP_OK) {
		err = nvs_commit(handle_);
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "writing %s to %s failed: %s", name, label_, esp_err_to_name(err));
	}
	return status_of(err);
}

BlobStatus NvsBlobStore::read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const {
	length = 0;
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		return status;
	}
	if (handle_ == 0) {
		return BlobStatus::storage_error;
	}
	uint8_t* data = nullptr;
	size_t total = 0;
	status = load(name, data, total);
	if (status != BlobStatus::ok) {
		return status;
	}
	size_t start = std::strlen(name) + 1;
	size_t content = total - start;
	if (offset < content) {
		length = content - offset < size ? content - offset : size;
		std::memcpy(buffer, data + start + offset, length);
	}
	delete[] data;
	return BlobStatus::ok;
}

BlobStatus NvsBlobStore::size(const char* name, size_t& size) const {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		return status;
	}
	if (handle_ == 0) {
		return BlobStatus::storage_error;
	}
	uint8_t* data = nullptr;
	size_t total = 0;
	status = load(name, data, total);
	if (status == BlobStatus::ok) {
		size = total - std::strlen(name) - 1;
		delete[] data;
	}
	return status;
}

BlobStatus NvsBlobStore::remove(const char* name) {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		return status;
	}
	if (handle_ == 0) {
		return BlobStatus::storage_error;
	}
	char key[16];
	key_of(name, key);
	esp_err_t err = nvs_erase_key(handle_, key);
	if (err == ESP_OK) {
		err = nvs_commit(handle_);
	}
	return status_of(err);
}

void NvsBlobStore::visit(BlobVisitor& visitor) const {
	if (handle_ == 0) {
		return;
	}
	nvs_iterator_t it = nullptr;
	esp_err_t err = nvs_entry_find(label_, name_space_, NVS_TYPE_BLOB, &it);
	while (err == ESP_OK) {
		nvs_entry_info_t info;
		nvs_entry_info(it, &info);
		size_t stored = 0;
		if (nvs_get_blob(handle_, info.key, nullptr, &stored) == ESP_OK && stored > 0) {
			uint8_t* data = new (std::nothrow) uint8_t[stored];
			if (data != nullptr && nvs_get_blob(handle_, info.key, data, &stored) == ESP_OK) {
				size_t name_length = strnlen(reinterpret_cast<const char*>(data), stored);
				if (name_length < stored && name_length <= kMaxNameLength) {
					visitor.blob(reinterpret_cast<const char*>(data), stored - name_length - 1);
				}
			}
			delete[] data;
		}
		err = nvs_entry_next(&it);
	}
	nvs_release_iterator(it);
}

} // namespace esp
} // namespace hc
