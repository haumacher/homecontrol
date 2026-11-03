/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_LITTLEFS_BLOB_STORE_H
#define HC_ESP_LITTLEFS_BLOB_STORE_H

#include "console.h"
#include "hc/platform/blob_store.h"

namespace hc {
namespace esp {

/// The BlobStore on the LittleFS partition `storage` (doc/persistence.md): one file per blob in
/// `/storage/state`.
///
/// write() writes `.<name>.tmp` (a dot name, which no blob can have), syncs and closes it (LittleFS
/// commits the file then) and renames it over `<name>`; LittleFS's rename is atomic, so a power cut
/// leaves the old or the new blob, and at worst an orphaned temporary file, which start() deletes.
/// start() (or the first call) mounts the partition through storage_mount() and creates the
/// directory; if either fails, every call refuses with storage_error, the cause logged once. A
/// streaming write (open_writer(), issue #43: the language packs) appends to the same temporary file and
/// commit() finishes it as write() does; the file is never visible under the blob's name before.
///
/// The core calls it on the event loop, the `storage` console command runs there too; the image
/// store's write of a deployed image runs in the worker task beside it (homecontrol #68): calls on
/// different blobs may overlap. Every call keeps its paths on the stack and esp_littlefs serializes
/// the file system's calls under its lock; the streaming writer is used by the loop alone.
class LittleFsBlobStore : public BlobStore {
public:
	static constexpr const char* kDirectory = "/storage/state";

	LittleFsBlobStore();

	/// Mounts the partition (once, shared with the web server), creates the directory, deletes
	/// unfinished writes and logs the blob count and the free space. Call on the event loop before
	/// the value store and the history load; returns false (logged) if the store is unusable.
	bool start();

	BlobStatus write(const char* name, const BlobChunk* chunks, size_t count) override;
	BlobStatus read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const override;
	BlobStatus size(const char* name, size_t& size) const override;
	BlobStatus remove(const char* name) override;
	void visit(BlobVisitor& visitor) const override;
	BlobWriter* open_writer(const char* name, size_t expected, BlobStatus& status) override;
	/// In place: LittleFS copies the touched block on write and commits the file when it is closed,
	/// so a power cut before the close leaves the old content.
	BlobStatus patch(const char* name, size_t offset, const BlobChunk* chunks, size_t count) override;
	/// The partition's free bytes (esp_littlefs_info).
	bool free_space(size_t& bytes) const override;

	/// The `storage` console command (ConsoleContext::extensions): `storage status`.
	const ConsoleCommand* console_commands() const { return &console_command_; }
	size_t console_command_count() const { return 1; }

private:
	enum class State { closed, ready, failed };

	class Writer : public BlobWriter {
	public:
		BlobStatus append(const void* data, size_t size) override;
		BlobStatus commit() override;
		void discard() override;
		size_t written() const override { return written_; }

		bool begin(const char* name);
		bool is_open() const { return fd_ >= 0; }

	private:
		int fd_ = -1;
		char name_[kMaxNameLength + 1] = {};
		size_t written_ = 0;
	};

	/// `/storage/state/` + a temporary prefix and suffix + a name + NUL.
	static constexpr size_t kMaxPath = 64;

	bool ready() const;
	void print_status() const;
	static int console_storage(void* context, int argc, char** argv);

	mutable State state_ = State::closed;
	ConsoleCommand console_command_;
	Writer writer_;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_LITTLEFS_BLOB_STORE_H
