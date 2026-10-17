/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_BLOB_STORE_H
#define HC_PLATFORM_BLOB_STORE_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// Outcome of a BlobStore call. Everything except `ok` is a refusal; blob_status_name() gives the
/// text to log.
enum class BlobStatus : uint8_t {
	ok,
	not_found,     ///< No blob with this name.
	invalid_name,  ///< Empty, longer than BlobStore::kMaxNameLength, a leading dot or a character outside [A-Za-z0-9._-].
	too_large,     ///< The blob would exceed BlobStore::kMaxBlobSize.
	no_space,      ///< The backing store is full; the previous content of the blob is unchanged.
	storage_error, ///< The backing store failed (flash, file system); the platform logged the cause.
};

/// "ok", "not found", "name invalid", "too large", "no space", "storage error".
inline const char* blob_status_name(BlobStatus status) {
	switch (status) {
		case BlobStatus::ok: return "ok";
		case BlobStatus::not_found: return "not found";
		case BlobStatus::invalid_name: return "name invalid";
		case BlobStatus::too_large: return "too large";
		case BlobStatus::no_space: return "no space";
		case BlobStatus::storage_error: return "storage error";
	}
	return "?";
}

/// One piece of a blob to write: BlobStore::write() concatenates the chunks, so a caller writes a
/// header and a ring buffer's two halves without assembling them in a buffer first.
struct BlobChunk {
	const void* data;
	size_t size;
};

/// Receives the names of BlobStore::visit().
class BlobVisitor {
public:
	virtual void blob(const char* name, size_t size) = 0;

protected:
	~BlobVisitor() = default;
};

/// Named binary blobs of up to a few KB that survive a restart: the last values of persisted
/// datapoints and the history rings (doc/persistence.md). Settings are too small for them (NVS
/// strings of 127 bytes).
///
/// The contract every implementation keeps (the ESP one on the LittleFS `storage` partition, the
/// POSIX one on files in a directory, the tests' FakeBlobStore in memory):
///
/// - A flat namespace of its own: names are 1..kMaxNameLength characters of [A-Za-z0-9._-], not
///   starting with a dot (so the implementation may use dot names for its temporary files); a name
///   is never interpreted as a path. Other users of the same file system cannot see or clash with
///   the blobs (a directory of their own).
/// - write() is an atomic replace: after a power cut at any moment the blob holds either the
///   complete previous content (or does not exist, if it did not) or the complete new content,
///   never a mix or a truncation (LittleFS: write a temporary file and rename it over the old one;
///   POSIX: write, fsync, rename, fsync the directory). When write() returns ok the content is
///   durable.
/// - Blobs are at most kMaxBlobSize bytes; a larger write is refused (too_large) before anything
///   changes. A full store refuses with no_space and keeps the previous content.
/// - read() returns the bytes at `offset` (fewer at the end of the blob; 0 at or past the end).
/// - Failures return a status, never abort; the implementation logs their cause.
///
/// Threading: not thread-safe; the core calls it on the event loop's thread. A write may take
/// milliseconds (flash erase); the callers rate-limit and batch their writes.
class BlobStore {
public:
	/// Longest name in characters (fits a LittleFS name with a temporary suffix).
	static constexpr size_t kMaxNameLength = 31;

	/// Largest blob in bytes.
	static constexpr size_t kMaxBlobSize = 20480;

	/// Replaces the blob `name` by the concatenation of `count` chunks (creating it if needed).
	virtual BlobStatus write(const char* name, const BlobChunk* chunks, size_t count) = 0;

	/// Copies up to `size` bytes of the blob from `offset` into `buffer`; `length` receives the
	/// number of bytes copied.
	virtual BlobStatus read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const = 0;

	/// The size of the blob in bytes.
	virtual BlobStatus size(const char* name, size_t& size) const = 0;

	/// Deletes the blob; not_found if there is none.
	virtual BlobStatus remove(const char* name) = 0;

	/// Calls `visitor` for every blob (order unspecified). The visitor must not modify the store.
	virtual void visit(BlobVisitor& visitor) const = 0;

	/// The name rule every implementation applies before it touches its store.
	static BlobStatus check_name(const char* name) {
		if (name == nullptr || name[0] == '\0' || name[0] == '.') {
			return BlobStatus::invalid_name;
		}
		size_t length = 0;
		for (const char* p = name; *p != '\0'; ++p, ++length) {
			char c = *p;
			bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
				c == '_' || c == '-';
			if (!ok || length >= kMaxNameLength) {
				return BlobStatus::invalid_name;
			}
		}
		return BlobStatus::ok;
	}

protected:
	~BlobStore() = default;
};

} // namespace hc

#endif // HC_PLATFORM_BLOB_STORE_H
