/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/blocks/block_runtime.h"

#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "hc/value_store.h"

namespace hc::blocks {

namespace {

constexpr const char* kTag = "hc.block";
constexpr uint16_t kNone = rules::kNone;
constexpr uint8_t kRecordFormat = 1;
constexpr const char kMagic[4] = {'H', 'C', 'B', 'S'};

const char* const kLogCodeNames[] = {"log", "fault", "writeRefused", "timerRefused", "parameterRefused",
	"parameterReset", "stateReset", "loopGuard"};

/// A datapoint value as a number (spec/blocks.md B6).
double number_of(const Value& v) {
	switch (v.type()) {
		case ValueType::boolean: return v.as_bool() ? 1 : 0;
		case ValueType::unsigned_int: return static_cast<double>(v.as_unsigned());
		case ValueType::signed_int: return static_cast<double>(v.as_signed());
		case ValueType::real: return v.as_real();
		case ValueType::enumeration: return static_cast<double>(v.as_enum());
		case ValueType::time_of_day: {
			TimeOfDay t = v.as_time();
			return static_cast<double>(t.hour * 3600 + t.minute * 60 + t.second);
		}
		default: return 0;
	}
}

/// A number as a value of the datapoint type `type`; empty when it does not fit.
std::optional<Value> value_of(ValueType type, double v) {
	switch (type) {
		case ValueType::boolean: return Value::of_bool(v != 0);
		case ValueType::real: return Value::of_real(v);
		case ValueType::enumeration:
			if (v < 0 || v > 4294967295.0 || v != std::floor(v)) {
				return std::nullopt;
			}
			return Value::of_enum(static_cast<uint32_t>(v));
		case ValueType::unsigned_int:
		case ValueType::signed_int: {
			double r = v >= 0 ? std::floor(v + 0.5) : -std::floor(-v + 0.5); // half away from zero
			if (type == ValueType::unsigned_int) {
				if (r < 0 || r > 4294967295.0) {
					return std::nullopt;
				}
				return Value::of_unsigned(static_cast<uint32_t>(r));
			}
			if (r < -2147483648.0 || r > 2147483647.0) {
				return std::nullopt;
			}
			return Value::of_signed(static_cast<int32_t>(r));
		}
		case ValueType::time_of_day: {
			if (v < 0 || v > 86399) {
				return std::nullopt;
			}
			uint32_t s = static_cast<uint32_t>(v);
			TimeOfDay t;
			t.hour = static_cast<uint8_t>(s / 3600);
			t.minute = static_cast<uint8_t>(s % 3600 / 60);
			t.second = static_cast<uint8_t>(s % 60);
			return Value::of_time(t);
		}
		default: return std::nullopt;
	}
}

rules::Val to_val(ParamType type, double v) {
	switch (type) {
		case ParamType::boolean: return rules::Val::of_bool(v != 0);
		case ParamType::integer: return rules::Val::of_int(static_cast<int64_t>(v));
		case ParamType::real: return rules::Val::of_real(v);
		case ParamType::duration: return rules::Val::of_duration(static_cast<int64_t>(v));
		case ParamType::time_of_day: {
			uint32_t s = static_cast<uint32_t>(v);
			TimeOfDay t;
			t.hour = static_cast<uint8_t>(s / 3600);
			t.minute = static_cast<uint8_t>(s % 3600 / 60);
			t.second = static_cast<uint8_t>(s % 60);
			return rules::Val::of_time(t);
		}
		case ParamType::enumeration: return rules::Val::of_enum(static_cast<uint32_t>(v));
	}
	return rules::Val::none();
}

/// A stored override back into a number; false when its type is not the parameter's.
bool from_val(ParamType type, const rules::Val& v, double& out) {
	if (!v.has()) {
		return false;
	}
	switch (type) {
		case ParamType::boolean:
			if (v.type != rules::Type::boolean) return false;
			out = v.u.b ? 1 : 0;
			return true;
		case ParamType::integer:
			if (v.type != rules::Type::integer) return false;
			out = static_cast<double>(v.u.i);
			return true;
		case ParamType::real:
			if (v.type != rules::Type::real) return false;
			out = v.u.r;
			return true;
		case ParamType::duration:
			if (v.type != rules::Type::duration) return false;
			out = static_cast<double>(v.u.i);
			return true;
		case ParamType::time_of_day:
			if (v.type != rules::Type::time_of_day) return false;
			out = static_cast<double>(v.u.tod.hour * 3600 + v.u.tod.minute * 60 + v.u.tod.second);
			return true;
		case ParamType::enumeration:
			if (v.type != rules::Type::enumeration) return false;
			out = static_cast<double>(v.u.e);
			return true;
	}
	return false;
}

/// The store's logical "rule" part of a block's override: `@<blockId>`.
void store_key(const char* block, char* out, size_t size) { std::snprintf(out, size, "@%s", block); }

void put16(uint8_t*& p, uint16_t v) {
	*p++ = static_cast<uint8_t>(v);
	*p++ = static_cast<uint8_t>(v >> 8);
}

void put_text(uint8_t*& p, const char* text) {
	size_t n = std::strlen(text);
	*p++ = static_cast<uint8_t>(n);
	std::memcpy(p, text, n);
	p += n;
}

} // namespace

const char* block_status_name(BlockStatus status) {
	switch (status) {
		case BlockStatus::running: return "running";
		case BlockStatus::disabled: return "disabled";
		case BlockStatus::fault: return "fault";
	}
	return "?";
}

const char* block_log_code_name(BlockLogCode code) { return kLogCodeNames[static_cast<size_t>(code)]; }

const char* call_kind_name(CallKind kind) {
	switch (kind) {
		case CallKind::none: return "none";
		case CallKind::start: return "start";
		case CallKind::input: return "input";
		case CallKind::timer: return "timer";
		case CallKind::period: return "period";
		case CallKind::parameter: return "parameter";
	}
	return "?";
}

const char* change_status_name(ChangeStatus status) {
	switch (status) {
		case ChangeStatus::ok: return "ok";
		case ChangeStatus::unknown_block: return "unknown";
		case ChangeStatus::unknown_parameter: return "unknown_parameter";
		case ChangeStatus::not_adjustable: return "not_adjustable";
		case ChangeStatus::invalid_value: return "invalid_value";
		case ChangeStatus::not_faulted: return "not_faulted";
	}
	return "?";
}

// ------------------------------------------------------------------------------------------
// Runtime structures

struct BlockRuntime::Instance {
	BlockStatus status = BlockStatus::disabled;
	bool started = false;          ///< Called since its start: inputs call it.
	bool enabled = true;
	bool enabled_overridden = false;
	bool restored = false;         ///< The record comes from before (the next start's `restored`).
	bool dirty = false;            ///< A persisted field differs from the stored record.
	bool written_once = false;     ///< Written since the boot: the next write waits for the interval.
	double* state = nullptr;       ///< One per manifest field.
	double* stored = nullptr;      ///< The persisted fields as the store holds them.
	double* params = nullptr;      ///< One per manifest parameter.
	uint32_t overridden = 0;       ///< Bit per parameter.
	uint16_t* mirror_params = nullptr;
	uint16_t* mirror_ports = nullptr;
	double* input_value = nullptr; ///< Per port: the input as last seen (change detection).
	Quality* input_quality = nullptr;
	uint16_t mirror_enabled = kNone;
	uint64_t timers[BlockLimits::kTimers] = {};
	uint8_t running = 0;           ///< Bit per timer.
	uint64_t period_next = 0;
	bool period_armed = false;
	uint64_t last_write_ms = 0;
	uint32_t cascade_calls = 0;
	// Diagnostics.
	uint32_t calls = 0;
	uint32_t time_ms = 0;
	uint32_t longest_ms = 0;
	uint32_t errors = 0;
	CallKind last_call = CallKind::none;
	uint64_t last_call_ms = 0;
	const char* last_call_arg = "";
	bool has_error = false;
	BlockLogCode last_error_code = BlockLogCode::log;
	uint64_t last_error_ms = 0;
	uint64_t fault_ms = 0;
	char last_error[80] = {};
	char fault[64] = {};
};

struct BlockRuntime::Runtime {
	size_t bytes = 0;
	Instance* instances = nullptr;
	size_t count = 0;
	uint8_t* interest = nullptr; ///< Bit per datapoint id: bound to an input.
	size_t interest_bits = 0;
};

/// What a call of a type may use (spec/blocks.md 3.1).
class BlockRuntime::Context : public BlockContext {
public:
	Context(BlockRuntime& runtime, size_t block)
		: rt_(runtime), block_(block), in_(runtime.runtime_->instances[block]), spec_(runtime.set_.at(block)),
		  m_(spec_.type->manifest()) {}

