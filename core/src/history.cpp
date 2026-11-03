/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/history.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "history_internal.h"
#include "hc/log.h"
#include "hc/value_store.h"

namespace hc {

using history_detail::get_u32;
using history_detail::kTag;
using history_detail::put_u32;
using history_detail::sampled_type;
using history_detail::to_bits;
using history_detail::to_double;

namespace {

constexpr const char* kPrefix = "h.";
constexpr uint8_t kMagic0 = 'H';
constexpr uint8_t kMagic1 = 'H';

/// Header fields (tag, length, data).
constexpr uint8_t kFieldName = 1;
constexpr uint8_t kFieldType = 2;
constexpr uint8_t kFieldSpacing = 3;
constexpr uint8_t kFieldRetention = 4;
constexpr uint8_t kFieldSampleSize = 5;
constexpr uint8_t kFieldCount = 6;

/// Magic (2), version (1), header length (2).
constexpr size_t kPrefixSize = 5;
/// The largest header this firmware reads (a newer one may carry more fields).
constexpr size_t kMaxHeaderRead = 256;

} // namespace

const char* history_status_name(HistoryStatus status) {
	switch (status) {
		case HistoryStatus::ok: return "ok";
		case HistoryStatus::malformed: return "malformed";
		case HistoryStatus::unknown_datapoint: return "unknown datapoint";
		case HistoryStatus::type_error: return "type error";
		case HistoryStatus::flag_mismatch: return "flag mismatch";
		case HistoryStatus::budget_exceeded: return "budget exceeded";
		case HistoryStatus::out_of_memory: return "out of memory";
		case HistoryStatus::busy: return "busy (called from a datapoint listener)";
		case HistoryStatus::capacity: return "capacity exhausted";
	}
	return "?";
}

History::History(EventLoop& loop, const Clock& clock, DatapointModel& model, BlobStore& store,
	const HistoryLimits& limits, const TimeZone* zone)
	: loop_(loop), clock_(clock), model_(model), store_(store), limits_(limits), zone_(zone) {}

History::~History() {
	if (tick_.valid()) {
		loop_.cancel(tick_);
	}
	if (save_timer_.valid()) {
		loop_.cancel(save_timer_);
	}
	if (listening_) {
		model_.remove_listener(*this);
	}
	free_rings();
}

void History::free_rings() {
	delete[] rings_;
	delete[] samples_;
	delete[] names_;
	delete[] archives_;
	delete[] pending_;
	rings_ = nullptr;
	samples_ = nullptr;
	names_ = nullptr;
	archives_ = nullptr;
	pending_ = nullptr;
	count_ = 0;
	sample_bytes_ = 0;
	ram_bytes_ = 0;
	flash_bytes_ = 0;
}

uint32_t History::ring_samples(uint32_t retention_s, uint32_t spacing_s) {
	if (spacing_s == 0) {
		return 0;
	}
	uint64_t samples = (static_cast<uint64_t>(retention_s) + spacing_s - 1) / spacing_s + 1;
	return samples > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(samples);
}

size_t History::archive_ram_bytes(uint32_t resolution_s) {
	if (resolution_s == 0) {
		return 0;
	}
	// The open slot, and the pending queue of one kPendingS plus the slot closing meanwhile.
	return (static_cast<size_t>((kPendingS + resolution_s - 1) / resolution_s) + 2) * sizeof(ArchiveSlot);
}

uint32_t History::archive_segments(uint32_t retention_s, uint32_t resolution_s) {
	uint32_t slots = ring_samples(retention_s, resolution_s);
	return static_cast<uint32_t>((static_cast<uint64_t>(slots) + kSlotsPerSegment - 1) / kSlotsPerSegment);
}

size_t History::ram_bytes(const HistorySpec& spec) {
	size_t bytes = ring_bytes(spec.retention_s, spec.spacing_s);
	for (uint8_t i = 0; spec.archives != nullptr && i < spec.archive_count; ++i) {
		bytes += archive_ram_bytes(spec.archives[i].resolution_s);
	}
	return bytes;
}

size_t History::flash_bytes(const HistorySpec& spec) {
	size_t bytes = 0;
	for (uint8_t i = 0; spec.archives != nullptr && i < spec.archive_count; ++i) {
		bytes += archive_flash_bytes(spec.archives[i].retention_s, spec.archives[i].resolution_s);
	}
	return bytes;
}

HistoryStatus History::validate(const HistorySpec* specs, size_t count, size_t budget_bytes,
	size_t archive_budget_bytes, size_t& ram, size_t& flash, size_t& archives, size_t& pending) const {
	if (budget_bytes > limits_.cap_bytes) {
		Log::error(kTag, "history refused: the image asks for %u bytes of RAM, the device allows at most %u",
			static_cast<unsigned>(budget_bytes), static_cast<unsigned>(limits_.cap_bytes));
		return HistoryStatus::budget_exceeded;
	}
	if (archive_budget_bytes > limits_.archive_cap_bytes) {
		Log::error(kTag, "history refused: the image asks for %u bytes of flash for archives, the device allows at most %u",
			static_cast<unsigned>(archive_budget_bytes), static_cast<unsigned>(limits_.archive_cap_bytes));
		return HistoryStatus::budget_exceeded;
	}
	if (specs == nullptr && count > 0) {
		Log::error(kTag, "history refused: %u specs missing", static_cast<unsigned>(count));
		return HistoryStatus::malformed;
	}
	ram = 0;
	flash = 0;
	archives = 0;
	pending = 0;
	for (size_t i = 0; i < count; ++i) {
		const HistorySpec& spec = specs[i];
		const Datapoint* dp = model_.get(spec.datapoint);
		if (dp == nullptr) {
			Log::error(kTag, "history refused: spec %u names datapoint %u, the model has %u",
				static_cast<unsigned>(i), spec.datapoint, static_cast<unsigned>(model_.size()));
			return HistoryStatus::unknown_datapoint;
		}
		if (!dp->has_history()) {
			Log::error(kTag, "history refused: '%s' has a history spec but no history flag", dp->name());
			return HistoryStatus::flag_mismatch;
		}
		if (!sampled_type(dp->type())) {
			Log::error(kTag, "history refused: '%s' is %s (history needs boolean, unsigned, signed or real)",
				dp->name(), value_type_name(dp->type()));
			return HistoryStatus::type_error;
		}
		if (spec.spacing_s < kMinSpacingS || spec.spacing_s > kMaxSpacingS) {
			Log::error(kTag, "history refused: '%s' spacing %u s outside %u..%u s", dp->name(),
				static_cast<unsigned>(spec.spacing_s), static_cast<unsigned>(kMinSpacingS),
				static_cast<unsigned>(kMaxSpacingS));
			return HistoryStatus::malformed;
		}
		if (spec.retention_s < spec.spacing_s || spec.retention_s > kMaxRetentionS) {
			Log::error(kTag, "history refused: '%s' retention %u s outside %u..%u s", dp->name(),
				static_cast<unsigned>(spec.retention_s), static_cast<unsigned>(spec.spacing_s),
				static_cast<unsigned>(kMaxRetentionS));
			return HistoryStatus::malformed;
		}
		uint32_t samples = ring_samples(spec.retention_s, spec.spacing_s);
		if (samples > kMaxSamples) {
			Log::error(kTag, "history refused: '%s' needs %u samples (%u s every %u s), at most %u", dp->name(),
				static_cast<unsigned>(samples), static_cast<unsigned>(spec.retention_s),
				static_cast<unsigned>(spec.spacing_s), static_cast<unsigned>(kMaxSamples));
			return HistoryStatus::malformed;
		}
		for (size_t j = 0; j < i; ++j) {
			if (specs[j].datapoint == spec.datapoint) {
				Log::error(kTag, "history refused: '%s' has two history specs (%u and %u)", dp->name(),
					static_cast<unsigned>(j), static_cast<unsigned>(i));
				return HistoryStatus::malformed;
			}
		}
		if (spec.archive_count > kMaxArchives || (spec.archive_count > 0 && spec.archives == nullptr)) {
			Log::error(kTag, "history refused: '%s' has %u archives, at most %u", dp->name(),
				static_cast<unsigned>(spec.archive_count), static_cast<unsigned>(kMaxArchives));
			return HistoryStatus::malformed;
		}
		for (uint8_t k = 0; k < spec.archive_count; ++k) {
			const ArchiveSpec& a = spec.archives[k];
			if (a.resolution_s < kMinResolutionS || a.resolution_s > kMaxResolutionS || a.resolution_s <= spec.spacing_s) {
				Log::error(kTag, "history refused: '%s' archive %u: resolution %u s outside %u..%u s or not above the spacing (%u s)",
					dp->name(), static_cast<unsigned>(k), static_cast<unsigned>(a.resolution_s),
					static_cast<unsigned>(kMinResolutionS), static_cast<unsigned>(kMaxResolutionS),
					static_cast<unsigned>(spec.spacing_s));
				return HistoryStatus::malformed;
			}
			if (a.retention_s < a.resolution_s || a.retention_s > kMaxArchiveRetentionS) {
				Log::error(kTag, "history refused: '%s' archive %u: retention %u s outside %u..%u s", dp->name(),
					static_cast<unsigned>(k), static_cast<unsigned>(a.retention_s), static_cast<unsigned>(a.resolution_s),
					static_cast<unsigned>(kMaxArchiveRetentionS));
				return HistoryStatus::malformed;
			}
			uint32_t slots = ring_samples(a.retention_s, a.resolution_s);
			if (slots > kMaxSlots) {
				Log::error(kTag, "history refused: '%s' archive %u needs %u slots (%u s every %u s), at most %u", dp->name(),
					static_cast<unsigned>(k), static_cast<unsigned>(slots), static_cast<unsigned>(a.retention_s),
					static_cast<unsigned>(a.resolution_s), static_cast<unsigned>(kMaxSlots));
				return HistoryStatus::malformed;
			}
			for (uint8_t m = 0; m < k; ++m) {
				if (spec.archives[m].resolution_s == a.resolution_s) {
					Log::error(kTag, "history refused: '%s' has two archives of %u s", dp->name(),
						static_cast<unsigned>(a.resolution_s));
					return HistoryStatus::malformed;
				}
			}
			pending += (archive_ram_bytes(a.resolution_s) / sizeof(ArchiveSlot)) - 1;
		}
		archives += spec.archive_count;
		ram += ram_bytes(spec);
		flash += flash_bytes(spec);
	}
	// Every flagged datapoint has a spec (the specs are unique and all flagged, so count them).
	size_t flagged = 0;
	for (size_t i = 0; i < model_.size(); ++i) {
		flagged += model_.get(static_cast<uint16_t>(i))->has_history() ? 1 : 0;
	}
	if (flagged != count) {
		for (size_t i = 0; i < model_.size(); ++i) {
			const Datapoint* dp = model_.get(static_cast<uint16_t>(i));
			bool specified = false;
			for (size_t j = 0; j < count && !specified; ++j) {
				specified = specs[j].datapoint == dp->id();
			}
			if (dp->has_history() && !specified) {
				Log::error(kTag, "history refused: '%s' has the history flag but no spec (retention, spacing)",
					dp->name());
				break;
			}
		}
		return HistoryStatus::flag_mismatch;
	}
	if (ram > budget_bytes) {
		Log::error(kTag, "history refused: the rings and archives need %u bytes of RAM, the image's budget is %u",
			static_cast<unsigned>(ram), static_cast<unsigned>(budget_bytes));
		return HistoryStatus::budget_exceeded;
	}
	if (flash > archive_budget_bytes) {
		Log::error(kTag, "history refused: the archives need %u bytes of flash, the image's archive budget is %u",
			static_cast<unsigned>(flash), static_cast<unsigned>(archive_budget_bytes));
		return HistoryStatus::budget_exceeded;
	}
	return HistoryStatus::ok;
}

HistoryStatus History::start(uint32_t save_interval_s) {
	uint64_t now = clock_.monotonic_ms();
	Callback save = Callback::member<History, &History::flush>(this);
	if (listening_) {
		if (save_interval_s != save_interval_s_) {
			// Re-armed with the new interval (the slot just freed is taken again: cannot fail).
			loop_.cancel(save_timer_);
			save_timer_ = loop_.schedule_periodic(now + save_interval_s * 1000ull, save_interval_s * 1000, save);
			save_interval_s_ = save_interval_s;
		}
		return HistoryStatus::ok;
	}
	if (model_.add_listener(*this) != DatapointStatus::ok) {
		Log::error(kTag, "history disabled: cannot listen to the model");
		return HistoryStatus::capacity;
	}
	listening_ = true;
	tick_ = loop_.schedule_periodic(now + kTickMs, kTickMs, Callback::member<History, &History::on_tick>(this));
	save_timer_ = loop_.schedule_periodic(now + save_interval_s * 1000ull, save_interval_s * 1000, save);
	save_interval_s_ = save_interval_s;
	if (!tick_.valid() || !save_timer_.valid()) {
		loop_.cancel(tick_);
		loop_.cancel(save_timer_);
		tick_ = TimerHandle();
		save_timer_ = TimerHandle();
		model_.remove_listener(*this);
		listening_ = false;
		Log::error(kTag, "history disabled: no timer for its tick");
		return HistoryStatus::capacity;
	}
	return HistoryStatus::ok;
}

HistoryStatus History::load(const HistorySpec* specs, size_t count, size_t budget_bytes, size_t archive_budget_bytes) {
	if (model_.notifying()) {
		Log::error(kTag, "refusing to load the history from a datapoint listener (post it)");
		return HistoryStatus::busy;
	}
	size_t ram = 0;
	size_t flash = 0;
	size_t archive_count = 0;
	size_t pending_slots = 0;
	HistoryStatus status =
		validate(specs, count, budget_bytes, archive_budget_bytes, ram, flash, archive_count, pending_slots);
	if (status != HistoryStatus::ok) {
		return status;
	}
	size_t bytes = 0;
	for (size_t i = 0; i < count; ++i) {
		bytes += ring_bytes(specs[i].retention_s, specs[i].spacing_s);
	}
	// The save interval keeps the bytes written per hour within the limit: the rings' samples and
	// one segment block per archive and save. Beyond an hour the archives write once an hour anyway
	// (their pending queue holds an hour of slots), so the rings get the rest of the budget, at
	// least a quarter of it (doc/persistence.md "The flash wear budget").
	uint64_t budget = limits_.save_bytes_per_hour;
	uint64_t archive_bytes = static_cast<uint64_t>(archive_count) * kSegmentBytes;
	uint64_t written = static_cast<uint64_t>(bytes) + archive_bytes;
	uint64_t paced = budget > 0 ? (written * 3600 + budget - 1) / budget : kMaxSpacingS;
	if (budget > 0 && archive_count > 0 && paced > kPendingS) {
		uint64_t share = std::max<uint64_t>(budget > archive_bytes ? budget - archive_bytes : 0, (budget + 3) / 4);
		paced = std::max<uint64_t>(kPendingS, (static_cast<uint64_t>(bytes) * 3600 + share - 1) / share);
	}
	uint32_t interval = static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(paced,
		std::max<uint32_t>(limits_.min_save_interval_s, 1)), kMaxSpacingS));
	status = start(interval);
	if (status != HistoryStatus::ok) {
		return status;
	}

	// The previous rings' new samples and archives' slots are saved from their own copies.
	flush();
	free_rings();

	size_t names_bytes = 0;
	for (size_t i = 0; i < count; ++i) {
		names_bytes += std::strlen(model_.get(specs[i].datapoint)->name()) + 1;
	}
	if (count > 0) {
		rings_ = new (std::nothrow) Ring[count];
		samples_ = new (std::nothrow) HistorySample[bytes / sizeof(HistorySample)];
		names_ = new (std::nothrow) char[names_bytes];
		if (archive_count > 0) {
			archives_ = new (std::nothrow) Archive[archive_count];
			pending_ = new (std::nothrow) ArchiveSlot[pending_slots];
		}
		if (rings_ == nullptr || samples_ == nullptr || names_ == nullptr ||
			(archive_count > 0 && (archives_ == nullptr || pending_ == nullptr))) {
			free_rings();
			Log::error(kTag, "history off: no memory for %u rings and %u archives (%u bytes)",
				static_cast<unsigned>(count), static_cast<unsigned>(archive_count),
				static_cast<unsigned>(ram + count * sizeof(Ring) + names_bytes + archive_count * sizeof(Archive)));
			return HistoryStatus::out_of_memory;
		}
	}
	HistorySample* cursor = samples_;
	char* name = names_;
	Archive* archive = archives_;
	ArchiveSlot* queue = pending_;
	for (size_t i = 0; i < count; ++i) {
		const Datapoint* dp = model_.get(specs[i].datapoint);
		Ring& r = rings_[i];
		size_t length = std::strlen(dp->name());
		std::memcpy(name, dp->name(), length + 1);
		r.name = name;
		name += length + 1;
		r.capacity = ring_samples(specs[i].retention_s, specs[i].spacing_s);
		r.samples = cursor;
		cursor += r.capacity;
		r.spacing_s = specs[i].spacing_s;
		r.retention_s = specs[i].retention_s;
		r.id = dp->id();
		r.type = dp->type();
		r.hash = persist::fnv1a(r.name);
		r.archives = specs[i].archive_count > 0 ? archive : nullptr;
		r.archive_count = specs[i].archive_count;
		for (uint8_t k = 0; k < specs[i].archive_count; ++k) {
			Archive& a = *archive++;
			a.resolution_s = specs[i].archives[k].resolution_s;
			a.retention_s = specs[i].archives[k].retention_s;
			a.slots = ring_samples(a.retention_s, a.resolution_s);
			a.segments = static_cast<uint16_t>(archive_segments(a.retention_s, a.resolution_s));
			a.positions = a.segments * kSlotsPerSegment;
			a.days = a.resolution_s % 86400 == 0 ? a.resolution_s / 86400 : 0;
			a.pending = queue;
			a.pending_capacity = static_cast<uint16_t>(archive_ram_bytes(a.resolution_s) / sizeof(ArchiveSlot) - 1);
			queue += a.pending_capacity;
		}
		std::sort(r.archives, r.archives + r.archive_count,
			[](const Archive& x, const Archive& y) { return x.resolution_s < y.resolution_s; });
	}
	count_ = count;
	sample_bytes_ = bytes;
	ram_bytes_ = ram;
	flash_bytes_ = flash;
	// Two names with the same hash would share a blob: the second is not persisted.
	for (size_t i = 0; i < count_; ++i) {
		for (size_t j = 0; j < i; ++j) {
			if (rings_[j].persistable && rings_[j].hash == rings_[i].hash) {
				rings_[i].persistable = false;
				Log::error(kTag, "history of '%s' is not saved: its blob name collides with '%s'", rings_[i].name,
					rings_[j].name);
				break;
			}
		}
	}
	std::sort(rings_, rings_ + count_, [](const Ring& a, const Ring& b) { return a.id < b.id; });

	size_t restored = 0;
	WallTime wall = clock_.wall_time();
	for (size_t i = 0; i < count_; ++i) {
		Ring& r = rings_[i];
		if (r.persistable) {
			restore(r);
			restored += r.count;
		}
		// A swap without a restart: the last sample's value goes on holding from now (the hold up
		// to the swap was written by the flush above). After a restart the last sample is too old.
		if (r.archive_count > 0 && r.count > 0 && wall.synchronized && wall.seconds >= r.back().t &&
			wall.seconds < static_cast<int64_t>(r.back().t) + r.spacing_s + kHoldSlackS &&
			wall.seconds <= static_cast<int64_t>(UINT32_MAX)) {
			r.credit_t = static_cast<uint32_t>(wall.seconds);
		}
	}

	// The persistable rings' names, for the orphan check.
	class RingKeeper : public persist::BlobKeeper {
	public:
		RingKeeper(const Ring* rings, size_t count) : rings_(rings), count_(count) {}
		bool keep(const char* name) const override {
			for (size_t i = 0; i < count_; ++i) {
				char own[persist::kBlobNameSize];
				persist::blob_name(kPrefix, rings_[i].name, own);
				if (rings_[i].persistable && std::strcmp(own, name) == 0) {
					return true;
				}
			}
			return false;
		}

	private:
		const Ring* rings_;
		size_t count_;
	};
	persist::delete_orphans(store_, kPrefix, RingKeeper(rings_, count_), kTag);
	check_archives();
	if (archive_count > 0) {
		Log::info(kTag,
			"history: %u rings, %u archives, %u of %u bytes of RAM, %u of %u bytes of flash, %u samples restored, saved every %u s",
			static_cast<unsigned>(count_), static_cast<unsigned>(archive_count), static_cast<unsigned>(ram),
			static_cast<unsigned>(budget_bytes), static_cast<unsigned>(flash), static_cast<unsigned>(archive_budget_bytes),
			static_cast<unsigned>(restored), static_cast<unsigned>(save_interval_s_));
	} else {
		Log::info(kTag, "history: %u rings, %u of %u sample bytes, %u samples restored, saved every %u s",
			static_cast<unsigned>(count_), static_cast<unsigned>(bytes), static_cast<unsigned>(budget_bytes),
			static_cast<unsigned>(restored), static_cast<unsigned>(save_interval_s_));
	}
	return HistoryStatus::ok;
}

