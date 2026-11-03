/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "littlefs_blob_store.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "hc/log.h"
#include "storage.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.blobs";
constexpr const char* kTemporarySuffix = ".tmp";

/// The status of a failed file call; logs the cause.
BlobStatus failure(int error, const char* what, const char* path) {
	if (error == ENOSPC) {
		Log::warn(kTag, "%s %s: the storage partition is full", what, path);
		return BlobStatus::no_space;
	}
	Log::error(kTag, "%s %s: %s", what, path, std::strerror(error));
	return BlobStatus::storage_error;
}

bool is_temporary(const char* name) {
	size_t length = std::strlen(name);
	size_t suffix = std::strlen(kTemporarySuffix);
	return name[0] == '.' && length > suffix + 1 && std::strcmp(name + length - suffix, kTemporarySuffix) == 0;
}

/// The path of a directory entry; false if it does not fit (a foreign file with a long name).
bool blob_path(char* path, size_t size, const char* name) {
	int n = std::snprintf(path, size, "%s/%s", LittleFsBlobStore::kDirectory, name);
	return n >= 0 && static_cast<size_t>(n) < size;
}

/// Always fits for a name that passed check_name().
void temporary_path(char* path, size_t size, const char* name) {
	if (std::snprintf(path, size, "%s/.%s%s", LittleFsBlobStore::kDirectory, name, kTemporarySuffix) < 0) {
		path[0] = '\0';
	}
}

bool write_all(int fd, const void* data, size_t size) {
	const char* p = static_cast<const char*>(data);
	while (size > 0) {
		ssize_t n = ::write(fd, p, size);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		if (n == 0) {
			errno = EIO;
			return false;
		}
		p += n;
		size -= static_cast<size_t>(n);
	}
	return true;
}

/// Counts of the state directory, for the start log and `storage status`.
struct DirectoryCount {
	unsigned blobs = 0;
	size_t blob_bytes = 0;
	unsigned other = 0;
};

DirectoryCount count_directory() {
	DirectoryCount count;
	DIR* dir = ::opendir(LittleFsBlobStore::kDirectory);
	if (dir == nullptr) {
		return count;
	}
	while (dirent* entry = ::readdir(dir)) {
		if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		char path[96];
		struct stat info;
		if (BlobStore::check_name(entry->d_name) == BlobStatus::ok && blob_path(path, sizeof(path), entry->d_name) &&
			::stat(path, &info) == 0 &&
			S_ISREG(info.st_mode)) {
			++count.blobs;
			count.blob_bytes += static_cast<size_t>(info.st_size);
		} else {
			++count.other;
		}
	}
	::closedir(dir);
	return count;
}

} // namespace

LittleFsBlobStore::LittleFsBlobStore()
	: console_command_{"storage", "status", "the storage partition: mount, free space, the blob store's files",
		  &LittleFsBlobStore::console_storage, this} {}

bool LittleFsBlobStore::start() {
	return ready();
}

bool LittleFsBlobStore::ready() const {
	if (state_ == State::ready) {
		return true;
	}
	if (state_ == State::failed) {
		return false;
	}
	state_ = State::failed;
	if (!storage_mount()) {
		Log::error(kTag, "blob store unusable: %s; values and history are not saved", storage_failure());
		return false;
	}
	if (::mkdir(kDirectory, 0700) != 0 && errno != EEXIST) {
		Log::error(kTag, "blob store unusable: cannot create %s: %s; values and history are not saved", kDirectory,
			std::strerror(errno));
		return false;
	}
	DIR* dir = ::opendir(kDirectory);
	if (dir == nullptr) {
		Log::error(kTag, "blob store unusable: cannot open %s: %s; values and history are not saved", kDirectory,
			std::strerror(errno));
		return false;
	}
	unsigned stale = 0;
	while (dirent* entry = ::readdir(dir)) {
		if (!is_temporary(entry->d_name)) {
			continue;
		}
		char path[96];
		if (!blob_path(path, sizeof(path), entry->d_name)) {
			Log::warn(kTag, "unfinished write with an overlong name in %s ignored", kDirectory);
			continue;
		}
		if (::unlink(path) == 0) {
			++stale;
		} else {
			Log::warn(kTag, "cannot delete the unfinished write %s: %s", path, std::strerror(errno));
		}
	}
	::closedir(dir);
	state_ = State::ready;
	if (stale > 0) {
		Log::warn(kTag, "deleted %u unfinished write(s) in %s (interrupted by a power loss; the previous contents "
			"are kept)", stale, kDirectory);
	}
	DirectoryCount count = count_directory();
	size_t total = 0;
	size_t used = 0;
	storage_usage(total, used);
	Log::info(kTag, "blob store %s: %u blob(s), %u B; %u B free on the partition", kDirectory, count.blobs,
		static_cast<unsigned>(count.blob_bytes), static_cast<unsigned>(total - used));
	return true;
}

