/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/file_blob_store.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "hc/log.h"

namespace hc {
namespace posix {

namespace {

constexpr const char* kTag = "hc.blobs";
constexpr const char* kTemporarySuffix = ".tmp";

/// The status of a failed file call; logs the cause unless it is a plain refusal.
BlobStatus failure(int error, const char* what, const std::string& path) {
	if (error == ENOSPC || error == EDQUOT) {
		Log::warn(kTag, "%s %s: no space left", what, path.c_str());
		return BlobStatus::no_space;
	}
	Log::error(kTag, "%s %s: %s", what, path.c_str(), std::strerror(error));
	return BlobStatus::storage_error;
}

bool is_temporary(const char* name) {
	size_t length = std::strlen(name);
	size_t suffix = std::strlen(kTemporarySuffix);
	return name[0] == '.' && length > suffix + 1 && std::strcmp(name + length - suffix, kTemporarySuffix) == 0;
}

/// Writes all of `size` bytes, retrying short writes and interrupts.
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
		p += n;
		size -= static_cast<size_t>(n);
	}
	return true;
}

} // namespace

FileBlobStore::FileBlobStore(std::string directory) : directory_(std::move(directory)) {
	while (directory_.size() > 1 && directory_.back() == '/') {
		directory_.pop_back();
	}
	if (directory_.empty()) {
		directory_ = ".";
	}
}

std::string FileBlobStore::directory_for_settings(const std::string& settings_path) {
	static const std::string kSettings = ".settings";
	std::string base = settings_path;
	size_t slash = base.find_last_of('/');
	size_t stem = slash == std::string::npos ? 0 : slash + 1;
	if (base.size() > stem + kSettings.size() &&
		base.compare(base.size() - kSettings.size(), kSettings.size(), kSettings) == 0) {
		base.resize(base.size() - kSettings.size());
	}
	return base + ".state";
}

bool FileBlobStore::start() {
	return ready();
}

bool FileBlobStore::ready() const {
	if (state_ == State::ready) {
		return true;
	}
	if (state_ == State::failed) {
		return false;
	}
	state_ = State::failed;
	if (::mkdir(directory_.c_str(), 0700) != 0 && errno != EEXIST) {
		Log::error(kTag, "blob store unusable: cannot create the directory %s: %s; values and history are not saved",
			directory_.c_str(), std::strerror(errno));
		return false;
	}
	DIR* dir = ::opendir(directory_.c_str());
	if (dir == nullptr) {
		Log::error(kTag, "blob store unusable: cannot open the directory %s: %s; values and history are not saved",
			directory_.c_str(), std::strerror(errno));
		return false;
	}
	unsigned blobs = 0;
	unsigned stale = 0;
	while (dirent* entry = ::readdir(dir)) {
		if (is_temporary(entry->d_name)) {
			std::string stale_path = directory_ + "/" + entry->d_name;
			if (::unlink(stale_path.c_str()) == 0) {
				++stale;
			} else {
				Log::warn(kTag, "cannot delete the unfinished write %s: %s", stale_path.c_str(), std::strerror(errno));
			}
		} else if (check_name(entry->d_name) == BlobStatus::ok) {
			++blobs;
		}
	}
	::closedir(dir);
	state_ = State::ready;
	if (stale > 0) {
		Log::warn(kTag, "deleted %u unfinished write(s) in %s (interrupted by a crash or power loss; the previous "
			"contents are kept)", stale, directory_.c_str());
		sync_directory();
	}
	Log::info(kTag, "blob store %s: %u blob(s)", directory_.c_str(), blobs);
	return true;
}

std::string FileBlobStore::path(const char* name) const {
	return directory_ + "/" + name;
}

std::string FileBlobStore::temporary_path(const char* name) const {
	return directory_ + "/." + name + kTemporarySuffix;
}

bool FileBlobStore::sync_directory() const {
	int fd = ::open(directory_.c_str(), O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		Log::error(kTag, "cannot open the directory %s to sync it: %s", directory_.c_str(), std::strerror(errno));
		return false;
	}
	bool ok = ::fsync(fd) == 0;
	if (!ok) {
		Log::error(kTag, "cannot sync the directory %s: %s", directory_.c_str(), std::strerror(errno));
	}
	::close(fd);
	return ok;
}

BlobStatus FileBlobStore::write(const char* name, const BlobChunk* chunks, size_t count) {
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
		Log::warn(kTag, "write of %s refused: %zu bytes, at most %zu allowed", name, total, kMaxBlobSize);
		return BlobStatus::too_large;
	}
	if (!ready()) {
		return BlobStatus::storage_error;
	}
	std::string temporary = temporary_path(name);
	int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
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
		::unlink(temporary.c_str());
		return failure(error, "cannot write", temporary);
	}
	std::string target = path(name);
	if (std::rename(temporary.c_str(), target.c_str()) != 0) {
		error = errno;
		::unlink(temporary.c_str());
		return failure(error, "cannot replace", target);
	}
	if (!sync_directory()) {
		return BlobStatus::storage_error;
	}
	return BlobStatus::ok;
}

BlobStatus FileBlobStore::read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const {
	length = 0;
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		Log::warn(kTag, "read refused: blob name invalid");
		return status;
	}
	if (!ready()) {
		return BlobStatus::storage_error;
	}
	std::string file = path(name);
	int fd = ::open(file.c_str(), O_RDONLY);
	if (fd < 0) {
		return errno == ENOENT ? BlobStatus::not_found : failure(errno, "cannot open", file);
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
			return failure(error, "cannot read", file);
		}
		if (n == 0) {
			break;
		}
		length += static_cast<size_t>(n);
	}
	::close(fd);
	return BlobStatus::ok;
}

BlobStatus FileBlobStore::size(const char* name, size_t& size) const {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		Log::warn(kTag, "size refused: blob name invalid");
		return status;
	}
	if (!ready()) {
		return BlobStatus::storage_error;
	}
	std::string file = path(name);
	struct stat info;
	if (::stat(file.c_str(), &info) != 0) {
		return errno == ENOENT ? BlobStatus::not_found : failure(errno, "cannot stat", file);
	}
	if (!S_ISREG(info.st_mode)) {
		Log::error(kTag, "%s is not a file", file.c_str());
		return BlobStatus::storage_error;
	}
	size = static_cast<size_t>(info.st_size);
	return BlobStatus::ok;
}

BlobStatus FileBlobStore::remove(const char* name) {
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		Log::warn(kTag, "remove refused: blob name invalid");
		return status;
	}
	if (!ready()) {
		return BlobStatus::storage_error;
	}
	std::string file = path(name);
	if (::unlink(file.c_str()) != 0) {
		return errno == ENOENT ? BlobStatus::not_found : failure(errno, "cannot delete", file);
	}
	return sync_directory() ? BlobStatus::ok : BlobStatus::storage_error;
}

void FileBlobStore::visit(BlobVisitor& visitor) const {
	if (!ready()) {
		return;
	}
	DIR* dir = ::opendir(directory_.c_str());
	if (dir == nullptr) {
		Log::error(kTag, "cannot list the directory %s: %s", directory_.c_str(), std::strerror(errno));
		return;
	}
	while (dirent* entry = ::readdir(dir)) {
		if (check_name(entry->d_name) != BlobStatus::ok) {
			continue; // ".", "..", temporary files, foreign files
		}
		struct stat info;
		std::string file = path(entry->d_name);
		if (::stat(file.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
			visitor.blob(entry->d_name, static_cast<size_t>(info.st_size));
		}
	}
	::closedir(dir);
}

} // namespace posix
} // namespace hc