	uint64_t now() const override { return rt_.engine_.in_cascade() ? rt_.engine_.cascade_ms() : rt_.clock_.monotonic_ms(); }
	bool wall(DateTime& out) const override { return rt_.engine_.cascade_wall(out); }

	InputValue read(size_t port) const override {
		if (port >= m_.port_count || m_.ports[port].direction != Direction::in) {
			return InputValue();
		}
		return rt_.aggregate(in_, port);
	}

	double param(size_t index) const override { return index < m_.param_count ? in_.params[index] : 0; }

	void write(size_t port, double value) override {
		if (faulted) {
			return;
		}
		if (port >= m_.port_count || m_.ports[port].direction != Direction::out) {
			fault("a write to a port that is no output");
			return;
		}
		const PortDecl& d = m_.ports[port];
		const char* why = nullptr;
		if (!std::isfinite(value)) {
			why = "is not finite";
		} else if (d.type == PortType::boolean && value != 0 && value != 1) {
			why = "is not 0 or 1";
		} else if ((d.type == PortType::integer || d.type == PortType::enumeration) && value != std::floor(value)) {
			why = "is not a whole number";
		} else if (d.type == PortType::integer && (value < -2147483648.0 || value > 2147483647.0)) {
			why = "is outside the signed 32-bit range";
		} else if (d.type == PortType::enumeration && (value < 0 || value > 4294967295.0)) {
			why = "is outside 0..4294967295";
		} else if ((d.has_min && value < d.min) || (d.has_max && value > d.max)) {
			why = "is outside the port's range";
		}
		if (why != nullptr) {
			char text[64];
			std::snprintf(text, sizeof(text), "the value %.6g of '%s' %s", value, d.id, why);
			fault(text);
			return;
		}
		rt_.write_output(block_, port, value);
	}

	void set_timer(size_t timer, int64_t ms) override {
		if (faulted) {
			return;
		}
		if (timer >= m_.timer_count) {
			fault("a timer the type does not declare");
			return;
		}
		if (ms < BlockLimits::kTimerMinMs || ms > BlockLimits::kTimerMaxMs) {
			rt_.log(block_, BlockLogCode::timer_refused, LogLevel::error, "timer '%s' of %" PRId64 " ms is not in 1 ms..30 d",
				m_.timers[timer], ms);
			fault("a timer outside 1 ms..30 d");
			return;
		}
		in_.timers[timer] = now() + static_cast<uint64_t>(ms);
		in_.running = static_cast<uint8_t>(in_.running | (1u << timer));
	}

	void cancel_timer(size_t timer) override {
		if (timer < m_.timer_count) {
			in_.running = static_cast<uint8_t>(in_.running & ~(1u << timer));
		}
	}

	bool timer_running(size_t timer) const override {
		return timer < m_.timer_count && (in_.running & (1u << timer)) != 0;
	}

	double* state() override { return in_.state; }

	void log(const char* text) override { rt_.log(block_, BlockLogCode::log, LogLevel::info, "%s", text); }

	void fault(const char* reason) override {
		if (!faulted) {
			faulted = true;
			std::snprintf(reason_, sizeof(reason_), "%s", reason);
		}
	}

