/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/blob_queue.h"

#include <cstring>
#include <new>

#include "hc/log.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.blobq";

/// After the worker could not start a job (no memory for its task).
constexpr uint32_t kStartRetryMs = 1000;

const char* op_name(uint8_t op) {
	switch (op) {
		case 0: return "write";
		case 1: return "patch";
		default: return "remove";
	}
}

size_t total_of(const BlobChunk* chunks, size_t count) {
	size_t total = 0;
	for (size_t i = 0; i < count; ++i) {
		total += chunks[i].size;
	}
	return total;
}

} // namespace

BlobQueue::BlobQueue(EventLoop& loop, const Clock& clock, BlobStore& store, Worker* worker,
	const BlobQueueLimits& limits)
	: loop_(loop), clock_(clock), store_(store), worker_(worker), limits_(limits) {
	if (worker_ == nullptr || limits_.entries == 0) {
		return;
	}
	entries_ = new (std::nothrow) Entry[limits_.entries];
	used_ = new (std::nothrow) bool[limits_.entries * 2];
	arena_ = limits_.arena_bytes > 0 ? new (std::nothrow) uint8_t[limits_.arena_bytes] : nullptr;
	if (entries_ == nullptr || used_ == nullptr || (limits_.arena_bytes > 0 && arena_ == nullptr)) {
		Log::error(kTag, "no memory for the write queue (%u bytes, %u changes): the flash is written on the loop",
			static_cast<unsigned>(limits_.arena_bytes), static_cast<unsigned>(limits_.entries));
		delete[] entries_;
		delete[] used_;
		delete[] arena_;
		entries_ = nullptr;
		used_ = nullptr;
		arena_ = nullptr;
		return;
	}
	for (size_t i = 0; i < limits_.entries * 2; ++i) {
		used_[i] = false; // [0, entries): the slot is taken; [entries, 2 entries): visit()'s marks
	}
}

BlobQueue::~BlobQueue() {
	loop_.cancel(retry_);
	delete[] entries_;
	delete[] used_;
	delete[] arena_;
}

// ---- the view of the queued changes ----

BlobQueue::View BlobQueue::view(const char* name) const {
	View v;
	for (size_t i = 0; i < limits_.entries && entries_ != nullptr; ++i) {
		if (!used_[i] || std::strcmp(entries_[i].name, name) != 0 || entries_[i].op == Op::patch) {
			continue;
		}
		if (v.base == nullptr || entries_[i].sequence > v.base->sequence) {
			v.base = &entries_[i];
			v.kind = entries_[i].op == Op::write ? View::written : View::removed;
		}
	}
	return v;
}

BlobQueue::Entry* BlobQueue::find_in_flight(const char* name) {
	if (job_ != nullptr && std::strcmp(job_->name, name) == 0) {
		return job_;
	}
	return nullptr;
}

bool BlobQueue::waits(const char* name) const {
	for (size_t i = 0; i < limits_.entries && entries_ != nullptr; ++i) {
		if (used_[i] && std::strcmp(entries_[i].name, name) == 0) {
			return true;
		}
	}
	return false;
}

void BlobQueue::drop_waiting(const char* name) {
	for (size_t i = 0; i < limits_.entries; ++i) {
		if (used_[i] && !entries_[i].in_flight && std::strcmp(entries_[i].name, name) == 0) {
			release(entries_[i]);
		}
	}
}

size_t BlobQueue::arena_used() const {
	size_t used = 0;
	for (size_t i = 0; i < limits_.entries && entries_ != nullptr; ++i) {
		if (used_[i]) {
			used += entries_[i].size;
		}
	}
	return used;
}

