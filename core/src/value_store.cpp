/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/value_store.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "hc/log.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.persist";
constexpr const char* kPrefix = "v.";
constexpr uint8_t kMagic0 = 'H';
constexpr uint8_t kMagic1 = 'V';

/// Record fields (tag, length, data).
constexpr uint8_t kFieldName = 1;
constexpr uint8_t kFieldValue = 2;
constexpr uint8_t kFieldWall = 3;

/// A record read from the store may be larger than one this firmware writes (fields of a newer one).
constexpr size_t kMaxRecordRead = 512;

/// The token bucket counts in units of 1/3600000 write: refilling `writes_per_hour` units per
/// millisecond gives `writes_per_hour` writes per hour.
constexpr uint64_t kWriteCost = 3600000;

/// A wall time that is not known.
constexpr int64_t kUnknown = INT64_MIN;

int64_t to_raw(std::optional<int64_t> wall) { return wall ? *wall : kUnknown; }


void put_u16(uint8_t* p, uint16_t v) {
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
}

void put_u32(uint8_t* p, uint32_t v) {
	for (int i = 0; i < 4; ++i) {
		p[i] = static_cast<uint8_t>(v >> (8 * i));
	}
}

void put_u64(uint8_t* p, uint64_t v) {
	for (int i = 0; i < 8; ++i) {
		p[i] = static_cast<uint8_t>(v >> (8 * i));
	}
}

uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t get_u32(const uint8_t* p) {
	uint32_t v = 0;
	for (int i = 3; i >= 0; --i) {
		v = (v << 8) | p[i];
	}
	return v;
}

uint64_t get_u64(const uint8_t* p) {
	uint64_t v = 0;
	for (int i = 7; i >= 0; --i) {
		v = (v << 8) | p[i];
	}
	return v;
}

/// Encodes a value as its type byte followed by the payload; returns the length (at most 1 + 28).
size_t encode_value(const Value& value, uint8_t* out) {
	out[0] = static_cast<uint8_t>(value.type());
	uint8_t* p = out + 1;
	switch (value.type()) {
		case ValueType::none: return 1;
		case ValueType::boolean: p[0] = value.as_bool() ? 1 : 0; return 2;
		case ValueType::unsigned_int: put_u32(p, value.as_unsigned()); return 5;
		case ValueType::signed_int: put_u32(p, static_cast<uint32_t>(value.as_signed())); return 5;
		case ValueType::enumeration: put_u32(p, value.as_enum()); return 5;
		case ValueType::real: {
			double d = value.as_real();
			uint64_t bits = 0;
			std::memcpy(&bits, &d, sizeof(bits));
			put_u64(p, bits);
			return 9;
		}
		case ValueType::string:
			std::memcpy(p, value.as_string(), value.length());
			return 1 + value.length();
		case ValueType::raw:
			if (value.length() > 0) {
				std::memcpy(p, value.raw(), value.length());
			}
			return 1 + value.length();
		case ValueType::time_of_day: {
			TimeOfDay t = value.as_time();
			p[0] = t.day_of_week;
			p[1] = t.hour;
			p[2] = t.minute;
			p[3] = t.second;
			return 5;
		}
		case ValueType::date: {
			Date d = value.as_date();
			put_u16(p, d.year);
			p[2] = d.month;
			p[3] = d.day;
			return 5;
		}
		case ValueType::date_time: {
			DateTime d = value.as_date_time();
			put_u16(p, d.year);
			p[2] = d.month;
			p[3] = d.day;
			p[4] = d.day_of_week;
			p[5] = d.hour;
			p[6] = d.minute;
			p[7] = d.second;
			put_u16(p + 8, d.flags);
			return 11;
		}
		case ValueType::controlled: {
			Controlled c = value.as_controlled();
			p[0] = c.control ? 1 : 0;
			p[1] = c.value;
			return 3;
		}
		case ValueType::color: {
			Color c = value.as_color();
			p[0] = c.red;
			p[1] = c.green;
			p[2] = c.blue;
			p[3] = c.white;
			p[4] = c.valid;
			return 6;
		}
	}
	return 1;
}

