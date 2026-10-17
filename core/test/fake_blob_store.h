/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// A BlobStore in memory with write counters and failure injection, for the persistence tests.
#ifndef HC_FAKE_BLOB_STORE_H
#define HC_FAKE_BLOB_STORE_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "hc/platform/blob_store.h"

namespace hc_test {

/// Keeps the contract of hc::BlobStore (name rule, size limit, atomic replace) in a map.
class FakeBlobStore : public hc::BlobStore {
public:
	hc::BlobStatus write(const char* name, const hc::BlobChunk* chunks, size_t count) override {
		hc::BlobStatus status = check_name(name);
		if (status != hc::BlobStatus::ok) {
			return status;
		}
		std::vector<uint8_t> content;
		for (size_t i = 0; i < count; ++i) {
			const uint8_t* p = static_cast<const uint8_t*>(chunks[i].data);
			content.insert(content.end(), p, p + chunks[i].size);
		}
		if (content.size() > kMaxBlobSize) {
			return hc::BlobStatus::too_large;
		}
		if (fail_writes) {
			return hc::BlobStatus::storage_error;
		}
		blobs[name] = content;
		++writes;
		++writes_of[name];
		return hc::BlobStatus::ok;
	}

	hc::BlobStatus read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const override {
		length = 0;
		hc::BlobStatus status = check_name(name);
		if (status != hc::BlobStatus::ok) {
			return status;
		}
		auto it = blobs.find(name);
		if (it == blobs.end()) {
			return hc::BlobStatus::not_found;
		}
		if (offset < it->second.size()) {
			length = std::min(size, it->second.size() - offset);
			std::memcpy(buffer, it->second.data() + offset, length);
		}
		return hc::BlobStatus::ok;
	}

	hc::BlobStatus size(const char* name, size_t& size) const override {
		hc::BlobStatus status = check_name(name);
		if (status != hc::BlobStatus::ok) {
			return status;
		}
		auto it = blobs.find(name);
		if (it == blobs.end()) {
			return hc::BlobStatus::not_found;
		}
		size = it->second.size();
		return hc::BlobStatus::ok;
	}

	hc::BlobStatus remove(const char* name) override {
		hc::BlobStatus status = check_name(name);
		if (status != hc::BlobStatus::ok) {
			return status;
		}
		return blobs.erase(name) > 0 ? hc::BlobStatus::ok : hc::BlobStatus::not_found;
	}

	void visit(hc::BlobVisitor& visitor) const override {
		for (const auto& blob : blobs) {
			visitor.blob(blob.first.c_str(), blob.second.size());
		}
	}

	std::map<std::string, std::vector<uint8_t>> blobs;
	std::map<std::string, int> writes_of;
	int writes = 0;
	bool fail_writes = false;
};

} // namespace hc_test

#endif // HC_FAKE_BLOB_STORE_H