History::Ring* History::ring(uint16_t id) const {
	Ring* begin = rings_;
	Ring* end = rings_ + count_;
	Ring* it = std::lower_bound(begin, end, id, [](const Ring& r, uint16_t key) { return r.id < key; });
	return it != end && it->id == id ? it : nullptr;
}

History::Ring* History::ring_of(const Datapoint& datapoint) const {
	Ring* r = ring(datapoint.id());
	return r != nullptr && std::strcmp(r->name, datapoint.name()) == 0 ? r : nullptr;
}

void History::append(Ring& r, uint32_t t, uint32_t bits) {
	feed(r, t, bits); // before the ring changes: the archives hold the previous sample's value up to t
	// The wall clock went back: samples from the "future" are dropped.
	uint32_t dropped = 0;
	while (r.count > 0 && r.back().t > t) {
		--r.count;
		++dropped;
	}
	if (dropped > 0) {
		Log::warn(kTag, "history of '%s': %u samples dropped, the clock went back", r.name,
			static_cast<unsigned>(dropped));
	}
	if (r.count > 0 && r.back().t == t) {
		r.at(r.count - 1).bits = bits;
	} else {
		if (r.count == r.capacity) {
			r.head = (r.head + 1) % r.capacity;
			--r.count;
		}
		r.at(r.count) = HistorySample{t, bits};
		++r.count;
	}
	while (r.count > 0 && static_cast<int64_t>(r.at(0).t) + r.retention_s < static_cast<int64_t>(t)) {
		r.head = (r.head + 1) % r.capacity;
		--r.count;
	}
	r.dirty = true;
}