/// Decodes encode_value(); false for an unknown type or a wrong payload length.
bool decode_value(const uint8_t* data, size_t length, Value& out) {
	if (length < 1) {
		return false;
	}
	const uint8_t* p = data + 1;
	size_t n = length - 1;
	switch (static_cast<ValueType>(data[0])) {
		case ValueType::boolean:
			if (n != 1 || p[0] > 1) {
				return false;
			}
			out = Value::of_bool(p[0] != 0);
			return true;
		case ValueType::unsigned_int:
			if (n != 4) {
				return false;
			}
			out = Value::of_unsigned(get_u32(p));
			return true;
		case ValueType::signed_int:
			if (n != 4) {
				return false;
			}
			out = Value::of_signed(static_cast<int32_t>(get_u32(p)));
			return true;
		case ValueType::enumeration:
			if (n != 4) {
				return false;
			}
			out = Value::of_enum(get_u32(p));
			return true;
		case ValueType::real: {
			if (n != 8) {
				return false;
			}
			uint64_t bits = get_u64(p);
			double d = 0;
			std::memcpy(&d, &bits, sizeof(d));
			out = Value::of_real(d);
			return !out.empty();
		}
		case ValueType::string: {
			std::optional<Value> v = Value::of_string(reinterpret_cast<const char*>(p), n);
			if (!v) {
				return false;
			}
			out = *v;
			return true;
		}
		case ValueType::raw: {
			std::optional<Value> v = Value::of_raw(p, n);
			if (!v) {
				return false;
			}
			out = *v;
			return true;
		}
		case ValueType::time_of_day:
			if (n != 4) {
				return false;
			}
			out = Value::of_time(TimeOfDay{p[0], p[1], p[2], p[3]});
			return true;
		case ValueType::date:
			if (n != 4) {
				return false;
			}
			out = Value::of_date(Date{get_u16(p), p[2], p[3]});
			return true;
		case ValueType::date_time: {
			if (n != 10) {
				return false;
			}
			DateTime d;
			d.year = get_u16(p);
			d.month = p[2];
			d.day = p[3];
			d.day_of_week = p[4];
			d.hour = p[5];
			d.minute = p[6];
			d.second = p[7];
			d.flags = get_u16(p + 8);
			out = Value::of_date_time(d);
			return true;
		}
		case ValueType::controlled:
			if (n != 2 || p[0] > 1) {
				return false;
			}
			out = Value::of_controlled(p[0] != 0, p[1]);
			return true;
		case ValueType::color:
			if (n != 5) {
				return false;
			}
			out = Value::of_color(Color{p[0], p[1], p[2], p[3], p[4]});
			return true;
		case ValueType::none: return false;
	}
	return false;
}

} // namespace

// ----------------------------------------------------------------------------------------------
// persist

