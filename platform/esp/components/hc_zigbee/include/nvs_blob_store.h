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
/// Zigbee device database). NVS keys have at most 15 characters, so a blob's keys are a letter and
/// the FNV-1a hash of its name in 8 hex digits: `n<hash>` holds the name (a string, written once:
/// visit() reports it and a hash collision is detected, refused as a storage error), `d<hash>` the
/// content, written straight from the caller's buffer (no copy for a write of one chunk) and read
/// straight into the caller's buffer when it takes the whole blob. The layout before (until the
/// memory review of issue #44) kept the name with its NUL and the content in one blob `b<hash>`;
/// such a blob is still read, and the next write replaces it (content key first, then the legacy
/// key erased: a power cut between leaves both, and the content key wins). nvs_set_blob plus
/// nvs_commit replace the content atomically: after a power cut NVS holds the old or the new version.
/// An empty blob keeps the legacy layout (NVS blobs are never empty).
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
	/// Reads the whole legacy value of `name` (`b<hash>`) into a new buffer (caller deletes). The
	/// stored name is checked.
	BlobStatus load_legacy(const char* name, uint8_t*& data, size_t& total) const;
	/// Whether the name key belonging to `data_key` names `name`: ok, not_found (no name key yet),
	/// storage_error (another name: a hash collision, logged).
	BlobStatus check_owner(const char* name, const char* data_key) const;

	nvs_handle_t handle_ = 0;
	char label_[16] = {};
	char name_space_[16] = {};
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_NVS_BLOB_STORE_H