namespace {

/// Whether a ring may take a sample at `now`: empty, the clock went back, or spacing elapsed.
bool due_at(uint32_t count, uint32_t last_t, uint32_t spacing_s, int64_t now) {
	return count == 0 || now < last_t || now - last_t >= spacing_s;
}

/// The wall clock as a sample time; false while it is not synchronized or outside uint32.
bool sample_time(const Clock& clock, uint32_t& t) {
	WallTime wall = clock.wall_time();
	if (!wall.synchronized || wall.seconds < 0 || wall.seconds > static_cast<int64_t>(UINT32_MAX)) {
		return false;
	}
	t = static_cast<uint32_t>(wall.seconds);
	return true;
}

} // namespace

void History::datapoint_updated(const DatapointUpdate& update) {
	const Datapoint& dp = update.datapoint;
	if (!dp.has_history()) {
		return;
	}
	Ring* r = ring_of(dp);
	uint32_t now = 0;
	if (r == nullptr || !sample_time(clock_, now)) {
		return;
	}
	if (dp.quality() != Quality::good) {
		// The value is no longer good: the archives hold the last one up to now and no further (a gap).
		settle(*r, now);
		r->credit_t = 0;
		return;
	}
	uint32_t bits = to_bits(dp.value());
	if (r->count > 0 && r->back().bits == bits) {
		return; // Unchanged: the keepalive takes care of it.
	}
	if (due_at(r->count, r->count > 0 ? r->back().t : 0, r->spacing_s, now)) {
		append(*r, now, bits);
	}
}

