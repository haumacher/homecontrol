/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/image/image_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "hc/image/image.h"
#include "hc/log.h"
#include "hc/value_store.h"

namespace hc {
namespace image {

namespace {

constexpr const char* kTag = "hc.image";
constexpr size_t kMaxChunks = (kMaxImageBytes + ImageStore::kChunkSize - 1) / ImageStore::kChunkSize;

} // namespace

void ImageStore::chunk_name(char generation, size_t index, char* buffer, size_t size) {
	std::snprintf(buffer, size, "image.%c.%u", generation, static_cast<unsigned>(index));
}

bool ImageStore::read_manifest(Manifest& out, bool& missing) const {
	missing = false;
	char text[80] = {};
	size_t length = 0;
	BlobStatus status = store_.read(kManifest, 0, text, sizeof(text) - 1, length);
	if (status == BlobStatus::not_found) {
		missing = true;
		return false;
	}
	if (status != BlobStatus::ok) {
		Log::error(kTag, "stored image: manifest unreadable (%s)", blob_status_name(status));
		return false;
	}
	text[length] = '\0';
	unsigned version = 0;
	char generation = 0;
	unsigned long bytes = 0;
	unsigned chunks = 0;
	unsigned long crc = 0;
	if (std::sscanf(text, "HCIMG %u %c %lu %u %lx", &version, &generation, &bytes, &chunks, &crc) != 5 ||
		(generation != 'a' && generation != 'b')) {
		Log::error(kTag, "stored image: manifest malformed");
		return false;
	}
	if (version != 1) {
		Log::error(kTag, "stored image: manifest format %u is newer than this firmware's 1", version);
		return false;
	}
	if (bytes > kMaxImageBytes || chunks != (bytes + kChunkSize - 1) / kChunkSize) {
		Log::error(kTag, "stored image: manifest inconsistent (%lu bytes in %u chunks)", bytes, chunks);
		return false;
	}
	out.generation = generation;
	out.bytes = bytes;
	out.chunks = chunks;
	out.crc = static_cast<uint32_t>(crc);
	return true;
}

bool ImageStore::stored(size_t& bytes) const {
	Manifest manifest;
	bool missing = false;
	if (!read_manifest(manifest, missing)) {
		return false;
	}
	bytes = manifest.bytes;
	return true;
}

uint32_t ImageStore::token() const {
	Manifest manifest;
	bool missing = false;
	if (!read_manifest(manifest, missing)) {
		return 0;
	}
	return manifest.crc ^ (manifest.generation == 'a' ? 0x5a5a5a5au : 0xa5a5a5a5u) ^
		static_cast<uint32_t>(manifest.bytes);
}

bool ImageStore::read(char* buffer, size_t size, size_t& length, bool& missing, const char** error) const {
	Manifest manifest;
	length = 0;
	if (!read_manifest(manifest, missing)) {
		*error = missing ? "no image stored" : "the stored image's manifest is damaged";
		return false;
	}
	if (size < manifest.bytes) {
		*error = "the buffer is smaller than the stored image";
		return false;
	}
	size_t at = 0;
	for (size_t i = 0; i < manifest.chunks; ++i) {
		char name[BlobStore::kMaxNameLength + 1];
		chunk_name(manifest.generation, i, name, sizeof(name));
		size_t want = manifest.bytes - at < kChunkSize ? manifest.bytes - at : kChunkSize;
		size_t got = 0;
		BlobStatus status = store_.read(name, 0, buffer + at, want, got);
		if (status != BlobStatus::ok || got != want) {
			Log::error(kTag, "stored image: chunk %s %s", name,
				status != BlobStatus::ok ? blob_status_name(status) : "is short");
			*error = "a chunk of the stored image is missing or short";
			return false;
		}
		at += got;
	}
	uint32_t crc = persist::crc32(0, buffer, manifest.bytes);
	if (crc != manifest.crc) {
		Log::error(kTag, "stored image: checksum %08x, the manifest says %08x", static_cast<unsigned>(crc),
			static_cast<unsigned>(manifest.crc));
		*error = "the stored image is damaged (checksum)";
		return false;
	}
	length = manifest.bytes;
	return true;
}

bool ImageStore::read(ImageBody& body, bool& missing, const char** error) const {
	Manifest manifest;
	if (!read_manifest(manifest, missing)) {
		*error = missing ? "no image stored" : "the stored image's manifest is damaged";
		return false;
	}
	if (!body.active() || body.size() != manifest.bytes || body.have() != 0) {
		*error = "the stored image changed while it was read";
		return false;
	}
	char* piece = new (std::nothrow) char[kReadPiece];
	if (piece == nullptr) {
		*error = "no memory to read the stored image";
		return false;
	}
	uint32_t crc = 0;
	size_t at = 0;
	bool ok = true;
	for (size_t i = 0; ok && i < manifest.chunks; ++i) {
		char name[BlobStore::kMaxNameLength + 1];
		chunk_name(manifest.generation, i, name, sizeof(name));
		size_t chunk_bytes = manifest.bytes - at < kChunkSize ? manifest.bytes - at : kChunkSize;
		for (size_t within = 0; within < chunk_bytes;) {
			size_t want = chunk_bytes - within < kReadPiece ? chunk_bytes - within : kReadPiece;
			size_t got = 0;
			BlobStatus status = store_.read(name, within, piece, want, got);
			if (status != BlobStatus::ok || got != want) {
				Log::error(kTag, "stored image: chunk %s %s", name,
					status != BlobStatus::ok ? blob_status_name(status) : "is short");
				*error = "a chunk of the stored image is missing or short";
				ok = false;
				break;
			}
			if (!body.append(piece, got)) {
				Log::error(kTag, "stored image: the buffer refused %u bytes at %u", static_cast<unsigned>(got),
					static_cast<unsigned>(at + within));
				*error = "the stored image could not be put together (the staging area failed)";
				ok = false;
				break;
			}
			crc = persist::crc32(crc, piece, got);
			within += got;
		}
		at += chunk_bytes;
	}
	delete[] piece;
	if (!ok) {
		return false;
	}
	if (crc != manifest.crc) {
		Log::error(kTag, "stored image: checksum %08x, the manifest says %08x", static_cast<unsigned>(crc),
			static_cast<unsigned>(manifest.crc));
		*error = "the stored image is damaged (checksum)";
		return false;
	}
	if (body.finish() == nullptr) {
		*error = "the stored image cannot be read back from the staging area";
		return false;
	}
	return true;
}

bool ImageStore::read_part(uint32_t token, size_t offset, char* buffer, size_t size, size_t& length) const {
	length = 0;
	Manifest manifest;
	bool missing = false;
	if (!read_manifest(manifest, missing)) {
		return false;
	}
	uint32_t now = manifest.crc ^ (manifest.generation == 'a' ? 0x5a5a5a5au : 0xa5a5a5a5u) ^
		static_cast<uint32_t>(manifest.bytes);
	if (now != token) {
		return false;
	}
	while (length < size && offset < manifest.bytes) {
		size_t index = offset / kChunkSize;
		size_t within = offset % kChunkSize;
		size_t chunk_bytes = manifest.bytes - index * kChunkSize < kChunkSize ? manifest.bytes - index * kChunkSize
			: kChunkSize;
		size_t want = chunk_bytes - within;
		if (want > size - length) {
			want = size - length;
		}
		char name[BlobStore::kMaxNameLength + 1];
		chunk_name(manifest.generation, index, name, sizeof(name));
		size_t got = 0;
		if (store_.read(name, within, buffer + length, want, got) != BlobStatus::ok || got != want) {
			return false;
		}
		length += got;
		offset += got;
	}
	return true;
}

bool ImageStore::write(const char* text, size_t length, const char** error) {
	if (length > kMaxImageBytes) {
		*error = "the image is larger than the limit";
		return false;
	}
	Manifest old;
	bool missing = false;
	bool has_old = read_manifest(old, missing);
	char generation = has_old && old.generation == 'a' ? 'b' : 'a';
	size_t chunks = (length + kChunkSize - 1) / kChunkSize;
	for (size_t i = 0; i < chunks; ++i) {
		char name[BlobStore::kMaxNameLength + 1];
		chunk_name(generation, i, name, sizeof(name));
		size_t size = length - i * kChunkSize < kChunkSize ? length - i * kChunkSize : kChunkSize;
		// In pieces of kPieceSize: the store writes each with a call of its own (LittleFS: one write()
		// each), so the file system's lock, which the loop may wait for while this runs in the worker
		// task (#68), is held for one piece at a time, not for the whole chunk.
		BlobChunk pieces[kChunkSize / kPieceSize];
		size_t count = 0;
		for (size_t at = 0; at < size; at += kPieceSize) {
			pieces[count++] = BlobChunk{text + i * kChunkSize + at, size - at < kPieceSize ? size - at : kPieceSize};
		}
		BlobStatus status = store_.write(name, pieces, count);
		if (status != BlobStatus::ok) {
			Log::error(kTag, "storing the image: chunk %s: %s; the stored image is unchanged", name,
				blob_status_name(status));
			*error = status == BlobStatus::no_space ? "the storage is full" : "the storage refused a write";
			collect_garbage();
			return false;
		}
	}
	char manifest[80];
	int n = std::snprintf(manifest, sizeof(manifest), "HCIMG 1 %c %u %u %08x\n", generation,
		static_cast<unsigned>(length), static_cast<unsigned>(chunks),
		static_cast<unsigned>(persist::crc32(0, text, length)));
	BlobChunk chunk{manifest, static_cast<size_t>(n)};
	BlobStatus status = store_.write(kManifest, &chunk, 1);
	if (status != BlobStatus::ok) {
		Log::error(kTag, "storing the image: manifest: %s; the stored image is unchanged", blob_status_name(status));
		*error = status == BlobStatus::no_space ? "the storage is full" : "the storage refused a write";
		collect_garbage();
		return false;
	}
	collect_garbage();
	return true;
}

size_t ImageStore::storable(size_t free) const {
	size_t manifest = store_.footprint(kManifestBytes);
	if (free < manifest) {
		return 0;
	}
	size_t left = free - manifest;
	size_t bytes = 0;
	size_t whole = store_.footprint(kChunkSize);
	while (bytes + kChunkSize <= kMaxImageBytes && whole <= left) {
		bytes += kChunkSize;
		left -= whole;
	}
	if (bytes == kMaxImageBytes) {
		return bytes;
	}
	// The last chunk: the longest that fits what is left (footprint() grows with the size).
	size_t low = 0;
	size_t high = kChunkSize - 1 < kMaxImageBytes - bytes ? kChunkSize - 1 : kMaxImageBytes - bytes;
	while (low < high) {
		size_t mid = low + (high - low + 1) / 2;
		if (store_.footprint(mid) <= left) {
			low = mid;
		} else {
			high = mid - 1;
		}
	}
	return bytes + low;
}

bool ImageStore::remove(const char** error) {
	BlobStatus status = store_.remove(kManifest);
	if (status != BlobStatus::ok && status != BlobStatus::not_found) {
		Log::error(kTag, "removing the stored image: %s", blob_status_name(status));
		*error = "the storage refused to remove the image";
		return false;
	}
	collect_garbage();
	return true;
}

void ImageStore::collect_garbage() {
	Manifest manifest;
	bool missing = false;
	bool has = read_manifest(manifest, missing);
	if (!has && !missing) {
		return; // a damaged manifest: keep the chunks for inspection
	}
	// The stale names first (the visitor must not modify the store), then remove them; a few per
	// pass on the stack (no static collector of every name), repeated until a pass finds no more.
	constexpr size_t kBatch = 8;
	struct Collector : BlobVisitor {
		const Manifest* manifest = nullptr;
		char names[kBatch][BlobStore::kMaxNameLength + 1];
		size_t count = 0;
		bool more = false;
		void blob(const char* name, size_t) override {
			if (std::strncmp(name, "image.", 6) != 0 || std::strcmp(name, kManifest) == 0) {
				return;
			}
			if (manifest != nullptr && name[6] == manifest->generation && name[7] == '.') {
				char* end = nullptr;
				unsigned long index = std::strtoul(name + 8, &end, 10);
				if (end != name + 8 && *end == '\0' && index < manifest->chunks) {
					return; // a chunk of the stored image
				}
			}
			if (count < kBatch) {
				std::snprintf(names[count++], sizeof(names[0]), "%s", name);
			} else {
				more = true;
			}
		}
	};
	for (;;) {
		Collector collector;
		collector.manifest = has ? &manifest : nullptr;
		store_.visit(collector);
		size_t removed = 0;
		for (size_t i = 0; i < collector.count; ++i) {
			if (store_.remove(collector.names[i]) == BlobStatus::ok) {
				++removed;
			}
		}
		if (!collector.more || removed == 0) {
			return; // done, or the store refuses (logged by it): no endless loop
		}
	}
}

} // namespace image
} // namespace hc
