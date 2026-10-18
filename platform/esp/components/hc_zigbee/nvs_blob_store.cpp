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

/// The NVS keys of a blob name: a letter and the FNV-1a hash of the name in 8 hex digits.
/// 'n': the name (a string), 'd': the content (a blob); 'b': the legacy layout (the name with its
/// NUL, then the content, in one blob), still read and replaced by the next write.
void key_of(const char* name, char letter, char key[16]) {
	uint32_t hash = 2166136261u;
	for (const char* p = name; *p != '\0'; ++p) {
		hash ^= static_cast<uint8_t>(*p);
		hash *= 16777619u;
	}
	std::snprintf(key, 16, "%c%08lx", letter, static_cast<unsigned long>(hash));
}

/// The key of the same name with another letter (`key` is one of key_of()'s).
void sibling_key(const char* key, char letter, char out[16]) {
	size_t length = strnlen(key + 1, 14);
	out[0] = letter;
	std::memcpy(out + 1, key + 1, length);
	out[1 + length] = '\0';
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

BlobStatus NvsBlobStore::check_owner(const char* name, const char* data_key) const {
	char key[16];
	sibling_key(data_key, 'n', key);
	char stored[kMaxNameLength + 2] = {};
	size_t length = sizeof(stored);
	esp_err_t err = nvs_get_str(handle_, key, stored, &length);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		return BlobStatus::not_found;
	}
	if (err == ESP_ERR_NVS_INVALID_LENGTH || (err == ESP_OK && std::strcmp(stored, name) != 0)) {
		ESP_LOGE(kTag, "%s: key %s holds another blob (hash collision)", name, key);
		return BlobStatus::storage_error;
	}
	return status_of(err);
}

BlobStatus NvsBlobStore::load_legacy(const char* name, uint8_t*& data, size_t& total) const {
	data = nullptr;
	total = 0;
	char key[16];
	key_of(name, 'b', key);
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
	size_t pieces = 0;
	const void* single = nullptr;
	for (size_t i = 0; i < count; ++i) {
		content += chunks[i].size;
		if (chunks[i].size > 0) {
			++pieces;
			single = chunks[i].data;
		}
	}
	if (content > kMaxBlobSize) {
		return BlobStatus::too_large;
	}
	char legacy[16];
	key_of(name, 'b', legacy);
	char data_key[16];
	key_of(name, 'd', data_key);
	char name_key[16];
	key_of(name, 'n', name_key);
	esp_err_t err = ESP_OK;
	if (content == 0) {
		// An empty blob keeps the legacy layout (the name alone): NVS blobs are never empty.
		err = nvs_set_blob(handle_, legacy, name, std::strlen(name) + 1);
		if (err == ESP_OK) {
			esp_err_t erased = nvs_erase_key(handle_, data_key);
			if (erased != ESP_OK && erased != ESP_ERR_NVS_NOT_FOUND) {
				err = erased;
			}
		}
	} else {
		// The name in a key of its own (written once), the content in another, straight from the
		// caller's buffer: no copy for one chunk. The order keeps every moment readable: until the
		// content key is written, a legacy blob of the name is still the content.
		status = check_owner(name, data_key);
		if (status == BlobStatus::not_found) {
			err = nvs_set_str(handle_, name_key, name);
		} else if (status != BlobStatus::ok) {
			return status;
		}
		uint8_t* joined = nullptr;
		if (err == ESP_OK && pieces > 1) {
			joined = new (std::nothrow) uint8_t[content];
			if (joined == nullptr) {
				ESP_LOGE(kTag, "out of memory writing %s (%u bytes in %u chunks)", name, static_cast<unsigned>(content),
					static_cast<unsigned>(pieces));
				return BlobStatus::storage_error;
			}
			size_t offset = 0;
			for (size_t i = 0; i < count; ++i) {
				if (chunks[i].size > 0) {
					std::memcpy(joined + offset, chunks[i].data, chunks[i].size);
					offset += chunks[i].size;
				}
			}
			single = joined;
		}
		if (err == ESP_OK) {
			err = nvs_set_blob(handle_, data_key, single, content);
		}
		delete[] joined;
		size_t old_size = 0;
		if (err == ESP_OK && nvs_get_blob(handle_, legacy, nullptr, &old_size) == ESP_OK) {
			// The first write after the layout change: the legacy blob goes if it is this name's
			// (read once to check; without the memory for that it stays, and the content key wins).
			uint8_t* old_data = nullptr;
			size_t old_total = 0;
			if (load_legacy(name, old_data, old_total) == BlobStatus::ok) {
				delete[] old_data;
				esp_err_t erased = nvs_erase_key(handle_, legacy);
				if (erased == ESP_OK) {
					ESP_LOGI(kTag, "%s: moved from the legacy layout (%u bytes)", name, static_cast<unsigned>(content));
				} else {
					ESP_LOGW(kTag, "%s: the legacy key %s stays (%s); the new content is read first", name, legacy,
						esp_err_to_name(erased));
				}
			}
		}
	}
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
	char data_key[16];
	key_of(name, 'd', data_key);
	size_t stored = 0;
	esp_err_t err = nvs_get_blob(handle_, data_key, nullptr, &stored);
	if (err == ESP_OK) {
		status = check_owner(name, data_key);
		if (status != BlobStatus::ok) {
			return status == BlobStatus::not_found ? BlobStatus::storage_error : status;
		}
		if (offset >= stored) {
			return BlobStatus::ok;
		}
		if (offset == 0 && size >= stored) {
			// The whole blob: straight into the caller's buffer.
			err = nvs_get_blob(handle_, data_key, buffer, &stored);
			if (err == ESP_OK) {
				length = stored;
			}
			return status_of(err);
		}
		uint8_t* data = new (std::nothrow) uint8_t[stored];
		if (data == nullptr) {
			ESP_LOGE(kTag, "out of memory reading %s (%u bytes)", name, static_cast<unsigned>(stored));
			return BlobStatus::storage_error;
		}
		err = nvs_get_blob(handle_, data_key, data, &stored);
		if (err == ESP_OK && offset < stored) {
			length = stored - offset < size ? stored - offset : size;
			std::memcpy(buffer, data + offset, length);
		}
		delete[] data;
		return status_of(err);
	}
	if (err != ESP_ERR_NVS_NOT_FOUND) {
		return status_of(err);
	}
	uint8_t* data = nullptr;
	size_t total = 0;
	status = load_legacy(name, data, total);
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
	// The lengths alone: nothing is read (read() checks the stored name).
	char key[16];
	key_of(name, 'd', key);
	size_t stored = 0;
	esp_err_t err = nvs_get_blob(handle_, key, nullptr, &stored);
	if (err == ESP_OK) {
		size = stored;
		return BlobStatus::ok;
	}
	if (err != ESP_ERR_NVS_NOT_FOUND) {
		return status_of(err);
	}
	key_of(name, 'b', key);
	err = nvs_get_blob(handle_, key, nullptr, &stored);
	if (err != ESP_OK) {
		return status_of(err);
	}
	size_t name_length = std::strlen(name);
	if (stored <= name_length) {
		ESP_LOGE(kTag, "%s: key %s holds another blob (hash collision)", name, key);
		return BlobStatus::storage_error;
	}
	size = stored - name_length - 1;
	return BlobStatus::ok;
}

