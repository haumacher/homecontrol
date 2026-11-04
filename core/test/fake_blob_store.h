/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// A BlobStore in memory with write counters and failure injection, for the persistence tests.
#ifndef HC_FAKE_BLOB_STORE_H
#define HC_FAKE_BLOB_STORE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "hc/platform/blob_store.h"

namespace hc_test {

/// Keeps the contract of hc::BlobStore (name rule, size limit, atomic replace) in a map. A
/// streaming write (open_writer()) collects into `temporaries` under the LittleFS store's temporary name
/// (`.<name>.tmp`), which visit() does not list, and moves it into `blobs` on commit.
class FakeBlobStore : public hc::BlobStore {
public:
	FakeBlobStore() : writer_(*this) {}
	/// Copies the content (blobs, counters, settings); an open streaming write stays behind.
	FakeBlobStore(const FakeBlobStore& other) : hc::BlobStore(), writer_(*this) { *this = other; }
	FakeBlobStore& operator=(const FakeBlobStore& other) {
		if (this != &other) {
			blobs = other.blobs;
			writes_of = other.writes_of;
			writes = other.writes;
			fail_writes = other.fail_writes;
			free_bytes = other.free_bytes;
			patches = other.patches;
			patches_of = other.patches_of;
			patched_bytes = other.patched_bytes;
			patchable = other.patchable;
			has_usage = other.has_usage;
			total_bytes = other.total_bytes;
			used_bytes = other.used_bytes;
			changed();
		}
		return *this;
	}

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
		changed();
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
		if (blobs.erase(name) == 0) {
			return hc::BlobStatus::not_found;
		}
		changed();
		return hc::BlobStatus::ok;
	}

	void visit(hc::BlobVisitor& visitor) const override {
		++visits;
		for (const auto& blob : blobs) {
			visitor.blob(blob.first.c_str(), blob.second.size());
		}
	}

	hc::BlobWriter* open_writer(const char* name, size_t expected, hc::BlobStatus& status) override {
		status = check_name(name);
		if (status != hc::BlobStatus::ok) {
			return nullptr;
		}
		if (writer_.open) {
			status = hc::BlobStatus::busy;
			return nullptr;
		}
		if (expected > free_bytes) {
			status = hc::BlobStatus::no_space;
			return nullptr;
		}
		++opens;
		writer_.open = true;
		writer_.name = name;
		writer_.temporary = std::string(".") + name + ".tmp";
		temporaries[writer_.temporary].clear();
		return &writer_;
	}

	hc::BlobStatus patch(const char* name, size_t offset, const hc::BlobChunk* chunks, size_t count) override {
		hc::BlobStatus status = check_name(name);
		if (status != hc::BlobStatus::ok) {
			return status;
		}
		auto it = blobs.find(name);
		if (it == blobs.end()) {
			return hc::BlobStatus::not_found;
		}
		size_t total = 0;
		for (size_t i = 0; i < count; ++i) {
			total += chunks[i].size;
		}
		if (offset > it->second.size() || total > it->second.size() - offset) {
			return hc::BlobStatus::too_large;
		}
		if (fail_writes) {
			return hc::BlobStatus::storage_error;
		}
		if (!patchable) {
			return hc::BlobStatus::unsupported;
		}
		for (size_t i = 0; i < count; ++i) {
			std::memcpy(it->second.data() + offset, chunks[i].data, chunks[i].size);
			offset += chunks[i].size;
		}
		++patches;
		++patches_of[name];
		patched_bytes += total;
		changed();
		return hc::BlobStatus::ok;
	}

	bool free_space(size_t& bytes) const override {
		++free_space_calls;
		bytes = free_bytes;
		return true;
	}

	bool usage(uint64_t& total, uint64_t& used) const override {
		++usage_calls;
		if (!has_usage) {
			return false;
		}
		total = total_bytes;
		used = used_bytes;
		return true;
	}

	/// A change of the store's state the test made directly (free_bytes, the usage): as a write would.
	void touch() { changed(); }

	/// The walks a real store makes (homecontrol #74): visit(), free_space() and usage() calls.
	int walks() const { return visits + free_space_calls + usage_calls; }

	/// Whether a streaming write is open.
	bool writing() const { return writer_.open; }

	std::map<std::string, std::vector<uint8_t>> blobs;
	/// The streaming writes in progress, by temporary name.
	std::map<std::string, std::vector<uint8_t>> temporaries;
	/// What free_space() reports and open_writer() checks `expected` against; appends beyond it fail with
	/// no_space.
	size_t free_bytes = SIZE_MAX;
	int opens = 0;
	int appends = 0;
	size_t appended_bytes = 0;
	int commits = 0;
	int discards = 0;
	std::map<std::string, int> writes_of;
	int writes = 0;
	bool fail_writes = false;
	/// patch() calls that changed a blob, by name, and the bytes they wrote.
	int patches = 0;
	std::map<std::string, int> patches_of;
	size_t patched_bytes = 0;
	/// False: patch() refuses with unsupported (a store that cannot patch).
	bool patchable = true;
	/// What usage() reports (false: it cannot tell, the default; then free_space() is asked).
	bool has_usage = false;
	uint64_t total_bytes = 0;
	uint64_t used_bytes = 0;
	mutable int visits = 0;
	mutable int free_space_calls = 0;
	mutable int usage_calls = 0;

private:
	class Writer : public hc::BlobWriter {
	public:
		explicit Writer(FakeBlobStore& store) : store_(store) {}

		hc::BlobStatus append(const void* data, size_t size) override {
			std::vector<uint8_t>& content = store_.temporaries[temporary];
			if (size > store_.free_bytes || content.size() > store_.free_bytes - size) {
				return hc::BlobStatus::no_space;
			}
			const uint8_t* p = static_cast<const uint8_t*>(data);
			content.insert(content.end(), p, p + size);
			++store_.appends;
			store_.appended_bytes += size;
			return hc::BlobStatus::ok;
		}

		hc::BlobStatus commit() override {
			open = false;
			std::vector<uint8_t> content = std::move(store_.temporaries[temporary]);
			store_.temporaries.erase(temporary);
			if (store_.fail_writes) {
				return hc::BlobStatus::storage_error;
			}
			store_.blobs[name] = std::move(content);
			++store_.commits;
			++store_.writes_of[name];
			store_.changed();
			return hc::BlobStatus::ok;
		}

		void discard() override {
			open = false;
			store_.temporaries.erase(temporary);
			++store_.discards;
		}

		size_t written() const override {
			auto it = store_.temporaries.find(temporary);
			return it != store_.temporaries.end() ? it->second.size() : 0;
		}

		bool open = false;
		std::string name;
		std::string temporary;

	private:
		FakeBlobStore& store_;
	};

	Writer writer_;
};

} // namespace hc_test

#endif // HC_FAKE_BLOB_STORE_H
