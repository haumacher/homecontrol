/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_STORAGE_STATS_H
#define HC_STORAGE_STATS_H

#include <cstddef>
#include <cstdint>

#include "hc/platform/blob_store.h"
#include "hc/platform/worker.h"

namespace hc {

/// The storage figures the polled endpoints report (homecontrol #74): the file system's size, use
/// and free space and the blob store's files and bytes, for GET /api/status (`storage`, the image's
/// `storable`), the System page (GET /api/system/diagnostics) and the E-Home link's status.
///
/// On the C6 each figure is a walk of the LittleFS partition: esp_littlefs_info() follows every
/// file's blocks, and the blob visit fetches the directory's metadata log twice per file (stat and
/// its mtime attribute). Measured on the board with 8 blobs: 49 to 124 ms per request, more the
/// longer the directory's log has grown since its last compaction; a host model of the partition
/// reads 70 to 185 KB of flash for one visit. The figures change only when the store does, so they
/// are measured once at start and again after a change (BlobStore::changes()), off the loop on a
/// Worker of their own; a request reads the last measurement. A change shows with the next
/// measurement, a poll or two later (a measurement takes as long as one request took before).
///
/// Without a worker (tests, a platform without one) the measurement runs within figures(), but only
/// after a change. A worker that cannot start a job leaves the old figures until the next request
/// retries (logged once per failure streak).
///
/// The store's free space for anything that must be exact (the free space a streaming write
/// checks, the archives' check at a load) is still asked from the store itself.
///
/// Every call is made on the event loop's thread; the measurement on the worker touches only the
/// store (read-only calls, which the BlobStore allows beside the loop's) and its own result.
class StorageStats {
public:
	struct Figures {
		/// False until the first measurement.
		bool measured = false;
		/// BlobStore::usage(): the file system's size and use.
		bool has_usage = false;
		uint64_t total = 0;
		uint64_t used = 0;
		/// BlobStore::free_space() (total - used when has_usage).
		bool has_free = false;
		size_t free = 0;
		/// BlobStore::visit(): the blobs and their bytes.
		uint32_t files = 0;
		uint64_t bytes = 0;
		/// BlobStore::changes() when the measurement began.
		uint32_t changes = 0;
	};

	explicit StorageStats(BlobStore& store) : store_(store) {}
	StorageStats(const StorageStats&) = delete;
	StorageStats& operator=(const StorageStats&) = delete;

	/// The worker the measurements run on (nullptr: within figures()). It must be a worker of its
	/// own: the image's store needs its worker when a deploy arrives.
	void set_worker(Worker* worker) { worker_ = worker; }

	/// Measures now, on the calling thread (at start, after the store opened).
	void measure_now();

	/// The last measurement. When the store changed since it began, a new one starts on the worker
	/// and these figures stay until it is done; without a worker it runs here.
	const Figures& figures();

	/// Starts a measurement when the store changed since the last one began (after a store or a
	/// removal of the image, so the next poll sees it), without returning figures.
	void refresh();

	/// Whether a measurement runs on the worker.
	bool measuring() const { return measuring_; }
	/// Completed measurements since construction (for the tests and `storage status`).
	uint32_t measurements() const { return measurements_; }

private:
	static void job(void* self);
	void done();
	void measure(Figures& out) const;

	BlobStore& store_;
	Worker* worker_ = nullptr;
	Figures figures_;
	/// What the worker's job writes; the loop reads it in done() only.
	Figures pending_;
	bool measuring_ = false;
	bool start_failed_ = false;
	uint32_t measurements_ = 0;
};

} // namespace hc

#endif // HC_STORAGE_STATS_H
