/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_NVS_BLOB_STORE_H
#define HC_ESP_NVS_BLOB_STORE_H

#include <cstddef>
#include <cstdint>

#include "hc/platform/blob_store.h"
#include "nvs.h"

namespace hc {
namespace esp {

/// hc::BlobStore over an NVS partition of its own (doc/zigbee.md: the zb_db partition holds the
/// Zigbee device database until the LittleFS BlobStore of doc/persistence.md exists). Each blob is
/// one NVS blob, its key a hash of the name ("b" and 8 hex digits: NVS keys have at most 15
/// characters), its value the name with its NUL followed by the content, so that visit() can
/// report names and a hash collision is detected (refused as a storage error). nvs_set_blob plus
/// nvs_commit replace a blob atomically: after a power cut NVS holds the old or the new version.
///
/// Threading: the event loop's thread, like every BlobStore.
class NvsBlobStore : public BlobStore {
public:
	/// Opens (and on first use formats) the partition `label` with the namespace `name_space`.
	/// False (logged) when the partition is missing or unusable: every call then refuses.
	bool open(const char* label, const char* name_space);

	bool is_open() const { return handle_ != 0; }

	BlobStatus write(const char* name, const BlobChunk* chunks, size_t count) override;
	BlobStatus read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const override;
	BlobStatus size(const char* name, size_t& size) const override;
	BlobStatus remove(const char* name) override;
	void visit(BlobVisitor& visitor) const override;

private:
	/// Reads the whole stored value of `name` into a new buffer (caller deletes). The stored name
	/// is checked.
	BlobStatus load(const char* name, uint8_t*& data, size_t& total) const;

	nvs_handle_t handle_ = 0;
	char label_[16] = {};
	char name_space_[16] = {};
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_NVS_BLOB_STORE_H
