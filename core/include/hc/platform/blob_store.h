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
	busy,          ///< A streaming write (BlobStore::open_writer()) is open already.
	unsupported,   ///< This store has no streaming write.
};

/// "ok", "not found", "name invalid", "too large", "no space", "storage error", "busy", "unsupported".
inline const char* blob_status_name(BlobStatus status) {
	switch (status) {
		case BlobStatus::ok: return "ok";
		case BlobStatus::not_found: return "not found";
		case BlobStatus::invalid_name: return "name invalid";
		case BlobStatus::too_large: return "too large";
		case BlobStatus::no_space: return "no space";
		case BlobStatus::storage_error: return "storage error";
		case BlobStatus::busy: return "busy";
		case BlobStatus::unsupported: return "unsupported";
	}
	return "?";
}

/// One piece of a blob to write: BlobStore::write() concatenates the chunks, so a caller writes a
/// header and a ring buffer's two halves without assembling them in a buffer first.
struct BlobChunk {
	const void* data;
	size_t size;
};

/// A streaming write of one blob (BlobStore::open_writer(), issue #43): the content arrives piece by
/// piece (a download) and is never held as a whole. append() writes to a temporary place that no
/// reader sees (LittleFS and POSIX: the dot file `.<name>.tmp`, which visit() skips); commit()
/// replaces the blob atomically, exactly as write() does; discard() drops what was written and keeps
/// the previous content. The writer belongs to the store; it is valid from open_writer() until
/// commit() or discard(), one of which the caller must call (also after a failed append()).
class BlobWriter {
public:
	/// Appends `size` bytes. A failure (no_space when the store is full, storage_error) leaves the
	/// writer open for discard().
	virtual BlobStatus append(const void* data, size_t size) = 0;

	/// Replaces the blob by what was appended and ends the writer, whatever the outcome; on a failure
	/// the previous content stays.
	virtual BlobStatus commit() = 0;

	/// Drops what was appended and ends the writer; the previous content stays.
	virtual void discard() = 0;

	/// Bytes appended so far.
	virtual size_t written() const = 0;

protected:
	~BlobWriter() = default;
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
/// - Blobs written with write() are at most kMaxBlobSize bytes; a larger write is refused
///   (too_large) before anything changes. A streamed blob (open_writer()) is bounded only by the
///   free space. A full store refuses with no_space and keeps the previous content.
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

	/// Opens a streaming write of the blob `name` (BlobWriter) for content of `expected` bytes (0:
	/// not known in advance). Refuses (nullptr, `status`) before anything is written: an invalid
	/// name, `expected` above the free space (no_space), a writer open already (busy, one at a time
	/// per store), a store without streaming (unsupported: the default). Meanwhile the blob keeps its
	/// previous content for readers; a write() or remove() of the same name is not allowed until the
	/// writer ended.
	virtual BlobWriter* open_writer(const char* name, size_t expected, BlobStatus& status) {
		(void)name;
		(void)expected;
		status = BlobStatus::unsupported;
		return nullptr;
	}

	/// Overwrites bytes of the existing blob `name` from `offset` with the concatenation of `count`
	/// chunks, in place (issue #56: the history's archive segments, patched slot by slot). The blob
	/// keeps its size: `offset` plus the chunks must end within it (too_large otherwise, nothing
	/// changed). Atomic like write(): after a power cut the blob holds the complete old or the
	/// complete new content (LittleFS: the file is copied on write and committed when it is closed;
	/// POSIX: a temporary copy renamed over the blob). not_found without the blob; unsupported (the
	/// default) for a store that cannot patch.
	virtual BlobStatus patch(const char* name, size_t offset, const BlobChunk* chunks, size_t count) {
		(void)name;
		(void)offset;
		(void)chunks;
		(void)count;
		return BlobStatus::unsupported;
	}

	/// The bytes a new blob can take now; false if the store cannot tell (the default).
	virtual bool free_space(size_t& bytes) const {
		(void)bytes;
		return false;
	}

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
