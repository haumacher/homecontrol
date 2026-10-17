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
/// directory; if either fails, every call refuses with storage_error, the cause logged once.
///
/// Not thread-safe, like every BlobStore: the core calls it on the event loop, the `storage`
/// console command runs there too.
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

	/// The `storage` console command (ConsoleContext::extensions): `storage status`.
	const ConsoleCommand* console_commands() const { return &console_command_; }
	size_t console_command_count() const { return 1; }

private:
	enum class State { closed, ready, failed };

	/// `/storage/state/` + a temporary prefix and suffix + a name + NUL.
	static constexpr size_t kMaxPath = 64;

	bool ready() const;
	void print_status() const;
	static int console_storage(void* context, int argc, char** argv);

	mutable State state_ = State::closed;
	ConsoleCommand console_command_;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_LITTLEFS_BLOB_STORE_H