bool BlobQueue::allocate(size_t bytes, size_t& offset) const {
	if (bytes == 0) {
		offset = 0;
		return true;
	}
	if (bytes > limits_.arena_bytes) {
		return false;
	}
	// First fit: the lowest start at which `bytes` overlap no copy. The candidates are 0 and the end
	// of every copy; the table is small (a few dozen entries).
	for (size_t c = 0; c <= limits_.entries; ++c) {
		size_t start = 0;
		if (c < limits_.entries) {
			if (!used_[c] || entries_[c].size == 0) {
				continue;
			}
			start = entries_[c].data + entries_[c].size;
		}
		if (start + bytes > limits_.arena_bytes) {
			continue;
		}
		bool free = true;
		for (size_t i = 0; i < limits_.entries && free; ++i) {
			if (used_[i] && entries_[i].size > 0) {
				size_t a = entries_[i].data;
				size_t b = a + entries_[i].size;
				free = start + bytes <= a || start >= b;
			}
		}
		if (free) {
			offset = start;
			return true;
		}
	}
	return false;
}

BlobQueue::Entry* BlobQueue::add(const char* name, Op op, size_t bytes) {
	if (count_ == limits_.entries) {
		return nullptr;
	}
	size_t offset = 0;
	if (!allocate(bytes, offset)) {
		return nullptr;
	}
	for (size_t i = 0; i < limits_.entries; ++i) {
		if (!used_[i]) {
			Entry& e = entries_[i];
			std::strncpy(e.name, name, sizeof(e.name) - 1);
			e.name[sizeof(e.name) - 1] = '\0';
			e.op = op;
			e.in_flight = false;
			e.attempts = 0;
			e.sequence = next_sequence_++;
			e.offset = 0;
			e.data = offset;
			e.size = bytes;
			e.not_before_ms = 0;
			used_[i] = true;
			++count_;
			return &e;
		}
	}
	return nullptr;
}

void BlobQueue::release(Entry& entry) {
	size_t index = static_cast<size_t>(&entry - entries_);
	if (used_[index]) {
		used_[index] = false;
		--count_;
	}
	entry.size = 0;
	if (count_ == 0) {
		full_logged_ = false;
	}
}

void BlobQueue::full_warning(const char* name, const char* what) {
	++direct_;
	if (!full_logged_) {
		Log::warn(kTag, "write queue full (%u of %u bytes, %u of %u changes): the %s of %s goes to the flash on the loop",
			static_cast<unsigned>(arena_used()), static_cast<unsigned>(limits_.arena_bytes),
			static_cast<unsigned>(count_), static_cast<unsigned>(limits_.entries), what, name);
		full_logged_ = true;
	}
}

// ---- BlobStore ----

BlobStatus BlobQueue::write(const char* name, const BlobChunk* chunks, size_t count) {
	if (entries_ == nullptr) {
		return store_.write(name, chunks, count);
	}
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		Log::warn(kTag, "write refused: blob name invalid");
		return status;
	}
	size_t total = total_of(chunks, count);
	if (total > kMaxBlobSize) {
		Log::warn(kTag, "write of %s refused: %u bytes, at most %u", name, static_cast<unsigned>(total),
			static_cast<unsigned>(kMaxBlobSize));
		return BlobStatus::too_large;
	}
	drop_waiting(name); // replaced by this content
	Entry* e = add(name, Op::write, total);
	if (e == nullptr) {
		if (find_in_flight(name) != nullptr) {
			Log::warn(kTag, "write of %s refused for now: queue full and its previous write in flight", name);
			return BlobStatus::busy;
		}
		full_warning(name, "write");
		return store_.write(name, chunks, count);
	}
	uint8_t* p = arena_ + e->data;
	for (size_t i = 0; i < count; ++i) {
		if (chunks[i].size > 0) {
			std::memcpy(p, chunks[i].data, chunks[i].size);
			p += chunks[i].size;
		}
	}
	changed();
	kick();
	return BlobStatus::ok;
}