BlobStatus LittleFsBlobStore::write(const char* name, const BlobChunk* chunks, size_t count) {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		Log::warn(kTag, "write refused: blob name invalid");
		return status;
	}
	size_t total = 0;
	for (size_t i = 0; i < count; ++i) {
		total += chunks[i].size;
	}
	if (total > kMaxBlobSize) {
		Log::warn(kTag, "write of %s refused: %u bytes, at most %u allowed", name, static_cast<unsigned>(total),
			static_cast<unsigned>(kMaxBlobSize));
		return BlobStatus::too_large;
	}
	if (!ready()) {
		return BlobStatus::storage_error;
	}
	char temporary[kMaxPath];
	temporary_path(temporary, sizeof(temporary), name);
	int fd = ::open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		return failure(errno, "cannot create", temporary);
	}
	bool ok = true;
	for (size_t i = 0; ok && i < count; ++i) {
		ok = write_all(fd, chunks[i].data, chunks[i].size);
	}
	ok = ok && ::fsync(fd) == 0;
	int error = errno;
	if (::close(fd) != 0 && ok) {
		ok = false;
		error = errno;
	}
	if (!ok) {
		::unlink(temporary);
		return failure(error, "cannot write", temporary);
	}
	char target[kMaxPath];
	blob_path(target, sizeof(target), name);
	if (::rename(temporary, target) != 0) {
		error = errno;
		::unlink(temporary);
		return failure(error, "cannot replace", target);
	}
	return BlobStatus::ok;
}

BlobStatus LittleFsBlobStore::read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const {
	length = 0;
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		Log::warn(kTag, "read refused: blob name invalid");
		return status;
	}
	if (!ready()) {
		return BlobStatus::storage_error;
	}
	char path[kMaxPath];
	blob_path(path, sizeof(path), name);
	int fd = ::open(path, O_RDONLY);
	if (fd < 0) {
		return errno == ENOENT ? BlobStatus::not_found : failure(errno, "cannot open", path);
	}
	char* p = static_cast<char*>(buffer);
	while (length < size) {
		ssize_t n = ::pread(fd, p + length, size - length, static_cast<off_t>(offset + length));
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			int error = errno;
			::close(fd);
			length = 0;
			return failure(error, "cannot read", path);
		}
		if (n == 0) {
			break;
		}
		length += static_cast<size_t>(n);
	}
	::close(fd);
	return BlobStatus::ok;
}

BlobStatus LittleFsBlobStore::patch(const char* name, size_t offset, const BlobChunk* chunks, size_t count) {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		Log::warn(kTag, "patch refused: blob name invalid");
		return status;
	}
	if (!ready()) {
		return BlobStatus::storage_error;
	}
	char path[kMaxPath];
	blob_path(path, sizeof(path), name);
	struct stat info;
	if (::stat(path, &info) != 0) {
		return errno == ENOENT ? BlobStatus::not_found : failure(errno, "cannot stat", path);
	}
	size_t current = static_cast<size_t>(info.st_size);
	size_t total = 0;
	for (size_t i = 0; i < count; ++i) {
		total += chunks[i].size;
	}
	if (offset > current || total > current - offset) {
		Log::warn(kTag, "patch of %s refused: %u bytes at %u, the blob has %u", name, static_cast<unsigned>(total),
			static_cast<unsigned>(offset), static_cast<unsigned>(current));
		return BlobStatus::too_large;
	}
	int fd = ::open(path, O_WRONLY);
	if (fd < 0) {
		return failure(errno, "cannot open", path);
	}
	bool ok = ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) == static_cast<off_t>(offset);
	for (size_t i = 0; ok && i < count; ++i) {
		ok = write_all(fd, chunks[i].data, chunks[i].size);
	}
	ok = ok && ::fsync(fd) == 0;
	int error = errno;
	if (::close(fd) != 0 && ok) {
		ok = false;
		error = errno;
	}
	return ok ? BlobStatus::ok : failure(error, "cannot patch", path);
}

BlobStatus LittleFsBlobStore::size(const char* name, size_t& size) const {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		Log::warn(kTag, "size refused: blob name invalid");
		return status;
	}
	if (!ready()) {
		return BlobStatus::storage_error;
	}
	char path[kMaxPath];
	blob_path(path, sizeof(path), name);
	struct stat info;
	if (::stat(path, &info) != 0) {
		return errno == ENOENT ? BlobStatus::not_found : failure(errno, "cannot stat", path);
	}
	if (!S_ISREG(info.st_mode)) {
		Log::error(kTag, "%s is not a file", path);
		return BlobStatus::storage_error;
	}
	size = static_cast<size_t>(info.st_size);
	return BlobStatus::ok;
}

BlobStatus LittleFsBlobStore::remove(const char* name) {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		Log::warn(kTag, "remove refused: blob name invalid");
		return status;
	}
	if (!ready()) {
		return BlobStatus::storage_error;
	}
	char path[kMaxPath];
	blob_path(path, sizeof(path), name);
	if (::unlink(path) != 0) {
		return errno == ENOENT ? BlobStatus::not_found : failure(errno, "cannot delete", path);
	}
	return BlobStatus::ok;
}

