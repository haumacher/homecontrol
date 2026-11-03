/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_IMAGE_STORE_H
#define HC_IMAGE_IMAGE_STORE_H

#include <cstddef>
#include <cstdint>

#include "hc/platform/blob_store.h"

namespace hc {
namespace image {

/// The stored runtime image in the platform's BlobStore: the JSON text exactly as received, in
/// chunks of kChunkSize (a blob holds at most BlobStore::kMaxBlobSize), with a manifest written
/// last. Replacing the image is atomic: the new chunks go to the other of two generations
/// (`image.a.<n>`, `image.b.<n>`), then the manifest `image.idx` (one blob, an atomic replace)
/// switches to them, then the old generation's chunks are removed. A power cut leaves the complete
/// old or the complete new image; stray chunks are removed at the next write or check. The
/// manifest carries the size and a CRC-32 of the whole text, so a damaged chunk is detected
/// when the image is read.
///
/// Manifest format (text, frozen): "HCIMG 1 <generation a|b> <bytes> <chunks> <crc32 hex>\n".
class ImageStore {
public:
	static constexpr size_t kChunkSize = 16384;
	/// A chunk is handed to BlobStore::write() in pieces of this size (one file write each).
	static constexpr size_t kPieceSize = 4096;
	static_assert(kChunkSize % kPieceSize == 0, "a chunk is whole pieces");
	static constexpr const char* kManifest = "image.idx";

	explicit ImageStore(BlobStore& store) : store_(store) {}

	/// Whether an image is stored; `bytes` its size.
	bool stored(size_t& bytes) const;

	/// Reads the whole stored image into `buffer` (at least its size); checks size and CRC.
	/// False with `error` when there is none (`missing` set), it is damaged or unreadable.
	bool read(char* buffer, size_t size, size_t& length, bool& missing, const char** error) const;

	/// Reads `size` bytes from `offset` of the stored image (for GET /api/image). `token` is the
	/// value of token() when the reader started: a changed image refuses the read.
	bool read_part(uint32_t token, size_t offset, char* buffer, size_t size, size_t& length) const;

	/// Changes whenever the stored image changes (CRC and generation of the manifest).
	uint32_t token() const;

	/// Replaces the stored image. False with `error` (nothing changed when the chunks could not be
	/// written; the old image stays).
	bool write(const char* text, size_t length, const char** error);

	/// Removes the stored image (the manifest first, so a cut leaves none).
	bool remove(const char** error);

	/// Removes chunks no manifest refers to (after a power cut during a write).
	void collect_garbage();

private:
	struct Manifest {
		char generation = 'a';
		size_t bytes = 0;
		size_t chunks = 0;
		uint32_t crc = 0;
	};

	bool read_manifest(Manifest& out, bool& missing) const;
	static void chunk_name(char generation, size_t index, char* buffer, size_t size);

	BlobStore& store_;
};

} // namespace image
} // namespace hc

#endif // HC_IMAGE_IMAGE_STORE_H