void History::on_tick() {
	uint32_t now = 0;
	if (!sample_time(clock_, now)) {
		return;
	}
	for (size_t i = 0; i < count_; ++i) {
		Ring& r = rings_[i];
		const Datapoint* dp = model_.get(r.id);
		if (dp != nullptr && dp->quality() == Quality::good && std::strcmp(dp->name(), r.name) == 0 &&
			due_at(r.count, r.count > 0 ? r.back().t : 0, r.spacing_s, now)) {
			append(r, now, to_bits(dp->value()));
		}
		// The archives hold the last value on and close the slots that ended, good value or not.
		settle(r, now);
	}
}

HistoryStatus History::range(uint16_t id, int64_t from, int64_t to, HistoryPoint* out, size_t max_points,
	size_t& count) const {
	count = 0;
	const Ring* r = ring(id);
	const Datapoint* dp = model_.get(id);
	if (r == nullptr || dp == nullptr || std::strcmp(dp->name(), r->name) != 0) {
		return HistoryStatus::unknown_datapoint;
	}
	if (to <= from || max_points == 0 || out == nullptr) {
		return HistoryStatus::malformed;
	}
	// Sample times are uint32 seconds; outside the retention there are none.
	from = std::max<int64_t>(from, 0);
	to = std::min<int64_t>(to, static_cast<int64_t>(UINT32_MAX) + 1);
	WallTime wall = clock_.wall_time();
	if (wall.synchronized) {
		from = std::max(from, wall.seconds - static_cast<int64_t>(r->retention_s));
	}
	if (to <= from) {
		return HistoryStatus::ok;
	}
	// First sample with t >= from and first with t >= to (the ring is sorted by time).
	auto lower = [r](int64_t key) {
		uint32_t lo = 0;
		uint32_t hi = r->count;
		while (lo < hi) {
			uint32_t mid = lo + (hi - lo) / 2;
			if (static_cast<int64_t>(r->at(mid).t) < key) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		return lo;
	};
	uint32_t begin = lower(from);
	uint32_t end = lower(to);
	if (end - begin <= max_points) {
		for (uint32_t i = begin; i < end; ++i) {
			const HistorySample& s = r->at(i);
			double v = to_double(r->type, s.bits);
			out[count++] = HistoryPoint{s.t, s.t, v, v, v, 1, v};
		}
		return HistoryStatus::ok;
	}
	uint64_t span = static_cast<uint64_t>(to - from);
	int64_t width = static_cast<int64_t>((span + max_points - 1) / max_points);
	int64_t bucket = -1;
	for (uint32_t i = begin; i < end; ++i) {
		const HistorySample& s = r->at(i);
		double v = to_double(r->type, s.bits);
		int64_t b = (static_cast<int64_t>(s.t) - from) / width;
		if (b != bucket) {
			if (count > 0) {
				out[count - 1].mean /= out[count - 1].count; // the sum so far
			}
			bucket = b;
			out[count++] = HistoryPoint{from + b * width, s.t, v, v, v, 1, v};
			continue;
		}
		HistoryPoint& p = out[count - 1];
		p.min = std::min(p.min, v);
		p.max = std::max(p.max, v);
		p.last = v;
		p.last_t = s.t;
		p.mean += v;
		++p.count;
	}
	if (count > 0) {
		out[count - 1].mean /= out[count - 1].count;
	}
	return HistoryStatus::ok;
}

size_t History::samples(uint16_t id) const {
	const Ring* r = ring(id);
	return r != nullptr ? r->count : 0;
}

uint32_t History::retention_s(uint16_t id) const {
	const Ring* r = ring(id);
	return r != nullptr ? r->retention_s : 0;
}

void History::flush() {
	for (size_t i = 0; i < count_; ++i) {
		Ring& r = rings_[i];
		if (r.dirty && r.persistable) {
			save(r);
		}
		for (uint8_t k = 0; k < r.archive_count; ++k) {
			Archive& a = r.archives[k];
			if (a.pending_count > 0 || (a.has_open && a.open_dirty)) {
				save_archive(r, a);
			}
		}
	}
}

bool History::save(Ring& r) {
	uint8_t header[kPrefixSize + 2 + Datapoint::kMaxName + 3 + 6 + 6 + 3 + 6];
	uint8_t* p = header + kPrefixSize;
	size_t name_length = std::strlen(r.name);
	*p++ = kFieldName;
	*p++ = static_cast<uint8_t>(name_length);
	std::memcpy(p, r.name, name_length);
	p += name_length;
	*p++ = kFieldType;
	*p++ = 1;
	*p++ = static_cast<uint8_t>(r.type);
	*p++ = kFieldSpacing;
	*p++ = 4;
	put_u32(p, r.spacing_s);
	p += 4;
	*p++ = kFieldRetention;
	*p++ = 4;
	put_u32(p, r.retention_s);
	p += 4;
	*p++ = kFieldSampleSize;
	*p++ = 1;
	*p++ = sizeof(HistorySample);
	*p++ = kFieldCount;
	*p++ = 4;
	put_u32(p, r.count);
	p += 4;
	size_t header_length = static_cast<size_t>(p - header);
	size_t fields = header_length - kPrefixSize;
	header[0] = kMagic0;
	header[1] = kMagic1;
	header[2] = kFormatVersion;
	header[3] = static_cast<uint8_t>(fields);
	header[4] = static_cast<uint8_t>(fields >> 8);

	// The ring's samples in time order: from the head to the end of the storage, then the wrap.
	uint32_t first = std::min(r.count, r.capacity - r.head);
	BlobChunk chunks[4] = {
		{header, header_length},
		{r.samples + r.head, first * sizeof(HistorySample)},
		{r.samples, (r.count - first) * sizeof(HistorySample)},
		{nullptr, 4},
	};
	uint32_t crc = 0;
	for (size_t i = 0; i < 3; ++i) {
		crc = persist::crc32(crc, chunks[i].data, chunks[i].size);
	}
	uint8_t trailer[4];
	put_u32(trailer, crc);
	chunks[3].data = trailer;

	char name[persist::kBlobNameSize];
	persist::blob_name(kPrefix, r.name, name);
	BlobStatus status = store_.write(name, chunks, 4);
	if (status != BlobStatus::ok) {
		if (!failure_logged_) {
			Log::error(kTag, "cannot save the history of '%s' (%s): %s; retrying at the next save", r.name, name,
				blob_status_name(status));
			failure_logged_ = true;
		}
		return false;
	}
	if (failure_logged_) {
		Log::info(kTag, "saving the history works again");
		failure_logged_ = false;
	}
	r.dirty = false;
	++writes_;
	return true;
}

void History::restore(Ring& r) {
	char name[persist::kBlobNameSize];
	persist::blob_name(kPrefix, r.name, name);
	size_t size = 0;
	BlobStatus status = store_.size(name, size);
	if (status == BlobStatus::not_found) {
		return;
	}
	const char* problem = nullptr;
	uint8_t buffer[kMaxHeaderRead];
	size_t length = 0;
	if (status == BlobStatus::ok) {
		status = store_.read(name, 0, buffer, sizeof(buffer), length);
	}
	if (status != BlobStatus::ok) {
		Log::error(kTag, "cannot read the history of '%s' (%s): %s", r.name, name, blob_status_name(status));
		return;
	}
	size_t fields = length >= kPrefixSize ? static_cast<size_t>(buffer[3] | (buffer[4] << 8)) : 0;
	char saved_name[Datapoint::kMaxName + 1] = {};
	uint8_t type = 0;
	uint32_t sample_size = 0;
	uint32_t samples = 0;
	bool has_count = false;
	if (length < kPrefixSize + 4 || buffer[0] != kMagic0 || buffer[1] != kMagic1) {
		problem = "not a history blob";
	} else if (buffer[2] == 0 || buffer[2] > kFormatVersion) {
		problem = buffer[2] == 0 ? "format generation 0" : "written by a newer firmware (format generation unknown)";
	} else if (kPrefixSize + fields > length) {
		problem = "header too large or truncated";
	} else {
		size_t pos = kPrefixSize;
		size_t end = kPrefixSize + fields;
		while (pos < end && problem == nullptr) {
			if (end - pos < 2 || end - pos - 2 < buffer[pos + 1]) {
				problem = "truncated header field";
				break;
			}
			uint8_t tag = buffer[pos];
			size_t n = buffer[pos + 1];
			const uint8_t* field = buffer + pos + 2;
			switch (tag) {
				case kFieldName:
					if (n > Datapoint::kMaxName) {
						problem = "name length";
					} else {
						std::memcpy(saved_name, field, n);
						saved_name[n] = '\0';
					}
					break;
				case kFieldType:
					if (n != 1) {
						problem = "type length";
					} else {
						type = field[0];
					}
					break;
				case kFieldSampleSize:
					if (n != 1) {
						problem = "sample size length";
					} else {
						sample_size = field[0];
					}
					break;
				case kFieldCount:
					if (n != 4) {
						problem = "count length";
					} else {
						samples = get_u32(field);
						has_count = true;
					}
					break;
				default: break; // Spacing, retention (informative) and fields of a newer firmware.
			}
			pos += 2 + n;
		}
		if (problem == nullptr) {
			if (!has_count || sample_size < sizeof(HistorySample)) {
				problem = "no sample count or sample size";
			} else if (size != kPrefixSize + fields + static_cast<uint64_t>(samples) * sample_size + 4) {
				problem = "size does not match the sample count";
			}
		}
	}
	if (problem == nullptr) {
		// The checksum covers everything before it.
		uint32_t crc = 0;
		size_t offset = 0;
		while (offset < size - 4 && status == BlobStatus::ok) {
			size_t chunk = std::min(sizeof(buffer), size - 4 - offset);
			status = store_.read(name, offset, buffer, chunk, length);
			if (status == BlobStatus::ok && length != chunk) {
				status = BlobStatus::storage_error;
			}
			crc = persist::crc32(crc, buffer, chunk);
			offset += chunk;
		}
		if (status == BlobStatus::ok) {
			status = store_.read(name, size - 4, buffer, 4, length);
		}
		if (status != BlobStatus::ok || length != 4) {
			problem = "read failed";
		} else if (get_u32(buffer) != crc) {
			problem = "checksum mismatch";
		}
	}
	if (problem == nullptr && std::strcmp(saved_name, r.name) != 0) {
		Log::error(kTag, "history blob %s belongs to '%s', not '%s': ignored", name, saved_name, r.name);
		return;
	}
	if (problem == nullptr && type != static_cast<uint8_t>(r.type)) {
		Log::warn(kTag, "history of '%s' discarded: the datapoint's type changed", r.name);
		return;
	}
	if (problem != nullptr) {
		Log::error(kTag, "history of '%s' (%s) ignored: %s", r.name, name, problem);
		return;
	}

	// The newest samples that fit, oldest first, straight into the (empty) ring.
	uint32_t take = std::min(samples, r.capacity);
	size_t data = kPrefixSize + fields + static_cast<size_t>(samples - take) * sample_size;
	if (sample_size == sizeof(HistorySample)) {
		status = store_.read(name, data, r.samples, take * sizeof(HistorySample), length);
		if (status == BlobStatus::ok && length != take * sizeof(HistorySample)) {
			status = BlobStatus::storage_error;
		}
	} else {
		for (uint32_t i = 0; i < take && status == BlobStatus::ok; ++i) {
			status = store_.read(name, data + i * sample_size, &r.samples[i], sizeof(HistorySample), length);
			if (status == BlobStatus::ok && length != sizeof(HistorySample)) {
				status = BlobStatus::storage_error;
			}
		}
	}
	if (status != BlobStatus::ok) {
		Log::error(kTag, "cannot read the history of '%s' (%s): %s", r.name, name, blob_status_name(status));
		return;
	}
	// Merge into the series: strictly increasing times, nothing from the future, nothing older than
	// the retention (both only when the wall clock is known).
	WallTime wall = clock_.wall_time();
	uint32_t kept = 0;
	for (uint32_t i = 0; i < take; ++i) {
		const HistorySample s = r.samples[i];
		if (kept > 0 && s.t <= r.samples[kept - 1].t) {
			continue;
		}
		if (wall.synchronized && (static_cast<int64_t>(s.t) > wall.seconds ||
				static_cast<int64_t>(s.t) + r.retention_s < wall.seconds)) {
			continue;
		}
		r.samples[kept++] = s;
	}
	r.head = 0;
	r.count = kept;
}

} // namespace hc
