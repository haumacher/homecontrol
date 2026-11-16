/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_FILE_IMAGE_SLOT_H
#define HC_POSIX_FILE_IMAGE_SLOT_H

#include <string>

#include "hc/image/memory_image_slot.h"

namespace hc::posix {

/// The POSIX build's image slot (spec/runtime-image.md 7): the file `<settings stem>.image.bin` beside
/// the settings (and the blobs' directory `<stem>.state`), 512 KB at most, read into one heap block at start; every write and erase goes to the
/// file as well. A missing file is an erased slot.
class FileImageSlot : public image::MemoryImageSlot {
public:
	explicit FileImageSlot(std::string path, size_t size = kDefaultSize);

	/// `<stem>.image.bin` for the settings file `<stem>.settings`.
	static std::string path_for_settings(const std::string& settings_path);

	const std::string& path() const { return path_; }

protected:
	void on_write(size_t offset, size_t length) override;

private:
	std::string path_;
};

} // namespace hc::posix

#endif // HC_POSIX_FILE_IMAGE_SLOT_H
