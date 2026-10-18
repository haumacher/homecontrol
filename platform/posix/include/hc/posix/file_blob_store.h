/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_FILE_BLOB_STORE_H
#define HC_POSIX_FILE_BLOB_STORE_H

#include <string>

#include "hc/platform/blob_store.h"

namespace hc {
namespace posix {

/// The BlobStore on files: one file per blob in a directory of its own (doc/persistence.md).
///
/// write() writes `.<name>.tmp` (a dot name, which no blob can have), fsyncs it, renames it over
/// `<name>` and fsyncs the directory, so the replace is atomic and durable when it returns ok. The
/// directory is opened on the first call (or by start()): created if missing (mode 0700, the parent
/// must exist), and temporary files a crash left behind are deleted. If that fails, every call
/// refuses with storage_error; the cause is logged once. Files in the directory whose names break
/// the name rule are ignored by visit(). A streaming write (open_writer()) appends to the same temporary
/// file and commit() finishes it as write() does (fsync, rename, fsync the directory).
class FileBlobStore : public BlobStore {
public:
	/// `directory` without a trailing slash is fine; nothing is touched before the first call.
	explicit FileBlobStore(std::string directory);

	/// The default directory for a settings file: its path with the extension `.settings` replaced
	/// by `.state` (`homecontrol.settings` -> `homecontrol.state`), or `.state` appended.
	static std::string directory_for_settings(const std::string& settings_path);

	/// Opens the directory now (otherwise the first call does): creates it, deletes stale temporary
	/// files, logs the blob count. Returns false (logged) if the store is unusable.
	bool start();

	const std::string& directory() const { return directory_; }

	BlobStatus write(const char* name, const BlobChunk* chunks, size_t count) override;
	BlobStatus read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const override;
	BlobStatus size(const char* name, size_t& size) const override;
	BlobStatus remove(const char* name) override;
	void visit(BlobVisitor& visitor) const override;
	BlobWriter* open_writer(const char* name, size_t expected, BlobStatus& status) override;
	/// The file system's space available to this process (statvfs).
	bool free_space(size_t& bytes) const override;

private:
	class Writer : public BlobWriter {
	public:
		explicit Writer(FileBlobStore& store) : store_(store) {}
		~Writer();

		BlobStatus append(const void* data, size_t size) override;
		BlobStatus commit() override;
		void discard() override;
		size_t written() const override { return written_; }

		bool begin(const char* name);
		bool is_open() const { return fd_ >= 0; }

	private:
		FileBlobStore& store_;
		int fd_ = -1;
		std::string name_;
		size_t written_ = 0;
	};

	enum class State { closed, ready, failed };

	bool ready() const;
	std::string path(const char* name) const;
	std::string temporary_path(const char* name) const;
	bool sync_directory() const;

	std::string directory_;
	mutable State state_ = State::closed;
	Writer writer_{*this};
};

} // namespace posix
} // namespace hc

#endif // HC_POSIX_FILE_BLOB_STORE_H
