/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// A BlobStore in RAM: the stand-in until the platforms' flash and file stores are merged.
#ifndef HC_IMAGE_MEMORY_BLOB_STORE_H
#define HC_IMAGE_MEMORY_BLOB_STORE_H

#include <cstddef>
#include <cstdint>

#include "hc/platform/blob_store.h"

namespace hc {
namespace image {

/// Keeps the BlobStore contract (name rule, size limit, atomic replace, no_space when full) in
/// heap memory, up to `capacity` bytes of content in at most kMaxBlobs blobs. Nothing survives a
/// restart: with this store the runtime image, the persisted values and the history rings are
/// lost on reboot (spec/runtime-image.md "Storage"). A write allocates the new content before it
/// frees the old one, so a failed allocation keeps the previous blob (no_space). A streaming write
/// (open_writer()) collects into a heap buffer of its own (sized from `expected`, doubled when it is
/// exceeded) that commit() installs as the blob.
///
/// Threading: the event loop's thread, like every BlobStore.
class MemoryBlobStore : public BlobStore {
public:
	static constexpr size_t kMaxBlobs = 64;

	explicit MemoryBlobStore(size_t capacity) : capacity_(capacity) {}
	~MemoryBlobStore();

	MemoryBlobStore(const MemoryBlobStore&) = delete;
	MemoryBlobStore& operator=(const MemoryBlobStore&) = delete;

	BlobStatus write(const char* name, const BlobChunk* chunks, size_t count) override;
	BlobStatus read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const override;
	BlobStatus size(const char* name, size_t& size) const override;
	BlobStatus remove(const char* name) override;
	void visit(BlobVisitor& visitor) const override;
	BlobWriter* open_writer(const char* name, size_t expected, BlobStatus& status) override;
	bool free_space(size_t& bytes) const override;

	/// Content bytes held.
	size_t used() const { return used_; }
	size_t capacity() const { return capacity_; }

private:
	struct Blob {
		char name[kMaxNameLength + 1];
		uint8_t* data;
		size_t size;
	};

	class Writer : public BlobWriter {
	public:
		explicit Writer(MemoryBlobStore& store) : store_(store) {}
		~Writer() { delete[] data; }

		BlobStatus append(const void* bytes, size_t size) override;
		BlobStatus commit() override;
		void discard() override;
		size_t written() const override { return size; }

		bool open = false;
		char name[kMaxNameLength + 1] = {};
		uint8_t* data = nullptr;
		size_t size = 0;
		size_t allocated = 0;

	private:
		MemoryBlobStore& store_;
	};

	const Blob* find(const char* name) const;
	/// Room for `total` bytes as the content of `name` (replacing its previous content); logs the
	/// refusal.
	BlobStatus room(const char* name, size_t total) const;

	size_t capacity_;
	size_t used_ = 0;
	Blob blobs_[kMaxBlobs] = {};
	size_t count_ = 0;
	Writer writer_{*this};
};

} // namespace image
} // namespace hc

#endif // HC_IMAGE_MEMORY_BLOB_STORE_H
