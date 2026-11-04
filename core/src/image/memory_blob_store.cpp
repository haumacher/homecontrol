/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/image/memory_blob_store.h"

#include <cstring>
#include <new>

#include "hc/log.h"

namespace hc {
namespace image {

namespace {
constexpr const char* kTag = "hc.blob";
}

MemoryBlobStore::~MemoryBlobStore() {
	for (size_t i = 0; i < count_; ++i) {
		delete[] blobs_[i].data;
	}
}

const MemoryBlobStore::Blob* MemoryBlobStore::find(const char* name) const {
	for (size_t i = 0; i < count_; ++i) {
		if (std::strcmp(blobs_[i].name, name) == 0) {
			return &blobs_[i];
		}
	}
	return nullptr;
}

BlobStatus MemoryBlobStore::write(const char* name, const BlobChunk* chunks, size_t count) {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		return status;
	}
	size_t total = 0;
	for (size_t i = 0; i < count; ++i) {
		total += chunks[i].size;
	}
	if (total > kMaxBlobSize) {
		return BlobStatus::too_large;
	}
	status = room(name, total);
	if (status != BlobStatus::ok) {
		return status;
	}
	Blob* blob = const_cast<Blob*>(find(name));
	size_t previous = blob != nullptr ? blob->size : 0;
	uint8_t* data = new (std::nothrow) uint8_t[total > 0 ? total : 1];
	if (data == nullptr) {
		Log::warn(kTag, "refusing blob %s: no memory for %u bytes", name, static_cast<unsigned>(total));
		return BlobStatus::no_space;
	}
	size_t at = 0;
	for (size_t i = 0; i < count; ++i) {
		std::memcpy(data + at, chunks[i].data, chunks[i].size);
		at += chunks[i].size;
	}
	if (blob == nullptr) {
		blob = &blobs_[count_++];
		std::strcpy(blob->name, name);
	} else {
		delete[] blob->data;
	}
	blob->data = data;
	blob->size = total;
	used_ = used_ - previous + total;
	changed();
	return BlobStatus::ok;
}

BlobStatus MemoryBlobStore::room(const char* name, size_t total) const {
	const Blob* blob = find(name);
	size_t previous = blob != nullptr ? blob->size : 0;
	if (used_ - previous + total > capacity_) {
		Log::warn(kTag, "refusing blob %s (%u bytes): the RAM store holds %u of %u bytes", name,
			static_cast<unsigned>(total), static_cast<unsigned>(used_), static_cast<unsigned>(capacity_));
		return BlobStatus::no_space;
	}
	if (blob == nullptr && count_ >= kMaxBlobs) {
		Log::warn(kTag, "refusing blob %s: the RAM store holds %u blobs already", name,
			static_cast<unsigned>(kMaxBlobs));
		return BlobStatus::no_space;
	}
	return BlobStatus::ok;
}

BlobStatus MemoryBlobStore::patch(const char* name, size_t offset, const BlobChunk* chunks, size_t count) {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		return status;
	}
	Blob* blob = const_cast<Blob*>(find(name));
	if (blob == nullptr) {
		return BlobStatus::not_found;
	}
	size_t total = 0;
	for (size_t i = 0; i < count; ++i) {
		total += chunks[i].size;
	}
	if (offset > blob->size || total > blob->size - offset) {
		return BlobStatus::too_large;
	}
	// In place: nothing can fail half-way in RAM.
	for (size_t i = 0; i < count; ++i) {
		std::memcpy(blob->data + offset, chunks[i].data, chunks[i].size);
		offset += chunks[i].size;
	}
	changed();
	return BlobStatus::ok;
}

bool MemoryBlobStore::free_space(size_t& bytes) const {
	bytes = capacity_ - used_;
	return true;
}

BlobWriter* MemoryBlobStore::open_writer(const char* name, size_t expected, BlobStatus& status) {
	status = check_name(name);
	if (status != BlobStatus::ok) {
		return nullptr;
	}
	if (writer_.open) {
		status = BlobStatus::busy;
		return nullptr;
	}
	status = room(name, expected);
	if (status != BlobStatus::ok) {
		return nullptr;
	}
	writer_.open = true;
	std::strcpy(writer_.name, name);
	writer_.size = 0;
	if (expected > writer_.allocated) {
		delete[] writer_.data;
		writer_.data = new (std::nothrow) uint8_t[expected];
		writer_.allocated = writer_.data != nullptr ? expected : 0;
		// Without memory now, append() tries again in smaller steps.
	}
	return &writer_;
}

BlobStatus MemoryBlobStore::Writer::append(const void* bytes, size_t length) {
	if (length == 0) {
		return BlobStatus::ok;
	}
	if (store_.room(name, size + length) != BlobStatus::ok) {
		return BlobStatus::no_space;
	}
	if (size + length > allocated) {
		size_t grown = allocated * 2 > size + length ? allocated * 2 : size + length;
		uint8_t* bigger = new (std::nothrow) uint8_t[grown];
		if (bigger == nullptr) {
			Log::warn(kTag, "streaming write of %s: no memory for %u bytes", name, static_cast<unsigned>(grown));
			return BlobStatus::no_space;
		}
		if (size > 0) {
			std::memcpy(bigger, data, size);
		}
		delete[] data;
		data = bigger;
		allocated = grown;
	}
	std::memcpy(data + size, bytes, length);
	size += length;
	return BlobStatus::ok;
}

BlobStatus MemoryBlobStore::Writer::commit() {
	open = false;
	BlobStatus status = store_.room(name, size);
	if (status != BlobStatus::ok) {
		return status;
	}
	Blob* blob = const_cast<Blob*>(store_.find(name));
	size_t previous = blob != nullptr ? blob->size : 0;
	if (blob == nullptr) {
		blob = &store_.blobs_[store_.count_++];
		std::strcpy(blob->name, name);
	} else {
		delete[] blob->data;
	}
	// The buffer becomes the blob's content (no copy; nullptr for an empty blob); the writer
	// allocates anew next time.
	blob->data = data;
	blob->size = size;
	store_.used_ = store_.used_ - previous + size;
	data = nullptr;
	allocated = 0;
	size = 0;
	store_.changed();
	return BlobStatus::ok;
}

void MemoryBlobStore::Writer::discard() {
	open = false;
	delete[] data;
	data = nullptr;
	allocated = 0;
	size = 0;
}

BlobStatus MemoryBlobStore::read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const {
	length = 0;
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		return status;
	}
	const Blob* blob = find(name);
	if (blob == nullptr) {
		return BlobStatus::not_found;
	}
	if (offset < blob->size) {
		length = blob->size - offset < size ? blob->size - offset : size;
		std::memcpy(buffer, blob->data + offset, length);
	}
	return BlobStatus::ok;
}

BlobStatus MemoryBlobStore::size(const char* name, size_t& size) const {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		return status;
	}
	const Blob* blob = find(name);
	if (blob == nullptr) {
		return BlobStatus::not_found;
	}
	size = blob->size;
	return BlobStatus::ok;
}

BlobStatus MemoryBlobStore::remove(const char* name) {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		return status;
	}
	Blob* blob = const_cast<Blob*>(find(name));
	if (blob == nullptr) {
		return BlobStatus::not_found;
	}
	delete[] blob->data;
	used_ -= blob->size;
	*blob = blobs_[--count_];
	blobs_[count_] = Blob();
	changed();
	return BlobStatus::ok;
}

void MemoryBlobStore::visit(BlobVisitor& visitor) const {
	for (size_t i = 0; i < count_; ++i) {
		visitor.blob(blobs_[i].name, blobs_[i].size);
	}
}

} // namespace image
} // namespace hc