	bool faulted = false;
	const char* reason() const { return reason_; }

private:
	BlockRuntime& rt_;
	size_t block_;
	Instance& in_;
	const BlockSpec& spec_;
	const Manifest& m_;
	char reason_[64] = {};
};

// ------------------------------------------------------------------------------------------
// Construction

BlockRuntime::BlockRuntime(EventLoop& loop, DatapointModel& model, const Clock& clock, Settings& settings,
	BlobStore& blobs, rules::Engine& engine)
	: loop_(loop), model_(model), clock_(clock), settings_(settings), blobs_(blobs), engine_(engine), store_(settings) {
	engine_.set_block_host(this);
}

BlockRuntime::~BlockRuntime() {
	flush();
	if (persist_timer_.valid()) {
		loop_.cancel(persist_timer_);
	}
	engine_.set_block_host(nullptr);
	release(runtime_);
	runtime_ = nullptr;
}

void BlockRuntime::release(Runtime* runtime) {
	if (runtime == nullptr) {
		return;
	}
	for (size_t i = 0; i < runtime->count; ++i) {
		runtime->instances[i].~Instance();
	}
	runtime->~Runtime();
	::operator delete(static_cast<void*>(runtime), std::nothrow);
}

// ------------------------------------------------------------------------------------------
// Logging

void BlockRuntime::log(size_t block, BlockLogCode code, LogLevel level, const char* format, ...) {
	char text[160];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	bool known = runtime_ != nullptr && block < runtime_->count;
	log_text(known ? set_.at(block).id : "-", known ? &runtime_->instances[block] : nullptr, code, level, text);
}

void BlockRuntime::log_text(const char* id, Instance* in, BlockLogCode code, LogLevel level, const char* text) {
	Log::write(level, kTag, "%s %s: %s", id, block_log_code_name(code), text);
	if (observer_) {
		observer_->block_logged(id, code, level, text);
	}
	if (in != nullptr && code != BlockLogCode::log && level <= LogLevel::warn) {
		++in->errors;
		in->has_error = true;
		in->last_error_code = code;
		in->last_error_ms = engine_.in_cascade() ? engine_.cascade_ms() : clock_.monotonic_ms();
		std::snprintf(in->last_error, sizeof(in->last_error), "%s", text);
	}
}

// ------------------------------------------------------------------------------------------
// Records (spec/blocks.md 8.1, doc/blocks.md "The record")

bool BlockRuntime::encode_record(const Manifest& m, const char* block, const double* values, uint8_t* out, size_t size,
	size_t& length) {
	size_t need = 4 + 1 + 1 + std::strlen(m.type) + 2 + 1 + std::strlen(block) + 1 + 4;
	size_t fields = 0;
	for (size_t i = 0; i < m.state_count; ++i) {
		if (m.state[i].persist) {
			need += 1 + std::strlen(m.state[i].id) + 8;
			++fields;
		}
	}
	if (need > size || std::strlen(m.type) > 255 || std::strlen(block) > 255) {
		return false;
	}
	uint8_t* p = out;
	std::memcpy(p, kMagic, 4);
	p += 4;
	*p++ = kRecordFormat;
	put_text(p, m.type);
	put16(p, m.major);
	put_text(p, block);
	*p++ = static_cast<uint8_t>(fields);
	for (size_t i = 0; i < m.state_count; ++i) {
		if (!m.state[i].persist) {
			continue;
		}
		put_text(p, m.state[i].id);
		uint64_t bits = 0;
		std::memcpy(&bits, &values[i], 8);
		for (int b = 0; b < 8; ++b) {
			*p++ = static_cast<uint8_t>(bits >> (8 * b));
		}
	}
	uint32_t crc = persist::crc32(0, out, static_cast<size_t>(p - out));
	for (int b = 0; b < 4; ++b) {
		*p++ = static_cast<uint8_t>(crc >> (8 * b));
	}
	length = static_cast<size_t>(p - out);
	return true;
}

bool BlockRuntime::decode_record(const Manifest& m, const char* block, const uint8_t* data, size_t length,
	double* values, const char** reason) {
	*reason = "damaged";
	if (length < 4 + 1 + 1 + 2 + 1 + 1 + 4 || std::memcmp(data, kMagic, 4) != 0) {
		return false;
	}
	uint32_t crc = 0;
	for (int b = 0; b < 4; ++b) {
		crc |= static_cast<uint32_t>(data[length - 4 + b]) << (8 * b);
	}
	if (persist::crc32(0, data, length - 4) != crc) {
		return false;
	}
	const uint8_t* p = data + 4;
	const uint8_t* end = data + length - 4;
	if (*p++ != kRecordFormat) {
		*reason = "of an unknown format";
		return false;
	}
	auto text = [&](char* out, size_t size) {
		if (p >= end || static_cast<size_t>(*p) >= size || p + 1 + *p > end) {
			return false;
		}
		size_t n = *p++;
		std::memcpy(out, p, n);
		out[n] = '\0';
		p += n;
		return true;
	};
	char type[72];
	char id[BlockLimits::kIdLength + 2];
	if (!text(type, sizeof(type)) || end - p < 2) {
		return false;
	}
	uint16_t major = static_cast<uint16_t>(p[0] | (p[1] << 8));
	p += 2;
	if (!text(id, sizeof(id)) || p >= end) {
		return false;
	}
	if (std::strcmp(type, m.type) != 0 || major != m.major) {
		*reason = "of another block type or major version";
		return false;
	}
	if (std::strcmp(id, block) != 0) {
		*reason = "of another block";
		return false;
	}
	size_t fields = *p++;
	double parsed[BlockLimits::kStateFields];
	bool found[BlockLimits::kStateFields] = {};
	for (size_t f = 0; f < fields; ++f) {
		char field[32];
		if (!text(field, sizeof(field)) || end - p < 8) {
			return false;
		}
		uint64_t bits = 0;
		for (int b = 0; b < 8; ++b) {
			bits |= static_cast<uint64_t>(p[b]) << (8 * b);
		}
		p += 8;
		int i = m.field(field);
		if (i >= 0 && m.state[i].persist) {
			std::memcpy(&parsed[i], &bits, 8);
			found[i] = std::isfinite(parsed[i]);
		}
	}
	if (p != end) {
		return false;
	}
	for (size_t i = 0; i < m.state_count; ++i) {
		if (found[i]) {
			values[i] = parsed[i];
		}
	}
	return true;
}

bool BlockRuntime::load_record(size_t block, bool& mismatch) {
	mismatch = false;
	const BlockSpec& spec = set_.at(block);
	const Manifest& m = spec.type->manifest();
	Instance& in = runtime_->instances[block];
	if (!m.persists()) {
		return false;
	}
	char name[persist::kBlobNameSize];
	persist::blob_name(kBlobPrefix, spec.id, name);
	uint8_t data[BlockLimits::kRecordBytes + 1];
	size_t length = 0;
	BlobStatus status = blobs_.read(name, 0, data, sizeof(data), length);
	if (status == BlobStatus::not_found || (status == BlobStatus::ok && length == 0)) {
		return false;
	}
	const char* reason = "unreadable";
	if (status != BlobStatus::ok || length > BlockLimits::kRecordBytes ||
		!decode_record(m, spec.id, data, length, in.state, &reason)) {
		mismatch = true;
		log(block, BlockLogCode::state_reset, LogLevel::warn, "the persisted state record is %s; the state starts fresh",
			reason);
		for (size_t i = 0; i < m.state_count; ++i) {
			in.state[i] = m.state[i].initial;
		}
		blobs_.remove(name);
		return false;
	}
	for (size_t i = 0; i < m.state_count; ++i) {
		in.stored[i] = in.state[i];
	}
	return true;
}

void BlockRuntime::record_changed(size_t block) {
	Instance& in = runtime_->instances[block];
	const Manifest& m = set_.at(block).type->manifest();
	if (in.status == BlockStatus::fault || !m.persists()) {
		return;
	}
	for (size_t i = 0; i < m.state_count && !in.dirty; ++i) {
		if (m.state[i].persist && std::memcmp(&in.state[i], &in.stored[i], sizeof(double)) != 0) {
			in.dirty = true;
		}
	}
	if (in.dirty) {
		schedule_persist();
	}
}

void BlockRuntime::write_record(size_t block) {
	Instance& in = runtime_->instances[block];
	const BlockSpec& spec = set_.at(block);
	const Manifest& m = spec.type->manifest();
	in.dirty = false;
	in.last_write_ms = clock_.monotonic_ms();
	in.written_once = true;
	uint8_t data[BlockLimits::kRecordBytes];
	size_t length = 0;
	if (!encode_record(m, spec.id, in.state, data, sizeof(data), length)) {
		Log::error(kTag, "%s: the state record does not fit %u bytes: not persisted", spec.id,
			static_cast<unsigned>(sizeof(data)));
		return;
	}
	char name[persist::kBlobNameSize];
	persist::blob_name(kBlobPrefix, spec.id, name);
	BlobChunk chunk{data, length};
	BlobStatus status = blobs_.write(name, &chunk, 1);
	if (status != BlobStatus::ok) {
		Log::error(kTag, "%s: the state record was not persisted: %s", spec.id, blob_status_name(status));
		return;
	}
	for (size_t i = 0; i < m.state_count; ++i) {
		in.stored[i] = in.state[i];
	}
}

void BlockRuntime::schedule_persist() {
	uint64_t earliest = EventLoop::kNever;
	for (size_t b = 0; runtime_ != nullptr && b < runtime_->count; ++b) {
		const Instance& in = runtime_->instances[b];
		if (in.dirty && in.status != BlockStatus::fault) {
			uint64_t due = in.written_once ? in.last_write_ms + persist_interval_ms_ : 0;
			earliest = due < earliest ? due : earliest;
		}
	}
	if (earliest == EventLoop::kNever) {
		if (persist_timer_.valid()) {
			loop_.cancel(persist_timer_);
			persist_timer_ = TimerHandle();
		}
		return;
	}
	if (persist_timer_.valid() && persist_deadline_ == earliest) {
		return;
	}
	if (persist_timer_.valid()) {
		loop_.cancel(persist_timer_);
	}
	persist_deadline_ = earliest;
	persist_timer_ = loop_.schedule_at(earliest, Callback::member<BlockRuntime, &BlockRuntime::on_persist>(this));
	if (!persist_timer_.valid()) {
		Log::error(kTag, "cannot schedule the write of the block states; they are written at the next change");
	}
}

void BlockRuntime::on_persist() {
	persist_timer_ = TimerHandle();
	uint64_t now = clock_.monotonic_ms();
	for (size_t b = 0; runtime_ != nullptr && b < runtime_->count; ++b) {
		Instance& in = runtime_->instances[b];
		if (in.dirty && in.status != BlockStatus::fault &&
			(!in.written_once || in.last_write_ms + persist_interval_ms_ <= now)) {
			write_record(b);
		}
	}
	schedule_persist();
}

void BlockRuntime::flush() {
	for (size_t b = 0; runtime_ != nullptr && b < runtime_->count; ++b) {
		Instance& in = runtime_->instances[b];
		if (in.dirty && in.status != BlockStatus::fault) {
			write_record(b);
		}
	}
	schedule_persist();
}

// ------------------------------------------------------------------------------------------
// Loading and swapping

namespace {

/// The persisted overrides kept after a load: the block runtime's own (`@...`) by the current
/// instances; the rules' entries are not its business.
struct OverrideKeeper : rules::ParameterStore::Visitor {
	explicit OverrideKeeper(const BlockRuntime& r, bool all) : runtime(r), forget_all(all) {}
	bool entry(const char* logical, const rules::Val& value) override {
		(void)value;
		if (logical[0] != '@') {
			return true;
		}
		if (forget_all) {
			return false;
		}
		const char* slash = std::strchr(logical, '/');
		if (slash == nullptr || static_cast<size_t>(slash - logical - 1) > BlockLimits::kIdLength) {
			return false;
		}
		char id[BlockLimits::kIdLength + 1];
		std::memcpy(id, logical + 1, static_cast<size_t>(slash - logical - 1));
		id[slash - logical - 1] = '\0';
		int b = runtime.find(id);
		if (b < 0) {
			return false;
		}
		if (std::strcmp(slash + 1, "enabled") == 0) {
			return runtime.enabled_overridden(static_cast<size_t>(b));
		}
		int p = runtime.set().at(static_cast<size_t>(b)).type->manifest().param(slash + 1);
		return p >= 0 && runtime.overridden(static_cast<size_t>(b), static_cast<size_t>(p));
	}
	const BlockRuntime& runtime;
	bool forget_all;
};

struct RecordKeeper : persist::BlobKeeper {
	RecordKeeper(const BlockSet& s, bool all) : set(s), forget_all(all) {}
	bool keep(const char* name) const override {
		if (forget_all) {
			return false;
		}
		for (size_t i = 0; i < set.size(); ++i) {
			char own[persist::kBlobNameSize];
			persist::blob_name(BlockRuntime::kBlobPrefix, set.at(i).id, own);
			if (std::strcmp(own, name) == 0 && set.at(i).type->manifest().persists()) {
				return true;
			}
		}
		return false;
	}
	const BlockSet& set;
	bool forget_all;
};

/// Logs the overrides of the current instances' parameters that a manifest no longer declares (at
/// boot; at a swap the runtime knows them from the previous instances).
struct GoneOverrides : SettingsVisitor {
	GoneOverrides(const Settings& s, BlockRuntime& r, int block, void (*report)(BlockRuntime&, int, const char*))
		: settings(s), runtime(r), only(block), gone(report) {}
	void entry(const char* key, SettingType type) override {
		if (std::strncmp(key, "rp.", 3) != 0 || type != SettingType::string) {
			return;
		}
		char text[Settings::kMaxStringLength + 1];
		char logical[rules::ParameterStore::kMaxLogicalKey + 1];
		rules::Val v;
		if (settings.get_string(key, text, sizeof(text)) != SettingsStatus::ok ||
			!rules::ParameterStore::decode(text, logical, sizeof(logical), v) || logical[0] != '@') {
			return;
		}
		const char* slash = std::strchr(logical, '/');
		if (slash == nullptr || static_cast<size_t>(slash - logical - 1) > BlockLimits::kIdLength) {
			return;
		}
		char id[BlockLimits::kIdLength + 1];
		std::memcpy(id, logical + 1, static_cast<size_t>(slash - logical - 1));
		id[slash - logical - 1] = '\0';
		int b = runtime.find(id);
		if (b == only && std::strcmp(slash + 1, "enabled") != 0 &&
			runtime.set().at(static_cast<size_t>(b)).type->manifest().param(slash + 1) < 0) {
			gone(runtime, b, slash + 1);
		}
	}
	const Settings& settings;
	BlockRuntime& runtime;
	int only;
	void (*gone)(BlockRuntime&, int, const char*);
};

} // namespace

bool BlockRuntime::adopt(BlockSet&& next_set) {
	BlockSet next = std::move(next_set);
	auto align = [](size_t n) { return (n + 7) & ~static_cast<size_t>(7); };
	size_t count = next.size();
	size_t interest_bits = model_.size();
	size_t bytes = align(sizeof(Runtime)) + align(count * sizeof(Instance));
	for (size_t i = 0; i < count; ++i) {
		const Manifest& m = next.at(i).type->manifest();
		bytes += align(m.state_count * sizeof(double)) * 2 + align(m.param_count * sizeof(double)) +
			align(m.param_count * sizeof(uint16_t)) + align(m.port_count * sizeof(uint16_t)) +
			align(m.port_count * sizeof(double)) + align(m.port_count * sizeof(Quality));
	}
	bytes += align((interest_bits + 7) / 8);
	void* block = ::operator new(bytes, std::nothrow);
	if (block == nullptr) {
		Log::error(kTag, "blocks refused: no memory for their runtime (%u bytes)", static_cast<unsigned>(bytes));
		return false;
	}
	std::memset(block, 0, bytes);
	char* base = static_cast<char*>(block);
	Runtime* rt = new (block) Runtime();
	rt->bytes = bytes;
	size_t offset = align(sizeof(Runtime));
	rt->instances = reinterpret_cast<Instance*>(base + offset);
	offset += align(count * sizeof(Instance));
	rt->count = count;
	for (size_t i = 0; i < count; ++i) {
		Instance* in = new (&rt->instances[i]) Instance();
		const Manifest& m = next.at(i).type->manifest();
		auto take = [&](size_t size) {
			char* p = base + offset;
			offset += align(size);
			return p;
		};
		in->state = reinterpret_cast<double*>(take(m.state_count * sizeof(double)));
		in->stored = reinterpret_cast<double*>(take(m.state_count * sizeof(double)));
		in->params = reinterpret_cast<double*>(take(m.param_count * sizeof(double)));
		in->mirror_params = reinterpret_cast<uint16_t*>(take(m.param_count * sizeof(uint16_t)));
		in->mirror_ports = reinterpret_cast<uint16_t*>(take(m.port_count * sizeof(uint16_t)));
		in->input_value = reinterpret_cast<double*>(take(m.port_count * sizeof(double)));
		in->input_quality = reinterpret_cast<Quality*>(take(m.port_count * sizeof(Quality)));
	}
	rt->interest = reinterpret_cast<uint8_t*>(base + offset);
	rt->interest_bits = interest_bits;

	Runtime* old = runtime_;
	BlockSet old_set = std::move(set_);
	bool boot = !booted_;
	set_ = std::move(next);
	runtime_ = rt;
	size_t persisting = 0;

	for (size_t i = 0; i < count; ++i) {
		const BlockSpec& spec = set_.at(i);
		const Manifest& m = spec.type->manifest();
		Instance& in = rt->instances[i];
		for (size_t f = 0; f < m.state_count; ++f) {
			in.state[f] = m.state[f].initial;
			in.stored[f] = m.state[f].initial;
		}
		for (size_t p = 0; p < m.param_count; ++p) {
			in.params[p] = spec.defaults[p];
		}
		in.enabled = spec.enabled;
		persisting += m.persists() ? 1 : 0;
		int o = old != nullptr ? old_set.find(spec.id) : -1;
		char key[BlockLimits::kIdLength + 2];
		store_key(spec.id, key, sizeof(key));
		if (o >= 0) {
			const BlockSpec& old_spec = old_set.at(static_cast<size_t>(o));
			const Manifest& om = old_spec.type->manifest();
			Instance& oi = old->instances[o];
			// Diagnostics follow the id.
			in.calls = oi.calls;
			in.time_ms = oi.time_ms;
			in.longest_ms = oi.longest_ms;
			in.errors = oi.errors;
			in.has_error = oi.has_error;
			in.last_error_code = oi.last_error_code;
			in.last_error_ms = oi.last_error_ms;
			std::memcpy(in.last_error, oi.last_error, sizeof(in.last_error));
			in.last_call = oi.last_call;
			in.last_call_ms = oi.last_call_ms;
			if (oi.status == BlockStatus::fault && std::strcmp(om.type, m.type) == 0 && om.major == m.major) {
				// A faulted instance starts from its persisted record, as a restart (spec/blocks.md B11).
				bool mismatch = false;
				in.restored = load_record(i, mismatch);
				in.written_once = oi.written_once;
				in.last_write_ms = oi.last_write_ms;
			} else if (std::strcmp(om.type, m.type) == 0 && om.major == m.major) {
				// The same type and major version: the record in RAM, by field id (spec/blocks.md 8.1).
				for (size_t f = 0; f < m.state_count; ++f) {
					int of = om.field(m.state[f].id);
					if (of >= 0) {
						in.state[f] = oi.state[of];
						in.stored[f] = oi.stored[of];
					}
				}
				in.dirty = oi.dirty;
				in.last_write_ms = oi.last_write_ms;
				in.written_once = oi.written_once;
				in.restored = true;
			} else {
				log(i, BlockLogCode::state_reset, LogLevel::warn, "the block type changed from %s %u.%u to %s %u.%u: the "
					"state starts fresh", om.type, static_cast<unsigned>(om.major), static_cast<unsigned>(old_spec.minor),
					m.type, static_cast<unsigned>(m.major), static_cast<unsigned>(spec.minor));
				char name[persist::kBlobNameSize];
				persist::blob_name(kBlobPrefix, spec.id, name);
				blobs_.remove(name);
			}
			// Overrides from RAM, checked against the new manifest (spec/blocks.md 8.2).
			for (size_t op = 0; op < om.param_count; ++op) {
				if ((oi.overridden & (1u << op)) == 0) {
					continue;
				}
				int p = m.param(om.params[op].id);
				const char* why = nullptr;
				if (p < 0) {
					why = "gone";
				} else if (!m.params[p].adjustable) {
					why = "not adjustable";
				} else if (m.params[p].type != om.params[op].type) {
					why = "type changed";
				} else if (!param_valid(m.params[p], oi.params[op], &why)) {
					why = "out of bounds";
				}
				if (why == nullptr) {
					in.params[p] = oi.params[op];
					in.overridden |= 1u << p;
				} else {
					store_.erase(key, om.params[op].id);
					log(i, BlockLogCode::parameter_reset, LogLevel::warn, "override %.6g of '%s' dropped: %s",
						oi.params[op], om.params[op].id, why);
				}
			}
			if (oi.enabled_overridden) {
				if (spec.enabled_adjustable) {
					in.enabled = oi.enabled;
					in.enabled_overridden = true;
				} else {
					store_.erase(key, "enabled");
					log(i, BlockLogCode::parameter_reset, LogLevel::warn, "override %s of 'enabled' dropped: not adjustable",
						oi.enabled ? "true" : "false");
				}
			}
		} else if (boot) {
			bool mismatch = false;
			in.restored = load_record(i, mismatch);
			for (size_t p = 0; p < m.param_count; ++p) {
				rules::Val stored;
				if (!store_.read(key, m.params[p].id, stored)) {
					continue;
				}
				double v = 0;
				const char* why = nullptr;
				if (!m.params[p].adjustable) {
					why = "not adjustable";
				} else if (!from_val(m.params[p].type, stored, v)) {
					why = "type changed";
				} else if (!param_valid(m.params[p], v, &why)) {
					why = "out of bounds";
				}
				if (why == nullptr) {
					in.params[p] = v;
					in.overridden |= 1u << p;
				} else {
					store_.erase(key, m.params[p].id);
					char text[48];
					rules::format_val(stored, text, sizeof(text));
					log(i, BlockLogCode::parameter_reset, LogLevel::warn, "override %s of '%s' dropped: %s", text,
						m.params[p].id, why);
				}
			}
			rules::Val stored;
			if (store_.read(key, "enabled", stored)) {
				if (spec.enabled_adjustable && stored.type == rules::Type::boolean) {
					in.enabled = stored.u.b;
					in.enabled_overridden = true;
				} else {
					store_.erase(key, "enabled");
					log(i, BlockLogCode::parameter_reset, LogLevel::warn, "override of 'enabled' dropped: %s",
						spec.enabled_adjustable ? "type changed" : "not adjustable");
				}
			}
			// Overrides of parameters the manifest no longer has (the prune below deletes them).
			GoneOverrides gone(settings_, *this, static_cast<int>(i), [](BlockRuntime& r, int b, const char* parameter) {
				r.log(static_cast<size_t>(b), BlockLogCode::parameter_reset, LogLevel::warn,
					"override of '%s' dropped: gone", parameter);
			});
			settings_.visit(gone);
		}
		in.status = in.enabled ? BlockStatus::running : BlockStatus::disabled;
		in.started = false;
	}
	if (old != nullptr) {
		// The overrides of parameters a new manifest no longer has were logged above; those of
		// removed blocks go silently with their records (prune below).
		release(old);
	}
	booted_ = true;
	uint32_t paced = static_cast<uint32_t>(persisting * 3600000u / BlockLimits::kRecordWritesPerHour);
	persist_interval_ms_ = paced;
	for (size_t i = 0; i < count; ++i) {
		const Manifest& m = set_.at(i).type->manifest();
		if (m.persists() && m.persist_every_ms > persist_interval_ms_) {
			persist_interval_ms_ = m.persist_every_ms;
		}
	}
	prune(false);
	bind();
	schedule_persist();
	engine_.deadlines_changed();
	return true;
}

void BlockRuntime::prune(bool forget_all) {
	OverrideKeeper keeper(*this, forget_all);
	store_.prune(keeper);
	RecordKeeper records(set_, forget_all);
	persist::delete_orphans(blobs_, kBlobPrefix, records, kTag);
}

void BlockRuntime::clear(bool forget) {
	if (!forget) {
		flush();
	}
	if (persist_timer_.valid()) {
		loop_.cancel(persist_timer_);
		persist_timer_ = TimerHandle();
	}
	release(runtime_);
	runtime_ = nullptr;
	set_.clear();
	if (forget) {
		prune(true);
	}
	booted_ = false; // the next image reads the persisted records and overrides again
	engine_.deadlines_changed();
}

void BlockRuntime::bind() {
	if (runtime_ == nullptr) {
		return;
	}
	std::memset(runtime_->interest, 0, (runtime_->interest_bits + 7) / 8);
	engine_.quietly([this] {
		for (size_t b = 0; b < runtime_->count; ++b) {
			const BlockSpec& spec = set_.at(b);
			const Manifest& m = spec.type->manifest();
			Instance& in = runtime_->instances[b];
			auto bind_one = [&](const char* member, ValueType type) -> uint16_t {
				char name[Datapoint::kMaxName + 1];
				if (!mirror_name(spec.id, member, name, sizeof(name))) {
					return kNone;
				}
				const Datapoint* dp = model_.find(name);
				if (dp == nullptr) {
					return kNone;
				}
				if (dp->type() != type) {
					Log::error(kTag, "datapoint '%s' is %s, the block needs %s: not mirrored", name,
						value_type_name(dp->type()), value_type_name(type));
					return kNone;
				}
				if (model_.bind(dp->id(), technology_) != DatapointStatus::ok) {
					Log::error(kTag, "datapoint '%s' is owned by another technology: not mirrored", name);
					return kNone;
				}
				return dp->id();
			};
			in.mirror_enabled = bind_one("enabled", ValueType::boolean);
			publish_mirror(in.mirror_enabled, ValueType::boolean, in.enabled ? 1 : 0, Quality::good);
			for (size_t p = 0; p < m.param_count; ++p) {
				in.mirror_params[p] = kNone;
				if (m.params[p].adjustable) {
					ValueType type = param_datapoint_type(m.params[p].type);
					in.mirror_params[p] = bind_one(m.params[p].id, type);
					publish_mirror(in.mirror_params[p], type, in.params[p], Quality::good);
				}
			}
			for (size_t p = 0; p < m.port_count; ++p) {
				const PortDecl& decl = m.ports[p];
				const PortBinding& binding = spec.ports[p];
				in.mirror_ports[p] = kNone;
				if (decl.direction == Direction::out) {
					in.mirror_ports[p] = bind_one(decl.id, port_datapoint_type(decl.type));
					continue;
				}
				for (size_t k = 0; k < binding.count; ++k) {
					uint16_t id = binding.datapoints[k];
					if (id < runtime_->interest_bits) {
						runtime_->interest[id / 8] = static_cast<uint8_t>(runtime_->interest[id / 8] | (1u << (id % 8)));
					}
				}
				InputValue v = aggregate(in, p);
				in.input_value[p] = v.value;
				in.input_quality[p] = v.quality;
				if (binding.count > 1) {
					in.mirror_ports[p] = bind_one(decl.id, port_datapoint_type(decl.type));
					publish_mirror(in.mirror_ports[p], port_datapoint_type(decl.type), v.value, v.quality);
				}
			}
		}
	});
}

// ------------------------------------------------------------------------------------------
// Inputs and outputs

InputValue BlockRuntime::aggregate(const Instance& instance, size_t port) const {
	size_t block = static_cast<size_t>(&instance - runtime_->instances);
	const BlockSpec& spec = set_.at(block);
	const PortBinding& binding = spec.ports[port];
	InputValue v;
	v.bound = binding.count > 0;
	const Quality wanted[] = {Quality::good, Quality::stale};
	for (Quality want : wanted) {
		size_t n = 0;
		double acc = 0;
		uint64_t updated = 0;
		for (size_t k = 0; k < binding.count; ++k) {
			const Datapoint* dp = model_.get(binding.datapoints[k]);
			if (dp == nullptr || dp->quality() != want) {
				continue;
			}
			double x = number_of(dp->value());
			if (n == 0) {
				acc = x;
			} else {
				switch (binding.aggregation) {
					case Aggregation::mean: acc = acc + x; break;
					case Aggregation::min: acc = x < acc ? x : acc; break;
					case Aggregation::max: acc = x > acc ? x : acc; break;
					case Aggregation::any: acc = (acc != 0 || x != 0) ? 1 : 0; break;
					case Aggregation::all: acc = (acc != 0 && x != 0) ? 1 : 0; break;
					case Aggregation::none: break;
				}
			}
			updated = dp->updated_ms() > updated ? dp->updated_ms() : updated;
			++n;
		}
		if (n > 0) {
			if (binding.aggregation == Aggregation::mean && n > 1) {
				acc = acc / static_cast<double>(n);
			} else if (n == 1 && (binding.aggregation == Aggregation::any || binding.aggregation == Aggregation::all)) {
				acc = acc != 0 ? 1 : 0;
			}
			v.value = acc;
			v.quality = want;
			v.updated_ms = updated;
			return v;
		}
	}
	return v;
}

void BlockRuntime::publish_mirror(uint16_t id, ValueType type, double value, Quality quality) {
	const Datapoint* dp = id != kNone ? model_.get(id) : nullptr;
	if (dp == nullptr || quality == Quality::unknown) {
		return;
	}
	std::optional<Value> v = value_of(type, value);
	if (!v) {
		Log::error(kTag, "%s: the value %.6g does not fit the %s datapoint; it keeps its previous value", dp->name(), value,
			value_type_name(type));
		return;
	}
	if (dp->quality() == quality && dp->value() == *v) {
		return;
	}
	if (dp->quality() != Quality::good || dp->value() != *v) {
		DatapointStatus status = model_.publish(technology_, id, *v);
		if (status != DatapointStatus::ok) {
			Log::error(kTag, "%s: not published: %s", dp->name(), datapoint_status_name(status));
			return;
		}
	}
	if (quality == Quality::stale) {
		model_.report_stale(technology_, id);
	}
}

void BlockRuntime::write_output(size_t block, size_t port, double value) {
	const BlockSpec& spec = set_.at(block);
	const PortDecl& decl = spec.type->manifest().ports[port];
	Instance& in = runtime_->instances[block];
	const PortBinding& binding = spec.ports[port];
	if (binding.count > 0) {
		const Datapoint* dp = model_.get(binding.datapoints[0]);
		std::optional<Value> v = dp != nullptr ? value_of(dp->type(), value) : std::nullopt;
		if (dp == nullptr) {
			log(block, BlockLogCode::write_refused, LogLevel::error, "output '%s': its datapoint is not in the model", decl.id);
		} else if (!v) {
			log(block, BlockLogCode::write_refused, LogLevel::error, "output '%s': %.6g does not fit the %s datapoint '%s'",
				decl.id, value, value_type_name(dp->type()), dp->name());
		} else {
			DatapointStatus status = model_.command(binding.datapoints[0], *v);
			if (status != DatapointStatus::ok) {
				log(block, BlockLogCode::write_refused, LogLevel::error, "output '%s': command to '%s' refused: %s", decl.id,
					dp->name(), datapoint_status_name(status));
			} else if (observer_) {
				observer_->block_commanded(spec.id, *dp, *v);
			}
		}
	}
	publish_mirror(in.mirror_ports[port], port_datapoint_type(decl.type), value, Quality::good);
}

void BlockRuntime::write_failsafe(size_t block) {
	const Manifest& m = set_.at(block).type->manifest();
	for (size_t p = 0; p < m.port_count; ++p) {
		if (m.ports[p].direction == Direction::out && !m.ports[p].diagnostic) {
			write_output(block, p, m.ports[p].failsafe);
		}
	}
}

// ------------------------------------------------------------------------------------------
// Calls

bool BlockRuntime::call(size_t block, CallKind kind, size_t arg, bool derived, StartReason reason, bool restored) {
	Instance& in = runtime_->instances[block];
	const BlockSpec& spec = set_.at(block);
	const Manifest& m = spec.type->manifest();
	if (derived) {
		const char* culprit = "-";
		uint32_t firings = 0;
		if (!engine_.derived_call(culprit, firings)) {
			log(block, BlockLogCode::loop_guard, LogLevel::error,
				"call refused after %u derived firings (most: %s with %u); the cascade is aborted",
				static_cast<unsigned>(rules::Limits::kDerivedFirings), culprit, static_cast<unsigned>(firings));
			if (observer_) {
				observer_->loop_guard(spec.id, culprit, firings);
			}
			return false;
		}
		++in.cascade_calls;
	}
	Context cx(*this, block);
	uint64_t begin = clock_.monotonic_ms();
	const BlockType& type = *spec.type;
	const char* what = "";
	switch (kind) {
		case CallKind::start:
			what = start_reason_name(reason);
			in.started = true;
			type.start(cx, reason, restored);
			break;
		case CallKind::input:
			what = m.ports[arg].id;
			type.input(cx, arg);
			break;
		case CallKind::timer:
			what = m.timers[arg];
			type.timer(cx, arg);
			break;
		case CallKind::period: type.period(cx); break;
		case CallKind::parameter:
			what = m.params[arg].id;
			type.parameter(cx, arg);
			break;
		case CallKind::none: break;
	}
	uint64_t used = clock_.monotonic_ms() - begin;
	++in.calls;
	in.last_call = kind;
	in.last_call_ms = cx.now();
	in.last_call_arg = what;
	in.time_ms += static_cast<uint32_t>(used);
	in.longest_ms = used > in.longest_ms ? static_cast<uint32_t>(used) : in.longest_ms;
	for (size_t f = 0; f < m.state_count && !cx.faulted; ++f) {
		if (!std::isfinite(in.state[f])) {
			char text[64];
			std::snprintf(text, sizeof(text), "the state field '%s' is not finite", m.state[f].id);
			cx.fault(text);
		}
	}
	if (cx.faulted) {
		fault(block, cx.reason());
		return true;
	}
	record_changed(block);
	return true;
}

void BlockRuntime::stop(size_t block, BlockStatus status) {
	Instance& in = runtime_->instances[block];
	in.status = status;
	in.started = false;
	in.running = 0;
	in.period_armed = false;
	write_failsafe(block);
}

void BlockRuntime::fault(size_t block, const char* reason) {
	Instance& in = runtime_->instances[block];
	std::snprintf(in.fault, sizeof(in.fault), "%s", reason);
	in.fault_ms = engine_.in_cascade() ? engine_.cascade_ms() : clock_.monotonic_ms();
	in.dirty = false;
	log(block, BlockLogCode::fault, LogLevel::error, "%s; stopped, the outputs at their failsafe values", reason);
	stop(block, BlockStatus::fault);
}

bool BlockRuntime::wants(uint16_t datapoint) const {
	return runtime_ != nullptr && datapoint < runtime_->interest_bits &&
		(runtime_->interest[datapoint / 8] & (1u << (datapoint % 8))) != 0;
}

void BlockRuntime::datapoint_event(uint16_t datapoint, bool derived) {
	if (!wants(datapoint)) {
		return;
	}
	for (size_t b = 0; b < runtime_->count; ++b) {
		const BlockSpec& spec = set_.at(b);
		const Manifest& m = spec.type->manifest();
		Instance& in = runtime_->instances[b];
		for (size_t p = 0; p < m.port_count; ++p) {
			const PortBinding& binding = spec.ports[p];
			if (m.ports[p].direction != Direction::in) {
				continue;
			}
			bool bound = false;
			for (size_t k = 0; k < binding.count && !bound; ++k) {
				bound = binding.datapoints[k] == datapoint;
			}
			if (!bound) {
				continue;
			}
			InputValue v = aggregate(in, p);
			bool changed = v.value != in.input_value[p] || v.quality != in.input_quality[p];
			in.input_value[p] = v.value;
			in.input_quality[p] = v.quality;
			if (binding.count > 1 && changed) {
				publish_mirror(in.mirror_ports[p], port_datapoint_type(m.ports[p].type), v.value, v.quality);
			}
			InputTrigger trigger = m.ports[p].trigger;
			if (in.status == BlockStatus::running && in.started &&
				(trigger == InputTrigger::update || (trigger == InputTrigger::change && changed))) {
				call(b, CallKind::input, p, derived);
			}
			if (engine_.aborted()) {
				return;
			}
		}
	}
}

void BlockRuntime::block_event(uint8_t kind, uint16_t block, uint16_t arg, bool derived) {
	if (runtime_ == nullptr || block >= runtime_->count) {
		return;
	}
	Instance& in = runtime_->instances[block];
	const Manifest& m = set_.at(block).type->manifest();
	switch (kind) {
		case kParameterChanged:
			// The value is the parameter's already; its mirror, then the call (spec/blocks.md 5.1).
			if (arg < m.param_count) {
				publish_mirror(in.mirror_params[arg], param_datapoint_type(m.params[arg].type), in.params[arg],
					Quality::good);
				if (in.status == BlockStatus::running && in.started) {
					call(block, CallKind::parameter, arg, derived);
				}
			}
			break;
		case kEnabledChanged:
			publish_mirror(in.mirror_enabled, ValueType::boolean, in.enabled ? 1 : 0, Quality::good);
			if (in.status == BlockStatus::fault) {
				break; // a fault ends with a restart or a new image only (spec/blocks.md 7)
			}
			if (in.enabled && in.status == BlockStatus::disabled) {
				in.status = BlockStatus::running;
				in.running = 0;
				in.period_next = engine_.cascade_ms() + m.period_ms;
				in.period_armed = m.period_ms > 0;
				call(block, CallKind::start, 0, derived, StartReason::enable, true);
			} else if (!in.enabled && in.status != BlockStatus::disabled) {
				stop(block, BlockStatus::disabled);
			}
			break;
		case kRestart:
			if (in.status == BlockStatus::fault) {
				// As at boot: the persisted record, the other fields fresh (spec/blocks.md B11).
				for (size_t f = 0; f < m.state_count; ++f) {
					in.state[f] = m.state[f].initial;
					in.stored[f] = m.state[f].initial;
				}
				bool mismatch = false;
				bool restored = load_record(block, mismatch);
				in.fault[0] = '\0';
				if (!in.enabled) {
					in.status = BlockStatus::disabled; // its outputs are at their failsafe values already
					break;
				}
				in.status = BlockStatus::running;
				in.running = 0;
				in.period_next = engine_.cascade_ms() + m.period_ms;
				in.period_armed = m.period_ms > 0;
				call(block, CallKind::start, 0, derived, StartReason::restart, restored);
			}
			break;
		default: break;
	}
}

uint64_t BlockRuntime::next_deadline() const {
	uint64_t earliest = EventLoop::kNever;
	for (size_t b = 0; runtime_ != nullptr && b < runtime_->count; ++b) {
		const Instance& in = runtime_->instances[b];
		if (in.status != BlockStatus::running) {
			continue;
		}
		for (size_t t = 0; t < BlockLimits::kTimers; ++t) {
			if ((in.running & (1u << t)) != 0 && in.timers[t] < earliest) {
				earliest = in.timers[t];
			}
		}
		if (in.period_armed && in.period_next < earliest) {
			earliest = in.period_next;
		}
	}
	return earliest;
}

void BlockRuntime::run_due(uint64_t now) {
	if (runtime_ == nullptr) {
		return;
	}
	// The earliest deadline; on a tie the first block, within it the timers in manifest order, then
	// the period (spec/blocks.md 5.1).
	size_t best_block = SIZE_MAX;
	size_t best_slot = 0;
	uint64_t best = EventLoop::kNever;
	for (size_t b = 0; b < runtime_->count; ++b) {
		const Instance& in = runtime_->instances[b];
		if (in.status != BlockStatus::running) {
			continue;
		}
		size_t timers = set_.at(b).type->manifest().timer_count;
		for (size_t t = 0; t <= timers; ++t) {
			bool armed = t < timers ? (in.running & (1u << t)) != 0 : in.period_armed;
			uint64_t deadline = t < timers ? in.timers[t] : in.period_next;
			if (armed && deadline <= now && deadline < best) {
				best = deadline;
				best_block = b;
				best_slot = t;
			}
		}
	}
	if (best_block == SIZE_MAX) {
		return;
	}
	Instance& in = runtime_->instances[best_block];
	const Manifest& m = set_.at(best_block).type->manifest();
	if (best_slot < m.timer_count) {
		in.running = static_cast<uint8_t>(in.running & ~(1u << best_slot));
		call(best_block, CallKind::timer, best_slot, false);
	} else {
		uint64_t next = in.period_next + m.period_ms;
		while (next <= now) {
			next += m.period_ms; // a period that passed while the loop was busy is called once
		}
		in.period_next = next;
		call(best_block, CallKind::period, 0, false);
	}
}

void BlockRuntime::start_block(size_t index, rules::StartReason reason) {
	if (runtime_ == nullptr || index >= runtime_->count) {
		return;
	}
	Instance& in = runtime_->instances[index];
	const Manifest& m = set_.at(index).type->manifest();
	if (!in.enabled) {
		in.status = BlockStatus::disabled;
		in.started = false;
		write_failsafe(index); // a disabled block's outputs are defined (spec/blocks.md B10)
		return;
	}
	in.status = BlockStatus::running;
	in.running = 0;
	in.period_next = engine_.cascade_ms() + m.period_ms;
	in.period_armed = m.period_ms > 0;
	bool restored = in.restored;
	in.restored = true; // a later start (enable) has the record in RAM
	call(index, CallKind::start, 0, false, reason == rules::StartReason::boot ? StartReason::boot : StartReason::image,
		restored);
}

void BlockRuntime::cascade_begun() {
	for (size_t b = 0; runtime_ != nullptr && b < runtime_->count; ++b) {
		runtime_->instances[b].cascade_calls = 0;
	}
}

bool BlockRuntime::busiest(const char*& id, uint32_t& calls) const {
	bool found = false;
	for (size_t b = 0; runtime_ != nullptr && b < runtime_->count; ++b) {
		uint32_t n = runtime_->instances[b].cascade_calls;
		if (n > 0 && (!found || n > calls)) {
			found = true;
			id = set_.at(b).id;
			calls = n;
		}
	}
	return found;
}

// ------------------------------------------------------------------------------------------
// Changes from the UI, the API and the mirrors' commands

void BlockRuntime::refuse(const char* block, const char* parameter, const char* reason) {
	int b = find(block);
	char text[128];
	std::snprintf(text, sizeof(text), "parameter '%s' refused: %s", parameter, reason);
	log_text(block, b >= 0 ? &runtime_->instances[b] : nullptr, BlockLogCode::parameter_refused, LogLevel::warn, text);
}

ChangeStatus BlockRuntime::find_param(const char* block, const char* parameter, int& b, int& p,
	const char*& reason) const {
	b = runtime_ != nullptr ? find(block) : -1;
	p = -1;
	if (b < 0) {
		reason = "no such block";
		return ChangeStatus::unknown_block;
	}
	const Manifest& m = set_.at(static_cast<size_t>(b)).type->manifest();
	p = m.param(parameter);
	if (p < 0) {
		reason = "no such parameter";
		return ChangeStatus::unknown_parameter;
	}
	if (!m.params[p].adjustable) {
		reason = "not adjustable";
		return ChangeStatus::not_adjustable;
	}
	return ChangeStatus::ok;
}

void BlockRuntime::apply_param(size_t block, size_t index, double value) {
	const BlockSpec& spec = set_.at(block);
	const ParamDecl& decl = spec.type->manifest().params[index];
	Instance& in = runtime_->instances[block];
	double old = in.params[index];
	bool was = (in.overridden & (1u << index)) != 0;
	char key[BlockLimits::kIdLength + 2];
	store_key(spec.id, key, sizeof(key));
	in.params[index] = value;
	if (value == spec.defaults[index]) {
		in.overridden &= ~(1u << index); // the default is no override (spec/rules.md D5)
		if (was) {
			store_.erase(key, decl.id);
		}
	} else {
		in.overridden |= 1u << index;
		if (!was || old != value) {
			store_.write(key, decl.id, to_val(decl.type, value));
		}
	}
	if (old != value) {
		engine_.queue_block_event(kParameterChanged, static_cast<uint16_t>(block), static_cast<uint16_t>(index));
	}
}

void BlockRuntime::apply_enabled(size_t block, bool enabled) {
	const BlockSpec& spec = set_.at(block);
	Instance& in = runtime_->instances[block];
	bool old = in.enabled;
	char key[BlockLimits::kIdLength + 2];
	store_key(spec.id, key, sizeof(key));
	in.enabled = enabled;
	if (enabled == spec.enabled) {
		if (in.enabled_overridden) {
			store_.erase(key, "enabled");
		}
		in.enabled_overridden = false;
	} else {
		if (!in.enabled_overridden || old != enabled) {
			store_.write(key, "enabled", rules::Val::of_bool(enabled));
		}
		in.enabled_overridden = true;
	}
	if (old != enabled) {
		engine_.queue_block_event(kEnabledChanged, static_cast<uint16_t>(block), enabled ? 1 : 0);
	}
}

ChangeStatus BlockRuntime::check_parameter(const char* block, const char* parameter, const JsonSpan* value,
	double* out, const char** reason_out) const {
	int b = -1, p = -1;
	const char* reason = "";
	ChangeStatus status = find_param(block, parameter, b, p, reason);
	if (status == ChangeStatus::ok && value != nullptr) {
		const ParamDecl& decl = set_.at(static_cast<size_t>(b)).type->manifest().params[p];
		double v = 0;
		if (!param_from_json(decl, *value, v, &reason) || !param_valid(decl, v, &reason)) {
			status = ChangeStatus::invalid_value;
		} else if (out != nullptr) {
			*out = v;
		}
	}
	*reason_out = reason;
	return status;
}

ChangeStatus BlockRuntime::check_enabled(const char* block, const char** reason) const {
	int b = runtime_ != nullptr ? find(block) : -1;
	if (b < 0) {
		*reason = "no such block";
		return ChangeStatus::unknown_block;
	}
	if (!set_.at(static_cast<size_t>(b)).enabled_adjustable) {
		*reason = "not adjustable";
		return ChangeStatus::not_adjustable;
	}
	return ChangeStatus::ok;
}

ChangeStatus BlockRuntime::check_restart(const char* block, const char** reason) const {
	int b = runtime_ != nullptr ? find(block) : -1;
	if (b < 0) {
		*reason = "no such block";
		return ChangeStatus::unknown_block;
	}
	if (runtime_->instances[b].status != BlockStatus::fault) {
		*reason = "the block has not faulted";
		return ChangeStatus::not_faulted;
	}
	return ChangeStatus::ok;
}

ChangeStatus BlockRuntime::set_parameter(const char* block, const char* parameter, const JsonSpan& value,
	const char** reason_out) {
	int b = -1, p = -1;
	const char* reason = "";
	ChangeStatus status = find_param(block, parameter, b, p, reason);
	double v = 0;
	if (status == ChangeStatus::ok &&
		!param_from_json(set_.at(static_cast<size_t>(b)).type->manifest().params[p], value, v, &reason)) {
		status = ChangeStatus::invalid_value;
	}
	if (status != ChangeStatus::ok) {
		refuse(block, parameter, reason);
		*reason_out = reason;
		return status;
	}
	return set_parameter(block, parameter, v, reason_out);
}

ChangeStatus BlockRuntime::set_parameter(const char* block, const char* parameter, double value,
	const char** reason_out) {
	int b = -1, p = -1;
	const char* reason = "";
	ChangeStatus status = find_param(block, parameter, b, p, reason);
	if (status == ChangeStatus::ok && !param_valid(set_.at(static_cast<size_t>(b)).type->manifest().params[p], value, &reason)) {
		status = ChangeStatus::invalid_value;
	}
	if (status != ChangeStatus::ok) {
		refuse(block, parameter, reason);
		*reason_out = reason;
		return status;
	}
	apply_param(static_cast<size_t>(b), static_cast<size_t>(p), value);
	return ChangeStatus::ok;
}

ChangeStatus BlockRuntime::reset_parameter(const char* block, const char* parameter, const char** reason_out) {
	int b = -1, p = -1;
	const char* reason = "";
	ChangeStatus status = find_param(block, parameter, b, p, reason);
	if (status != ChangeStatus::ok) {
		refuse(block, parameter, reason);
		*reason_out = reason;
		return status;
	}
	apply_param(static_cast<size_t>(b), static_cast<size_t>(p), set_.at(static_cast<size_t>(b)).defaults[p]);
	return ChangeStatus::ok;
}

ChangeStatus BlockRuntime::set_enabled(const char* block, bool enabled, const char** reason_out) {
	int b = runtime_ != nullptr ? find(block) : -1;
	if (b < 0) {
		*reason_out = "no such block";
		refuse(block, "enabled", *reason_out);
		return ChangeStatus::unknown_block;
	}
	if (!set_.at(static_cast<size_t>(b)).enabled_adjustable) {
		*reason_out = "not adjustable";
		refuse(block, "enabled", *reason_out);
		return ChangeStatus::not_adjustable;
	}
	apply_enabled(static_cast<size_t>(b), enabled);
	return ChangeStatus::ok;
}

ChangeStatus BlockRuntime::restart(const char* block, const char** reason_out) {
	int b = runtime_ != nullptr ? find(block) : -1;
	if (b < 0) {
		*reason_out = "no such block";
		return ChangeStatus::unknown_block;
	}
	if (runtime_->instances[b].status != BlockStatus::fault) {
		*reason_out = "the block has not faulted";
		return ChangeStatus::not_faulted;
	}
	engine_.queue_block_event(kRestart, static_cast<uint16_t>(b), 0);
	return ChangeStatus::ok;
}

bool BlockRuntime::mirror_command(const Datapoint& datapoint, const Value& value) {
	for (size_t b = 0; runtime_ != nullptr && b < runtime_->count; ++b) {
		const BlockSpec& spec = set_.at(b);
		const Manifest& m = spec.type->manifest();
		Instance& in = runtime_->instances[b];
		if (in.mirror_enabled == datapoint.id()) {
			if (!spec.enabled_adjustable || value.type() != ValueType::boolean) {
				refuse(spec.id, "enabled", spec.enabled_adjustable ? "not a boolean" : "not adjustable");
				return false;
			}
			apply_enabled(b, value.as_bool());
			return true;
		}
		for (size_t p = 0; p < m.param_count; ++p) {
			if (in.mirror_params[p] != datapoint.id()) {
				continue;
			}
			const char* reason = nullptr;
			double v = number_of(value);
			bool valid = value.type() == param_datapoint_type(m.params[p].type);
			if (!valid) {
				reason = "not a value of the parameter's type";
			} else {
				valid = param_valid(m.params[p], v, &reason);
			}
			if (!valid) {
				refuse(spec.id, m.params[p].id, reason);
				return false;
			}
			apply_param(b, p, v);
			return true;
		}
	}
	Log::error(kTag, "command to '%s' refused: it is not a block's parameter or enable switch", datapoint.name());
	return false;
}

// ------------------------------------------------------------------------------------------
// Accessors

bool BlockRuntime::diagnostics(size_t block, BlockDiagnostics& out) const {
	if (runtime_ == nullptr || block >= runtime_->count) {
		return false;
	}
	const Instance& in = runtime_->instances[block];
	out.status = in.status;
	out.fault = in.fault;
	out.fault_ms = in.fault_ms;
	out.calls = in.calls;
	out.last_call = in.last_call;
	out.last_call_ms = in.last_call_ms;
	out.last_call_arg = in.last_call_arg;
	out.time_ms = in.time_ms;
	out.longest_ms = in.longest_ms;
	out.errors = in.errors;
	out.has_error = in.has_error;
	out.last_error_code = in.last_error_code;
	out.last_error = in.last_error;
	out.last_error_ms = in.last_error_ms;
	out.restored = in.restored;
	return true;
}

const double* BlockRuntime::state(size_t block) const {
	return runtime_ != nullptr && block < runtime_->count ? runtime_->instances[block].state : nullptr;
}

double BlockRuntime::param(size_t block, size_t index) const { return runtime_->instances[block].params[index]; }

bool BlockRuntime::overridden(size_t block, size_t index) const {
	return (runtime_->instances[block].overridden & (1u << index)) != 0;
}

bool BlockRuntime::enabled(size_t block) const { return runtime_->instances[block].enabled; }

bool BlockRuntime::enabled_overridden(size_t block) const { return runtime_->instances[block].enabled_overridden; }

InputValue BlockRuntime::input(size_t block, size_t port) const {
	return aggregate(runtime_->instances[block], port);
}

uint16_t BlockRuntime::mirror(size_t block, size_t port) const { return runtime_->instances[block].mirror_ports[port]; }

bool BlockRuntime::timer_remaining(size_t block, size_t timer, int64_t& ms) const {
	const Instance& in = runtime_->instances[block];
	if ((in.running & (1u << timer)) == 0) {
		return false;
	}
	ms = static_cast<int64_t>(in.timers[timer]) - static_cast<int64_t>(clock_.monotonic_ms());
	return true;
}

BlockMemory BlockRuntime::memory() const {
	return BlockMemory{sizeof(BlockRuntime), runtime_ != nullptr ? runtime_->bytes : 0, set_.bytes()};
}

} // namespace hc::blocks