BlobStatus BlobQueue::read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const {
	length = 0;
	if (entries_ == nullptr) {
		return store_.read(name, offset, buffer, size, length);
	}
	View v = view(name);
	uint32_t after = 0;
	if (v.kind == View::removed) {
		return BlobStatus::not_found;
	}
	if (v.kind == View::written) {
		after = v.base->sequence;
		if (offset < v.base->size) {
			length = v.base->size - offset < size ? v.base->size - offset : size;
			std::memcpy(buffer, arena_ + v.base->data + offset, length);
		}
	} else {
		BlobStatus status = store_.read(name, offset, buffer, size, length);
		if (status != BlobStatus::ok) {
			return status;
		}
	}
	// The patches after the base, oldest first (a later one wins where they overlap).
	bool have_last = false;
	uint32_t last = 0;
	for (;;) {
		const Entry* next = nullptr;
		for (size_t i = 0; i < limits_.entries; ++i) {
			const Entry& e = entries_[i];
			if (!used_[i] || e.op != Op::patch || std::strcmp(e.name, name) != 0) {
				continue;
			}
			if ((v.kind == View::written && e.sequence < after) || (have_last && e.sequence <= last)) {
				continue;
			}
			if (next == nullptr || e.sequence < next->sequence) {
				next = &e;
			}
		}
		if (next == nullptr) {
			break;
		}
		have_last = true;
		last = next->sequence;
		size_t a = next->offset > offset ? next->offset : offset;
		size_t b = next->offset + next->size < offset + length ? next->offset + next->size : offset + length;
		if (a < b) {
			std::memcpy(static_cast<uint8_t*>(buffer) + (a - offset), arena_ + next->data + (a - next->offset), b - a);
		}
	}
	return BlobStatus::ok;
}

BlobStatus BlobQueue::size(const char* name, size_t& size) const {
	if (entries_ == nullptr) {
		return store_.size(name, size);
	}
	View v = view(name);
	if (v.kind == View::removed) {
		return BlobStatus::not_found;
	}
	if (v.kind == View::written) {
		size = v.base->size;
		return BlobStatus::ok;
	}
	return store_.size(name, size);
}

BlobStatus BlobQueue::remove(const char* name) {
	if (entries_ == nullptr) {
		return store_.remove(name);
	}
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		return status;
	}
	size_t ignored = 0;
	status = size(name, ignored);
	if (status != BlobStatus::ok) {
		return status; // not found (in the queue's view), or the store cannot tell
	}
	drop_waiting(name);
	Entry* e = add(name, Op::remove, 0);
	if (e == nullptr) {
		if (find_in_flight(name) != nullptr) {
			Log::warn(kTag, "removal of %s refused for now: queue full and a change of it in flight", name);
			return BlobStatus::busy;
		}
		full_warning(name, "removal");
		return store_.remove(name);
	}
	changed();
	kick();
	return BlobStatus::ok;
}

void BlobQueue::visit(BlobVisitor& visitor) const {
	if (entries_ == nullptr) {
		store_.visit(visitor);
		return;
	}
	bool* seen = used_ + limits_.entries;
	for (size_t i = 0; i < limits_.entries; ++i) {
		seen[i] = false;
	}
	struct Merge : BlobVisitor {
		const BlobQueue& queue;
		BlobVisitor& out;
		bool* seen;
		Merge(const BlobQueue& q, BlobVisitor& o, bool* s) : queue(q), out(o), seen(s) {}
		void blob(const char* name, size_t size) override {
			View v = queue.view(name);
			if (v.kind == View::removed) {
				return;
			}
			if (v.kind == View::written) {
				seen[v.base - queue.entries_] = true;
				out.blob(name, v.base->size);
				return;
			}
			out.blob(name, size);
		}
	} merge(*this, visitor, seen);
	store_.visit(merge);
	// The queued writes of blobs the store does not have yet.
	for (size_t i = 0; i < limits_.entries; ++i) {
		if (!used_[i] || entries_[i].op != Op::write || seen[i]) {
			continue;
		}
		View v = view(entries_[i].name);
		if (v.base == &entries_[i]) {
			visitor.blob(entries_[i].name, entries_[i].size);
		}
	}
}

