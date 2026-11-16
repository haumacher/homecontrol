/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/file_image_slot.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "hc/log.h"

namespace hc::posix {

namespace {

constexpr const char* kTag = "hc.slot";

} // namespace

FileImageSlot::FileImageSlot(std::string path, size_t size) : MemoryImageSlot(size), path_(std::move(path)) {
	std::FILE* file = std::fopen(path_.c_str(), "rb");
	if (file == nullptr) {
		Log::info(kTag, "image slot %s: no file yet (an erased slot)", path_.c_str());
		return;
	}
	size_t n = std::fread(bytes(), 1, this->size(), file);
	std::fclose(file);
	Log::info(kTag, "image slot %s: %u bytes read", path_.c_str(), static_cast<unsigned>(n));
}

std::string FileImageSlot::path_for_settings(const std::string& settings_path) {
	static const std::string kSettings = ".settings";
	std::string base = settings_path;
	size_t slash = base.find_last_of('/');
	size_t stem = slash == std::string::npos ? 0 : slash + 1;
	if (base.size() > stem + kSettings.size() &&
		base.compare(base.size() - kSettings.size(), kSettings.size(), kSettings) == 0) {
		base.resize(base.size() - kSettings.size());
	}
	return base + ".image.bin";
}

void FileImageSlot::on_write(size_t offset, size_t length) {
	std::FILE* file = std::fopen(path_.c_str(), "r+b");
	if (file == nullptr) {
		file = std::fopen(path_.c_str(), "w+b");
	}
	if (file == nullptr) {
		Log::error(kTag, "image slot %s cannot be written: %s", path_.c_str(), std::strerror(errno));
		return;
	}
	if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0 ||
		std::fwrite(bytes() + offset, 1, length, file) != length) {
		Log::error(kTag, "image slot %s: write of %u bytes at %u failed", path_.c_str(), static_cast<unsigned>(length),
			static_cast<unsigned>(offset));
	}
	std::fclose(file);
}

} // namespace hc::posix