namespace persist {

uint32_t crc32(uint32_t crc, const void* data, size_t size) {
	const uint8_t* p = static_cast<const uint8_t*>(data);
	crc = ~crc;
	for (size_t i = 0; i < size; ++i) {
		crc ^= p[i];
		for (int bit = 0; bit < 8; ++bit) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

uint32_t fnv1a(const char* text) {
	uint32_t hash = 2166136261u;
	for (const char* p = text; *p != '\0'; ++p) {
		hash ^= static_cast<uint8_t>(*p);
		hash *= 16777619u;
	}
	return hash;
}

void blob_name(const char* prefix, const char* datapoint_name, char* buffer) {
	std::snprintf(buffer, kBlobNameSize, "%.4s%08" PRIx32, prefix, fnv1a(datapoint_name));
}

namespace {

/// Collects up to kBatch orphans per pass (the visitor must not modify the store).
class OrphanCollector : public BlobVisitor {
public:
	static constexpr size_t kBatch = 16;

	OrphanCollector(const char* prefix, const BlobKeeper& keeper) : prefix_(prefix), keeper_(keeper) {}

	void blob(const char* name, size_t) override {
		if (count_ == kBatch || std::strncmp(name, prefix_, std::strlen(prefix_)) != 0 || keeper_.keep(name)) {
			return;
		}
		std::snprintf(names_[count_++], sizeof(names_[0]), "%s", name);
	}

	size_t count() const { return count_; }
	const char* name(size_t i) const { return names_[i]; }

private:
	const char* prefix_;
	const BlobKeeper& keeper_;
	char names_[kBatch][BlobStore::kMaxNameLength + 1] = {};
	size_t count_ = 0;
};

} // namespace

size_t delete_orphans(BlobStore& store, const char* prefix, const BlobKeeper& keeper, const char* tag) {
	size_t deleted = 0;
	for (;;) {
		OrphanCollector collector(prefix, keeper);
		store.visit(collector);
		size_t removed = 0;
		for (size_t i = 0; i < collector.count(); ++i) {
			BlobStatus status = store.remove(collector.name(i));
			if (status == BlobStatus::ok) {
				Log::info(tag, "deleted %s: no longer in the image", collector.name(i));
				++removed;
			} else {
				Log::error(tag, "cannot delete %s: %s", collector.name(i), blob_status_name(status));
			}
		}
		deleted += removed;
		if (removed == 0 || collector.count() < OrphanCollector::kBatch) {
			return deleted;
		}
	}
}

} // namespace persist

// ----------------------------------------------------------------------------------------------
// ValueStore

struct ValueStore::Slot {
	Value value;                        ///< The last good value seen (or restored).
	uint64_t updated_ms = 0;            ///< Monotonic time of `value`; 0 when restored.
	uint64_t written_ms = 0;            ///< Monotonic time of the last write attempt.
	int64_t wall_s = kUnknown;          ///< Wall time of `value`, kUnknown when not known.
	int64_t saved_wall_s = kUnknown;    ///< Wall time in the record on flash, kUnknown when not known.
	const char* name = "";              ///< Own copy of the datapoint's name.
	uint32_t hash = 0;
	uint16_t id = 0;
	bool has_value = false;
	bool pending = false;
	bool written = false;     ///< A write was attempted since the load.
	bool persistable = true;  ///< False for the second of two names with the same hash.
};

ValueStore::ValueStore(EventLoop& loop, const Clock& clock, DatapointModel& model, BlobStore& store,
	const ValueStoreLimits& limits)
	: loop_(loop), clock_(clock), model_(model), store_(store), limits_(limits) {
	tokens_ = static_cast<uint64_t>(limits_.writes_per_hour) * kWriteCost;
	refilled_ms_ = clock_.monotonic_ms();
}

ValueStore::~ValueStore() {
	if (tick_.valid()) {
		loop_.cancel(tick_);
	}
	if (listening_) {
		model_.remove_listener(*this);
	}
	delete[] slots_;
	delete[] block_;
}

size_t ValueStore::encode(const char* name, const Value& value, std::optional<int64_t> wall_s, uint8_t* buffer,
	size_t size) {
	size_t name_length = std::strlen(name);
	uint8_t payload[1 + Value::kMaxString];
	size_t value_length = encode_value(value, payload);
	size_t total = 3 + 2 + name_length + 2 + value_length + (wall_s ? 2 + 8 : 0) + 4;
	if (name_length == 0 || name_length > Datapoint::kMaxName || value.empty() || total > size) {
		return 0;
	}
	uint8_t* p = buffer;
	*p++ = kMagic0;
	*p++ = kMagic1;
	*p++ = kRecordVersion;
	*p++ = kFieldName;
	*p++ = static_cast<uint8_t>(name_length);
	std::memcpy(p, name, name_length);
	p += name_length;
	*p++ = kFieldValue;
	*p++ = static_cast<uint8_t>(value_length);
	std::memcpy(p, payload, value_length);
	p += value_length;
	if (wall_s) {
		*p++ = kFieldWall;
		*p++ = 8;
		put_u64(p, static_cast<uint64_t>(*wall_s));
		p += 8;
	}
	put_u32(p, persist::crc32(0, buffer, static_cast<size_t>(p - buffer)));
	p += 4;
	return static_cast<size_t>(p - buffer);
}

bool ValueStore::decode(const uint8_t* data, size_t length, Record& out, const char** error) {
	const char* dummy = nullptr;
	const char*& why = error != nullptr ? *error : dummy;
	if (length < 3 + 4 || data[0] != kMagic0 || data[1] != kMagic1) {
		why = "not a value record";
		return false;
	}
	if (data[2] == 0 || data[2] > kRecordVersion) {
		why = data[2] == 0 ? "format generation 0" : "written by a newer firmware (format generation unknown)";
		return false;
	}
	size_t end = length - 4;
	if (persist::crc32(0, data, end) != get_u32(data + end)) {
		why = "checksum mismatch";
		return false;
	}
	bool has_name = false;
	bool has_value = false;
	out.wall_s.reset();
	size_t pos = 3;
	while (pos < end) {
		if (end - pos < 2 || end - pos - 2 < data[pos + 1]) {
			why = "truncated field";
			return false;
		}
		uint8_t tag = data[pos];
		size_t n = data[pos + 1];
		const uint8_t* field = data + pos + 2;
		switch (tag) {
			case kFieldName:
				if (n == 0 || n > Datapoint::kMaxName) {
					why = "name length";
					return false;
				}
				std::memcpy(out.name, field, n);
				out.name[n] = '\0';
				has_name = std::strlen(out.name) == n;
				if (!has_name) {
					why = "NUL in the name";
					return false;
				}
				break;
			case kFieldValue:
				if (!decode_value(field, n, out.value)) {
					why = "invalid value";
					return false;
				}
				has_value = true;
				break;
			case kFieldWall:
				if (n != 8) {
					why = "time length";
					return false;
				}
				out.wall_s = static_cast<int64_t>(get_u64(field));
				break;
			default: break; // A field of a newer firmware: skipped.
		}
		pos += 2 + n;
	}
	if (!has_name || !has_value) {
		why = has_name ? "no value" : "no name";
		return false;
	}
	return true;
}

ValueStore::Slot* ValueStore::slot(uint16_t id) const {
	Slot* begin = slots_;
	Slot* end = slots_ + count_;
	Slot* it = std::lower_bound(begin, end, id, [](const Slot& s, uint16_t key) { return s.id < key; });
	return it != end && it->id == id ? it : nullptr;
}

namespace {

class KeepNothing : public persist::BlobKeeper {
public:
	bool keep(const char*) const override { return false; }
};

} // namespace

DatapointStatus ValueStore::load() {
	if (model_.notifying()) {
		Log::error(kTag, "refusing to load the value store from a datapoint listener (post it)");
		return DatapointStatus::busy;
	}
	if (!listening_) {
		DatapointStatus status = model_.add_listener(*this);
		if (status != DatapointStatus::ok) {
			Log::error(kTag, "value store disabled: cannot listen to the model (%s)", datapoint_status_name(status));
			return status;
		}
		listening_ = true;
		uint64_t now = clock_.monotonic_ms();
		tick_ = loop_.schedule_periodic(now + kTickMs, kTickMs, Callback::member<ValueStore, &ValueStore::on_tick>(this));
		if (!tick_.valid()) {
			model_.remove_listener(*this);
			listening_ = false;
			Log::error(kTag, "value store disabled: no timer for its tick");
			return DatapointStatus::capacity;
		}
	}

	// What the previous image left pending is written from the store's own copies.
	flush();
	delete[] slots_;
	delete[] block_;
	slots_ = nullptr;
	block_ = nullptr;
	count_ = 0;

	size_t count = 0;
	size_t names_bytes = 0;
	for (size_t i = 0; i < model_.size(); ++i) {
		const Datapoint* dp = model_.get(static_cast<uint16_t>(i));
		if (dp->persists()) {
			++count;
			names_bytes += std::strlen(dp->name()) + 1;
		}
	}
	if (count == 0) {
		Log::info(kTag, "value store: no persisted datapoints");
		persist::delete_orphans(store_, kPrefix, KeepNothing(), kTag);
		return DatapointStatus::ok;
	}
	Slot* slots = new (std::nothrow) Slot[count];
	char* block = new (std::nothrow) char[names_bytes];
	if (slots == nullptr || block == nullptr) {
		delete[] slots;
		delete[] block;
		Log::error(kTag, "value store disabled: no memory for %u persisted datapoints (%u bytes)",
			static_cast<unsigned>(count), static_cast<unsigned>(count * sizeof(Slot) + names_bytes));
		return DatapointStatus::out_of_memory;
	}
	char* cursor = block;
	size_t n = 0;
	for (size_t i = 0; i < model_.size(); ++i) {
		const Datapoint* dp = model_.get(static_cast<uint16_t>(i));
		if (!dp->persists()) {
			continue;
		}
		size_t length = std::strlen(dp->name());
		std::memcpy(cursor, dp->name(), length + 1);
		Slot& s = slots[n++];
		s.name = cursor;
		s.id = dp->id();
		s.hash = persist::fnv1a(cursor);
		cursor += length + 1;
	}
	// Two names with the same hash would share a blob: the second is not persisted.
	std::sort(slots, slots + count, [](const Slot& a, const Slot& b) {
		return a.hash != b.hash ? a.hash < b.hash : a.id < b.id;
	});
	for (size_t i = 1; i < count; ++i) {
		if (slots[i].hash == slots[i - 1].hash) {
			slots[i].persistable = false;
			Log::error(kTag, "'%s' is not persisted: its record name collides with '%s'", slots[i].name,
				slots[i - 1].name);
		}
	}
	std::sort(slots, slots + count, [](const Slot& a, const Slot& b) { return a.id < b.id; });
	slots_ = slots;
	block_ = block;
	count_ = count;

	size_t restored = 0;
	for (size_t i = 0; i < count_; ++i) {
		if (slots_[i].persistable) {
			restore(slots_[i]);
			restored += slots_[i].has_value ? 1 : 0;
		}
	}

	// Keeps the blob names of the persistable slots.
	class Keeper : public persist::BlobKeeper {
	public:
		Keeper(const Slot* slots, size_t count) : slots_(slots), count_(count) {}
		bool keep(const char* name) const override {
			for (size_t i = 0; i < count_; ++i) {
				char own[persist::kBlobNameSize];
				persist::blob_name(kPrefix, slots_[i].name, own);
				if (slots_[i].persistable && std::strcmp(own, name) == 0) {
					return true;
				}
			}
			return false;
		}

	private:
		const Slot* slots_;
		size_t count_;
	};
	persist::delete_orphans(store_, kPrefix, Keeper(slots_, count_), kTag);
	Log::info(kTag, "value store: %u persisted datapoints, %u restored, %u bytes", static_cast<unsigned>(count_),
		static_cast<unsigned>(restored), static_cast<unsigned>(count_ * sizeof(Slot) + names_bytes));
	return DatapointStatus::ok;
}

void ValueStore::restore(Slot& s) {
	char name[persist::kBlobNameSize];
	persist::blob_name(kPrefix, s.name, name);
	size_t size = 0;
	BlobStatus status = store_.size(name, size);
	if (status == BlobStatus::not_found) {
		return;
	}
	if (status != BlobStatus::ok) {
		Log::error(kTag, "cannot read the record of '%s' (%s): %s", s.name, name, blob_status_name(status));
		return;
	}
	if (size > kMaxRecordRead) {
		Log::error(kTag, "record of '%s' (%s) ignored: %u bytes exceed %u", s.name, name, static_cast<unsigned>(size),
			static_cast<unsigned>(kMaxRecordRead));
		return;
	}
	uint8_t buffer[kMaxRecordRead];
	size_t length = 0;
	status = store_.read(name, 0, buffer, sizeof(buffer), length);
	if (status != BlobStatus::ok) {
		Log::error(kTag, "cannot read the record of '%s' (%s): %s", s.name, name, blob_status_name(status));
		return;
	}
	Record record;
	const char* error = nullptr;
	if (!decode(buffer, length, record, &error)) {
		Log::error(kTag, "record of '%s' (%s) ignored: %s", s.name, name, error);
		return;
	}
	if (std::strcmp(record.name, s.name) != 0) {
		Log::error(kTag, "record %s belongs to '%s', not '%s': ignored", name, record.name, s.name);
		return;
	}
	if (model_.restore(s.id, record.value) != DatapointStatus::ok) {
		return; // The model logged why (a changed type).
	}
	const Datapoint* dp = model_.get(s.id);
	s.value = dp->value();
	s.has_value = true;
	s.wall_s = to_raw(record.wall_s);
	s.saved_wall_s = s.wall_s;
}

void ValueStore::datapoint_updated(const DatapointUpdate& update) {
	const Datapoint& dp = update.datapoint;
	if (update.origin == UpdateOrigin::stale || !dp.persists()) {
		return;
	}
	Slot* s = slot(dp.id());
	if (s == nullptr || !s->persistable || std::strcmp(s->name, dp.name()) != 0) {
		return; // Not (yet) loaded for this model.
	}
	bool changed = !s->has_value || s->value != dp.value();
	WallTime wall = clock_.wall_time();
	s->value = dp.value();
	s->has_value = true;
	s->updated_ms = dp.updated_ms();
	s->wall_s = wall.synchronized ? wall.seconds : kUnknown;
	if (!changed) {
		// The same value again: worth a write only to refresh an old (or unknown) saved time.
		if (s->wall_s == kUnknown) {
			return;
		}
		if (s->saved_wall_s != kUnknown && s->wall_s - s->saved_wall_s < static_cast<int64_t>(limits_.refresh_s)) {
			return;
		}
	}
	s->pending = true;
	uint64_t now = clock_.monotonic_ms();
	bool due = !s->written || now - s->written_ms >= static_cast<uint64_t>(limits_.min_interval_s) * 1000;
	if (due && !posted_) {
		posted_ = loop_.post(Callback::member<ValueStore, &ValueStore::on_post>(this));
	}
}

void ValueStore::on_post() {
	posted_ = false;
	write_due();
}

void ValueStore::on_tick() { write_due(); }

void ValueStore::refill(uint64_t now_ms) {
	uint64_t capacity = static_cast<uint64_t>(limits_.writes_per_hour) * kWriteCost;
	uint64_t elapsed = now_ms - refilled_ms_;
	refilled_ms_ = now_ms;
	uint64_t gain = elapsed * limits_.writes_per_hour;
	tokens_ = capacity - tokens_ <= gain ? capacity : tokens_ + gain;
}

void ValueStore::write_due() {
	uint64_t now = clock_.monotonic_ms();
	refill(now);
	uint64_t interval = static_cast<uint64_t>(limits_.min_interval_s) * 1000;
	size_t deferred = 0;
	for (size_t i = 0; i < count_; ++i) {
		Slot& s = slots_[i];
		if (!s.pending || (s.written && now - s.written_ms < interval)) {
			continue;
		}
		if (tokens_ < kWriteCost) {
			++deferred;
			continue;
		}
		tokens_ -= kWriteCost;
		write(s, now);
	}
	if (deferred > 0 && !budget_logged_) {
		Log::warn(kTag, "write budget of %u per hour exhausted: %u values wait",
			static_cast<unsigned>(limits_.writes_per_hour),
			static_cast<unsigned>(deferred));
		budget_logged_ = true;
	} else if (deferred == 0 && budget_logged_) {
		Log::info(kTag, "write budget recovered");
		budget_logged_ = false;
	}
}

std::optional<int64_t> ValueStore::resolve_wall(const Slot& s) const {
	if (s.wall_s != kUnknown) {
		return s.wall_s;
	}
	if (!s.has_value || s.updated_ms == 0) {
		return std::nullopt;
	}
	WallTime wall = clock_.wall_time();
	if (!wall.synchronized) {
		return std::nullopt;
	}
	uint64_t now = clock_.monotonic_ms();
	return wall.seconds - static_cast<int64_t>((now - s.updated_ms) / 1000);
}

bool ValueStore::write(Slot& s, uint64_t now_ms) {
	s.written = true;
	s.written_ms = now_ms;
	std::optional<int64_t> wall = resolve_wall(s);
	uint8_t buffer[kMaxRecord];
	size_t length = encode(s.name, s.value, wall, buffer, sizeof(buffer));
	if (length == 0) {
		s.pending = false;
		Log::error(kTag, "cannot encode the record of '%s'", s.name);
		return false;
	}
	char name[persist::kBlobNameSize];
	persist::blob_name(kPrefix, s.name, name);
	BlobChunk chunk{buffer, length};
	BlobStatus status = store_.write(name, &chunk, 1);
	if (status != BlobStatus::ok) {
		if (!failure_logged_) {
			Log::error(kTag, "cannot save '%s' (%s): %s; retrying", s.name, name, blob_status_name(status));
			failure_logged_ = true;
		}
		return false;
	}
	if (failure_logged_) {
		Log::info(kTag, "saving values works again");
		failure_logged_ = false;
	}
	s.pending = false;
	s.wall_s = to_raw(wall);
	s.saved_wall_s = s.wall_s;
	++writes_;
	return true;
}

void ValueStore::flush() {
	uint64_t now = clock_.monotonic_ms();
	for (size_t i = 0; i < count_; ++i) {
		if (slots_[i].pending) {
			write(slots_[i], now);
		}
	}
}

size_t ValueStore::pending() const {
	size_t result = 0;
	for (size_t i = 0; i < count_; ++i) {
		result += slots_[i].pending ? 1 : 0;
	}
	return result;
}

std::optional<int64_t> ValueStore::wall_time(uint16_t id) const {
	const Slot* s = slot(id);
	const Datapoint* dp = model_.get(id);
	if (s == nullptr || dp == nullptr || std::strcmp(s->name, dp->name()) != 0) {
		return std::nullopt;
	}
	return resolve_wall(*s);
}

} // namespace hc