BlobWriter* BlobQueue::open_writer(const char* name, size_t expected, BlobStatus& status) {
	if (entries_ != nullptr && waits(name)) {
		Log::warn(kTag, "streaming write of %s refused for now: a change of it waits to be written", name);
		status = BlobStatus::busy;
		return nullptr;
	}
	return store_.open_writer(name, expected, status);
}

BlobStatus BlobQueue::patch(const char* name, size_t offset, const BlobChunk* chunks, size_t count) {
	if (entries_ == nullptr) {
		return store_.patch(name, offset, chunks, count);
	}
	BlobStatus status = check_name(name);
	if (status != BlobStatus::ok) {
		Log::warn(kTag, "patch refused: blob name invalid");
		return status;
	}
	size_t current = 0;
	status = size(name, current);
	if (status != BlobStatus::ok) {
		return status;
	}
	size_t total = total_of(chunks, count);
	if (offset > current || total > current - offset) {
		Log::warn(kTag, "patch of %s refused: %u bytes at %u, the blob has %u", name, static_cast<unsigned>(total),
			static_cast<unsigned>(offset), static_cast<unsigned>(current));
		return BlobStatus::too_large;
	}
	// Into the copy of a waiting write that nothing follows: the store sees one write.
	View v = view(name);
	Entry* target = nullptr;
	if (v.kind == View::written && !v.base->in_flight) {
		target = const_cast<Entry*>(v.base);
		for (size_t i = 0; i < limits_.entries; ++i) {
			if (used_[i] && entries_[i].op == Op::patch && entries_[i].sequence > target->sequence &&
				std::strcmp(entries_[i].name, name) == 0) {
				target = nullptr;
				break;
			}
		}
	}
	if (target == nullptr) {
		target = add(name, Op::patch, total);
		if (target == nullptr) {
			if (waits(name)) {
				Log::warn(kTag, "patch of %s refused for now: queue full and earlier changes of it wait", name);
				return BlobStatus::busy;
			}
			full_warning(name, "patch");
			return store_.patch(name, offset, chunks, count);
		}
		target->offset = offset;
		offset = 0; // the copy holds exactly the patch
	}
	uint8_t* p = arena_ + target->data + offset;
	for (size_t i = 0; i < count; ++i) {
		if (chunks[i].size > 0) {
			std::memcpy(p, chunks[i].data, chunks[i].size);
			p += chunks[i].size;
		}
	}
	changed();
	kick();
	return BlobStatus::ok;
}

// ---- the worker ----

void BlobQueue::kick() {
	if (worker_ == nullptr || entries_ == nullptr || job_ != nullptr || count_ == 0) {
		check_shutdown();
		return;
	}
	uint64_t now = clock_.monotonic_ms();
	bool hurry = shutdown_ != nullptr; // a planned restart does not wait for the retry time
	Entry* next = nullptr;
	uint64_t earliest = UINT64_MAX;
	for (size_t i = 0; i < limits_.entries; ++i) {
		if (!used_[i]) {
			continue;
		}
		Entry& e = entries_[i];
		// The oldest change of its blob only: the changes of one blob reach the store in order.
		bool first = true;
		for (size_t j = 0; j < limits_.entries && first; ++j) {
			first = !(used_[j] && entries_[j].sequence < e.sequence && std::strcmp(entries_[j].name, e.name) == 0);
		}
		if (!first) {
			continue;
		}
		if (!hurry && e.not_before_ms > now) {
			if (e.not_before_ms < earliest) {
				earliest = e.not_before_ms;
			}
			continue;
		}
		if (next == nullptr || e.sequence < next->sequence) {
			next = &e;
		}
	}
	if (next == nullptr) {
		if (earliest != UINT64_MAX && !retry_.valid()) {
			retry_ = loop_.schedule_at(earliest, Callback::member<BlobQueue, &BlobQueue::retry_due>(this, "blob_queue.retry"));
		}
		return;
	}
	job_ = next;
	next->in_flight = true;
	++next->attempts;
	const char* error = nullptr;
	if (!worker_->run(Callback{&BlobQueue::job, this, "blob_queue.job"},
			Callback::member<BlobQueue, &BlobQueue::done>(this, "blob_queue.done"), &error)) {
		next->in_flight = false;
		--next->attempts;
		job_ = nullptr;
		if (!start_failed_) {
			Log::error(kTag, "cannot start writing %s off the loop (%s); trying again in %u ms", next->name,
				error != nullptr ? error : "?", static_cast<unsigned>(kStartRetryMs));
			start_failed_ = true;
		}
		if (!retry_.valid()) {
			retry_ = loop_.schedule_at(now + kStartRetryMs,
				Callback::member<BlobQueue, &BlobQueue::retry_due>(this, "blob_queue.retry"));
		}
		return;
	}
	if (start_failed_) {
		Log::info(kTag, "writing off the loop works again");
		start_failed_ = false;
	}
}