void LittleFsBlobStore::visit(BlobVisitor& visitor) const {
	if (!ready()) {
		return;
	}
	DIR* dir = ::opendir(kDirectory);
	if (dir == nullptr) {
		Log::error(kTag, "cannot list %s: %s", kDirectory, std::strerror(errno));
		return;
	}
	while (dirent* entry = ::readdir(dir)) {
		if (check_name(entry->d_name) != BlobStatus::ok) {
			continue; // ".", "..", temporary files, foreign files
		}
		char path[kMaxPath];
		struct stat info;
		if (blob_path(path, sizeof(path), entry->d_name) && ::stat(path, &info) == 0 && S_ISREG(info.st_mode)) {
			visitor.blob(entry->d_name, static_cast<size_t>(info.st_size));
		}
	}
	::closedir(dir);
}

bool LittleFsBlobStore::free_space(size_t& bytes) const {
	size_t total = 0;
	size_t used = 0;
	if (!ready() || !storage_usage(total, used)) {
		return false;
	}
	bytes = total > used ? total - used : 0;
	return true;
}

BlobWriter* LittleFsBlobStore::open_writer(const char* name, size_t expected, BlobStatus& status) {
	status = check_name(name);
	if (status != BlobStatus::ok) {
		Log::warn(kTag, "streaming write refused: blob name invalid");
		return nullptr;
	}
	if (writer_.is_open()) {
		Log::warn(kTag, "streaming write of %s refused: another one is open", name);
		status = BlobStatus::busy;
		return nullptr;
	}
	if (!ready()) {
		status = BlobStatus::storage_error;
		return nullptr;
	}
	size_t free = 0;
	if (expected > 0 && free_space(free) && expected > free) {
		Log::warn(kTag, "streaming write of %s refused: %u bytes, %u free on the partition", name,
			static_cast<unsigned>(expected), static_cast<unsigned>(free));
		status = BlobStatus::no_space;
		return nullptr;
	}
	if (!writer_.begin(name)) {
		char temporary[kMaxPath];
		temporary_path(temporary, sizeof(temporary), name);
		status = failure(errno, "cannot create", temporary);
		return nullptr;
	}
	status = BlobStatus::ok;
	return &writer_;
}

bool LittleFsBlobStore::Writer::begin(const char* name) {
	std::snprintf(name_, sizeof(name_), "%s", name);
	written_ = 0;
	char temporary[kMaxPath];
	temporary_path(temporary, sizeof(temporary), name);
	fd_ = ::open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	return fd_ >= 0;
}

BlobStatus LittleFsBlobStore::Writer::append(const void* data, size_t size) {
	if (fd_ < 0) {
		return BlobStatus::storage_error;
	}
	if (!write_all(fd_, data, size)) {
		char temporary[kMaxPath];
		temporary_path(temporary, sizeof(temporary), name_);
		return failure(errno, "cannot write", temporary);
	}
	written_ += size;
	return BlobStatus::ok;
}

BlobStatus LittleFsBlobStore::Writer::commit() {
	if (fd_ < 0) {
		return BlobStatus::storage_error;
	}
	char temporary[kMaxPath];
	temporary_path(temporary, sizeof(temporary), name_);
	bool ok = ::fsync(fd_) == 0;
	int error = errno;
	if (::close(fd_) != 0 && ok) {
		ok = false;
		error = errno;
	}
	fd_ = -1;
	if (!ok) {
		::unlink(temporary);
		return failure(error, "cannot write", temporary);
	}
	char target[kMaxPath];
	blob_path(target, sizeof(target), name_);
	if (::rename(temporary, target) != 0) {
		error = errno;
		::unlink(temporary);
		return failure(error, "cannot replace", target);
	}
	return BlobStatus::ok;
}

void LittleFsBlobStore::Writer::discard() {
	if (fd_ < 0) {
		return;
	}
	::close(fd_);
	fd_ = -1;
	char temporary[kMaxPath];
	temporary_path(temporary, sizeof(temporary), name_);
	::unlink(temporary);
}

void LittleFsBlobStore::print_status() const {
	if (!storage_mount()) {
		std::printf("partition %s: not mounted: %s\n", kStoragePartition, storage_failure());
		return;
	}
	size_t total = 0;
	size_t used = 0;
	storage_usage(total, used);
	std::printf("partition %s at %s: %u of %u B used, %u B free\n", kStoragePartition, kStorageMountPoint,
		static_cast<unsigned>(used), static_cast<unsigned>(total), static_cast<unsigned>(total - used));
	if (!ready()) {
		std::printf("blobs     unusable (see the log)\n");
		return;
	}
	DirectoryCount count = count_directory();
	std::printf("blobs     %u in %s, %u B\n", count.blobs, kDirectory, static_cast<unsigned>(count.blob_bytes));
	if (count.other > 0) {
		std::printf("other     %u file(s) that are no blobs (ignored)\n", count.other);
	}
}

int LittleFsBlobStore::console_storage(void* context, int argc, char** argv) {
	const LittleFsBlobStore& store = *static_cast<const LittleFsBlobStore*>(context);
	if (argc == 1 || (argc == 2 && std::strcmp(argv[1], "status") == 0)) {
		store.print_status();
		return 0;
	}
	std::printf("usage: storage status\n");
	return 1;
}

} // namespace esp
} // namespace hc
