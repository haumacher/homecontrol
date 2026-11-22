/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_BLOB_QUEUE_H
#define HC_BLOB_QUEUE_H

#include <cstddef>
#include <cstdint>

#include "hc/event_loop.h"
#include "hc/platform/blob_store.h"
#include "hc/platform/clock.h"
#include "hc/platform/worker.h"
#include "hc/shutdown.h"

namespace hc {

/// The sizes of a BlobQueue, the platform's choice (allocated once, at construction).
struct BlobQueueLimits {
	/// Bytes of the copies of queued writes and patches.
	size_t arena_bytes = 8192;
	/// Changes waiting at the same time (writes, patches, removals).
	size_t entries = 24;
	/// A failed write is tried again after this long ...
	uint32_t retry_ms = 30000;
	/// ... at most this often in all, then dropped (logged).
	uint8_t attempts = 5;
};

/// The flash writes of the event loop, off the loop (homecontrol #101, doc/persistence.md "Writing
/// off the loop"): a BlobStore in front of the platform's store that the loop's writers (the value
/// store, the history and its archives, the blocks' managed state, the inhabitant's timelines, the
/// language packs' removals) use instead of it. A write, a patch or a removal is copied into the
/// queue's arena and returns at once; a Worker of its own performs them on the store one after the
/// other, in the order they came, while the loop goes on. On the C6 a blob write is a LittleFS
/// write with its erase (26 to 182 ms measured on the house controller); the loop no longer waits
/// for it.
///
/// - Read your writes: read(), size() and visit() answer from the queued changes where there are
///   any (the latest write or removal of a blob, the patches after it overlaid), from the store
///   otherwise; so a writer reads what it wrote at once, as before.
/// - A write or removal replaces the changes of the same blob that wait (not the one in flight):
///   the store sees only the latest content. A patch of a blob whose write waits goes into the copy.
/// - Failures: a change the store refuses is tried again after `retry_ms` (later changes of other
///   blobs go first, those of the same blob wait behind it), at most `attempts` times, then dropped;
///   each is logged (one error per failure streak, the drop always). What the writer gets at once
///   is the validation the store would do (name, size, a patch's range): an `ok` means accepted.
/// - When the arena or the entries are full, or a change does not fit at all, the change goes to
///   the store at once, on the loop, as without the queue (logged once per streak as a warning,
///   counted in direct()); not possible while a change of the same blob is in flight or (patch)
///   waits: then `busy`, which the writers treat as a failed write and retry.
/// - A write_from() (homecontrol #111: the Zigbee device database) takes no copy: it waits as the
///   source, which the queue asks for the content on the loop when the worker is about to write it,
///   into a buffer of exactly its size that lives until the worker is done (never more than one per
///   blob, the arena untouched). A later write_from() of the same blob while one waits replaces it at
///   no cost; a failed write asks the source again at its retry. A read, size or visit of such a blob
///   asks the source too (a read into a temporary buffer). forget_source() takes the content of the
///   source's waiting writes at once, into such a buffer each.
/// - Without a worker (nullptr: tests, a platform without one) every call goes straight through.
/// - The streaming write (open_writer()) and the measurements (free_space(), usage(),
///   footprint()) go straight to the store; open_writer() is refused (busy) while a change of the
///   same blob waits.
/// - A planned restart (ShutdownParticipant) waits until the queue is empty, at most the
///   Shutdown's kMaxWaitMs.
///
/// Threading: every call on the event loop's thread; the worker's job touches only the one change
/// in flight (its copy, which nothing changes meanwhile) and the store. The store sees calls from
/// the loop (reads, the direct writes) and the worker at the same time, on different blobs except
/// a read beside a patch of the same blob, which the store answers with the old or the new content
/// (doc/persistence.md "Threads"); the read overlays the patch, so it sees the new.
class BlobQueue : public BlobStore, public ShutdownParticipant {
public:
	BlobQueue(EventLoop& loop, const Clock& clock, BlobStore& store, Worker* worker,
		const BlobQueueLimits& limits = BlobQueueLimits());
	~BlobQueue();
	BlobQueue(const BlobQueue&) = delete;
	BlobQueue& operator=(const BlobQueue&) = delete;