void BlobQueue::retry_due() {
	retry_ = TimerHandle();
	kick();
}

void BlobQueue::job(void* context) {
	// The worker's thread: only the change in flight and the store.
	BlobQueue* self = static_cast<BlobQueue*>(context);
	Entry& e = *self->job_;
	uint64_t start = self->clock_.monotonic_ms();
	BlobChunk chunk{self->arena_ + e.data, e.size};
	switch (e.op) {
		case Op::write:
			self->job_status_ = self->store_.write(e.name, &chunk, 1);
			break;
		case Op::patch:
			self->job_status_ = self->store_.patch(e.name, e.offset, &chunk, 1);
			break;
		case Op::remove:
			self->job_status_ = self->store_.remove(e.name);
			if (self->job_status_ == BlobStatus::not_found) {
				self->job_status_ = BlobStatus::ok; // gone is gone
			}
			break;
	}
	self->job_ms_ = static_cast<uint32_t>(self->clock_.monotonic_ms() - start);
}

void BlobQueue::done() {
	Entry& e = *job_;
	job_ = nullptr;
	e.in_flight = false;
	++performed_;
	if (job_ms_ > longest_ms_) {
		longest_ms_ = job_ms_;
	}
	BlobStatus status = job_status_;
	if (status == BlobStatus::ok) {
		if (failure_logged_) {
			Log::info(kTag, "writing the flash works again (%s of %s)", op_name(static_cast<uint8_t>(e.op)), e.name);
			failure_logged_ = false;
		}
		release(e);
	} else {
		// A later write or removal of the blob replaces what failed.
		View v = view(e.name);
		bool replaced = v.base != nullptr && v.base->sequence > e.sequence;
		if (replaced) {
			release(e);
		} else if (e.attempts >= limits_.attempts) {
			Log::error(kTag, "%s of %s dropped after %u attempts: %s", op_name(static_cast<uint8_t>(e.op)), e.name,
				static_cast<unsigned>(e.attempts), blob_status_name(status));
			++dropped_;
			release(e);
		} else {
			if (!failure_logged_) {
				Log::error(kTag, "cannot %s %s: %s; trying again in %u s", op_name(static_cast<uint8_t>(e.op)), e.name,
					blob_status_name(status), static_cast<unsigned>(limits_.retry_ms / 1000));
				failure_logged_ = true;
			}
			e.not_before_ms = clock_.monotonic_ms() + limits_.retry_ms;
		}
	}
	kick();
	check_shutdown();
}

void BlobQueue::shutdown_begin(Shutdown& shutdown) {
	shutdown_ = &shutdown;
	kick();
	check_shutdown();
}

void BlobQueue::check_shutdown() {
	if (shutdown_ != nullptr && count_ == 0) {
		Shutdown* s = shutdown_;
		shutdown_ = nullptr;
		s->done(*this);
	}
}

} // namespace hc