BlobStatus NvsBlobStore::remove(const char* name) {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		return status;
	}
	if (handle_ == 0) {
		return BlobStatus::storage_error;
	}
	bool found = false;
	esp_err_t failed = ESP_OK;
	static constexpr char kLetters[] = {'d', 'b', 'n'};
	for (char letter : kLetters) {
		char key[16];
		key_of(name, letter, key);
		esp_err_t err = nvs_erase_key(handle_, key);
		if (err == ESP_OK) {
			found = found || letter != 'n';
		} else if (err != ESP_ERR_NVS_NOT_FOUND) {
			failed = err;
		}
	}
	esp_err_t err = failed != ESP_OK ? failed : nvs_commit(handle_);
	if (err != ESP_OK) {
		return status_of(err);
	}
	return found ? BlobStatus::ok : BlobStatus::not_found;
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
		if (info.key[0] == 'd') {
			char key[16];
			sibling_key(info.key, 'n', key);
			char name[kMaxNameLength + 2] = {};
			size_t length = sizeof(name);
			if (nvs_get_str(handle_, key, name, &length) == ESP_OK &&
				nvs_get_blob(handle_, info.key, nullptr, &stored) == ESP_OK) {
				visitor.blob(name, stored);
			}
		} else if (info.key[0] == 'b') {
			// A legacy blob, unless the content key of the same name replaced it (a write cut
			// short between the two steps).
			char key[16];
			sibling_key(info.key, 'd', key);
			size_t newer = 0;
			bool replaced = nvs_get_blob(handle_, key, nullptr, &newer) == ESP_OK;
			if (!replaced && nvs_get_blob(handle_, info.key, nullptr, &stored) == ESP_OK && stored > 0) {
				uint8_t* data = new (std::nothrow) uint8_t[stored];
				if (data != nullptr && nvs_get_blob(handle_, info.key, data, &stored) == ESP_OK) {
					size_t name_length = strnlen(reinterpret_cast<const char*>(data), stored);
					if (name_length < stored && name_length <= kMaxNameLength) {
						visitor.blob(reinterpret_cast<const char*>(data), stored - name_length - 1);
					}
				}
				delete[] data;
			}
		}
		err = nvs_entry_next(&it);
	}
	nvs_release_iterator(it);
}

} // namespace esp
} // namespace hc