	// BlobStore
	BlobStatus write(const char* name, const BlobChunk* chunks, size_t count) override;
	BlobStatus read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const override;
	BlobStatus size(const char* name, size_t& size) const override;
	BlobStatus remove(const char* name) override;
	void visit(BlobVisitor& visitor) const override;
	BlobWriter* open_writer(const char* name, size_t expected, BlobStatus& status) override;
	BlobStatus patch(const char* name, size_t offset, const BlobChunk* chunks, size_t count) override;
	BlobStatus write_from(const char* name, const BlobSource& source) override;
	void forget_source(const BlobSource& source) override;
	bool free_space(size_t& bytes) const override { return store_.free_space(bytes); }
	bool usage(uint64_t& total, uint64_t& used) const override { return store_.usage(total, used); }
	size_t footprint(size_t bytes) const override { return store_.footprint(bytes); }

	// ShutdownParticipant
	const char* shutdown_name() const override { return "flash writes"; }
	void shutdown_begin(Shutdown& shutdown) override;

	/// Changes waiting or in flight.
	size_t pending() const { return count_; }
	/// Whether nothing waits and nothing is in flight.
	bool idle() const { return count_ == 0; }
	/// Bytes of the arena the waiting changes hold.
	size_t arena_used() const;
	/// Whether the queue had its memory (false: every call goes straight through, logged at start).
	bool allocated() const { return entries_ != nullptr; }

	/// Changes the worker performed (successful or not), since construction.
	uint32_t performed() const { return performed_; }
	/// Changes that went to the store on the loop because the queue was full.
	uint32_t direct() const { return direct_; }
	/// Changes dropped after `attempts` failures.
	uint32_t dropped() const { return dropped_; }
	/// The longest a change took on the worker (ms), since construction.
	uint32_t longest_ms() const { return longest_ms_; }

private:
	/// `source`: a write_from(), its content produced when the worker takes it (homecontrol #111).
	enum class Op : uint8_t { write, patch, remove, source };

	struct Entry {
		char name[kMaxNameLength + 1];
		Op op;
		bool in_flight;
		uint8_t attempts;
		/// Order of arrival (FIFO across the table).
		uint32_t sequence;
		union {
			size_t offset; ///< A patch's offset.
			/// A write_from()'s source (Op::source; nullptr once forgotten: `content` holds it).
			const BlobSource* source;
		};
		union {
			size_t data; ///< Offset of the copy in the arena.
			/// The produced content of an Op::source (nullptr: not produced), on the heap.
			uint8_t* content;
		};
		size_t size;   ///< Bytes of the copy (of `content` for an Op::source).
		uint64_t not_before_ms; ///< A retry waits until then.
	};

	/// What the queued changes make of a blob: `kind` none (the store's content counts), present
	/// (a queued write: `base`) or absent (a queued removal).
	struct View {
		enum Kind : uint8_t { store, written, removed } kind = store;
		const Entry* base = nullptr;
	};

	View view(const char* name) const;
	/// The bytes of the content a write or write_from() entry gives the blob.
	size_t content_size(const Entry& entry) const;
	/// Copies up to `size` bytes of that content from `offset` (`length` the bytes copied).
	BlobStatus copy_content(const Entry& entry, size_t offset, void* buffer, size_t size, size_t& length) const;
	/// Asks an Op::source entry's source for its content (on the loop); false (logged) if it cannot.
	bool produce(Entry& entry);
	/// The end of a change on the store (or of a production that failed): released, retried later
	/// or dropped.
	void finish(Entry& entry, BlobStatus status);
	Entry* find_in_flight(const char* name);
	bool waits(const char* name) const;
	/// Drops the waiting (not in flight) changes of `name`.
	void drop_waiting(const char* name);
	Entry* add(const char* name, Op op, size_t bytes);
	bool allocate(size_t bytes, size_t& offset) const;
	void release(Entry& entry);
	void kick();
	void retry_due();
	static void job(void* self);
	void done();
	void full_warning(const char* name, const char* what);
	void check_shutdown();

	EventLoop& loop_;
	const Clock& clock_;
	BlobStore& store_;
	Worker* worker_;
	BlobQueueLimits limits_;
	Entry* entries_ = nullptr;
	bool* used_ = nullptr;
	uint8_t* arena_ = nullptr;
	size_t count_ = 0;
	uint32_t next_sequence_ = 0;
	/// The change on the worker (nullptr: none); job_status_ is the worker's answer.
	Entry* job_ = nullptr;
	BlobStatus job_status_ = BlobStatus::ok;
	uint32_t job_ms_ = 0;
	TimerHandle retry_;
	Shutdown* shutdown_ = nullptr;
	bool failure_logged_ = false;
	bool full_logged_ = false;
	bool start_failed_ = false;
	uint32_t performed_ = 0;
	uint32_t direct_ = 0;
	uint32_t dropped_ = 0;
	uint32_t longest_ms_ = 0;
};

} // namespace hc

#endif // HC_BLOB_QUEUE_H
