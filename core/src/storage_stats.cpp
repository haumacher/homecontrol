/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/storage_stats.h"

#include "hc/log.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.storage";

struct Counter : BlobVisitor {
	void blob(const char*, size_t size) override {
		++files;
		bytes += size;
	}
	uint32_t files = 0;
	uint64_t bytes = 0;
};

} // namespace

void StorageStats::measure(Figures& out) const {
	Figures f;
	// Read before the walk: a change while it runs is measured again.
	f.changes = store_.changes();
	f.has_usage = store_.usage(f.total, f.used);
	if (f.has_usage) {
		f.has_free = true;
		f.free = f.total > f.used ? static_cast<size_t>(f.total - f.used) : 0;
	} else {
		f.has_free = store_.free_space(f.free);
	}
	Counter counter;
	store_.visit(counter);
	f.files = counter.files;
	f.bytes = counter.bytes;
	f.measured = true;
	out = f;
}

void StorageStats::measure_now() {
	if (measuring_) {
		return; // the worker's measurement ends in done()
	}
	measure(figures_);
	++measurements_;
	Log::info(kTag, "storage figures: %u blob(s), %llu B; %s%llu of %llu B used; measured again off the loop "
		"after each change%s", static_cast<unsigned>(figures_.files), static_cast<unsigned long long>(figures_.bytes),
		figures_.has_usage ? "" : "(unknown) ", static_cast<unsigned long long>(figures_.used),
		static_cast<unsigned long long>(figures_.total), worker_ != nullptr ? "" : " (no worker: on the loop)");
}

void StorageStats::refresh() {
	if (measuring_ || (figures_.measured && figures_.changes == store_.changes())) {
		return;
	}
	if (worker_ == nullptr) {
		measure(figures_);
		++measurements_;
		return;
	}
	const char* error = nullptr;
	if (!worker_->run(Callback{&StorageStats::job, this}, Callback::member<StorageStats, &StorageStats::done>(this),
			&error)) {
		if (!start_failed_) {
			Log::warn(kTag, "the storage figures are not measured again (%s): the status shows the last ones",
				error != nullptr ? error : "the worker refused");
			start_failed_ = true;
		}
		return;
	}
	start_failed_ = false;
	measuring_ = true;
}

const StorageStats::Figures& StorageStats::figures() {
	refresh();
	return figures_;
}

void StorageStats::job(void* self) {
	StorageStats* stats = static_cast<StorageStats*>(self);
	stats->measure(stats->pending_);
}

void StorageStats::done() {
	figures_ = pending_;
	measuring_ = false;
	++measurements_;
	Log::debug(kTag, "storage figures measured again (%u): %u blob(s), %llu B, %llu B used",
		static_cast<unsigned>(measurements_), static_cast<unsigned>(figures_.files),
		static_cast<unsigned long long>(figures_.bytes), static_cast<unsigned long long>(figures_.used));
}

} // namespace hc
