/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/rules/engine.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "hc/network_settings.h"
#include "hc/rules/printer.h"

namespace hc::rules {

namespace {

constexpr const char* kTag = "hc.rule";

constexpr const char* kLogCodeNames[] = {"log", "conditionNoValue", "valueNoValue", "evalError", "writeRefused",
	"timerRefused", "parameterRefused", "parameterReset", "stateReset", "loopGuard", "queueOverflow", "readRefused"};

enum EventKind : uint8_t { kDatapoint, kSchedule, kTimer, kStart, kParameter, kStartAll, kSilence, kBlock, kBlockDue };

/// Event flags.
constexpr uint8_t kChanged = 0x01;   ///< datapoint: value or quality changed.
constexpr uint8_t kExternal = 0x80;  ///< In the cascade queue but not derived (the start of an enable).

/// Per-node flags (on expression roots).
constexpr uint8_t kEvalErrorLogged = 0x01;
constexpr uint8_t kNoValueLogged = 0x02;
constexpr uint8_t kMatchMemory = 0x04;
constexpr uint8_t kMatched = 0x08;

constexpr uint8_t kNoKind = 0xFF;

/// Per-action flags.
constexpr uint8_t kReadRefusedLogged = 0x01;

uint8_t reason_flag(StartReason reason) {
	switch (reason) {
		case StartReason::boot: return TriggerFlags::kBoot;
		case StartReason::image: return TriggerFlags::kImage;
		case StartReason::enable: return TriggerFlags::kEnable;
	}
	return 0;
}

const char* reason_name_of_flag(uint8_t flag) {
	return flag == TriggerFlags::kBoot ? "boot" : (flag == TriggerFlags::kImage ? "image" : "enable");
}

bool is_real(const Val& v) { return v.type == Type::real; }

double as_double(const Val& v) { return v.type == Type::real ? v.u.r : static_cast<double>(v.u.i); }

int sign(double d) { return d < 0 ? -1 : (d > 0 ? 1 : 0); }
int sign64(int64_t a, int64_t b) { return a < b ? -1 : (a > b ? 1 : 0); }

int seconds_of_day(const TimeOfDay& t) { return t.hour * 3600 + t.minute * 60 + t.second; }

int64_t seconds_of(const DateTime& d) {
	return days_of(d.year, d.month, d.day) * 86400 + d.hour * 3600 + d.minute * 60 + d.second;
}

/// Ordering of two present values of ordered types (numeric mix compares as real). Date-times
/// compare as instants when `zone` is given (spec 4.5), as local fields otherwise.
int order(const Val& a, const Val& b, const TimeSource* zone = nullptr) {
	if ((a.type == Type::integer || a.type == Type::real) && (b.type == Type::integer || b.type == Type::real)) {
		if (is_real(a) || is_real(b)) {
			return sign(as_double(a) - as_double(b)) == 0 ? (as_double(a) < as_double(b) ? -1 : (as_double(a) > as_double(b) ? 1 : 0))
														  : (as_double(a) < as_double(b) ? -1 : 1);
		}
		return sign64(a.u.i, b.u.i);
	}
	switch (a.type) {
		case Type::duration: return sign64(a.u.i, b.u.i);
		case Type::time_of_day: return sign64(seconds_of_day(a.u.tod), seconds_of_day(b.u.tod));
		case Type::date: {
			int64_t x = days_of(a.u.date.year, a.u.date.month, a.u.date.day);
			int64_t y = days_of(b.u.date.year, b.u.date.month, b.u.date.day);
			return sign64(x, y);
		}
		case Type::date_time:
			return zone != nullptr ? sign64(zone->local_to_utc(a.u.dt), zone->local_to_utc(b.u.dt))
								   : sign64(seconds_of(a.u.dt), seconds_of(b.u.dt));
		case Type::string: {
			size_t n = a.length < b.length ? a.length : b.length;
			int c = std::memcmp(a.u.bytes, b.u.bytes, n);
			if (c != 0) {
				return c < 0 ? -1 : 1;
			}
			return a.length < b.length ? -1 : (a.length > b.length ? 1 : 0);
		}
		default: return 0;
	}
}

/// Equality of the language (spec 4.5), numeric and enumeration/integer mixes included; date-times
/// as instants when `zone` is given.
bool equal(const Val& a, const Val& b, const TimeSource* zone = nullptr) {
	bool a_num = a.type == Type::integer || a.type == Type::real;
	bool b_num = b.type == Type::integer || b.type == Type::real;
	if (a_num && b_num) {
		if (is_real(a) || is_real(b)) {
			return as_double(a) == as_double(b);
		}
		return a.u.i == b.u.i;
	}
	if (a.type == Type::enumeration && b.type == Type::integer) {
		return static_cast<int64_t>(a.u.e) == b.u.i;
	}
	if (a.type == Type::integer && b.type == Type::enumeration) {
		return a.u.i == static_cast<int64_t>(b.u.e);
	}
	if (zone != nullptr && a.type == Type::date_time && b.type == Type::date_time) {
		return zone->local_to_utc(a.u.dt) == zone->local_to_utc(b.u.dt);
	}
	return val_equal(a, b);
}

Val promote(const Val& v, Type type) {
	if (v.present && type == Type::real && v.type == Type::integer) {
		return Val::of_real(static_cast<double>(v.u.i));
	}
	return v;
}

bool contains_bytes(const Val& s, const Val& part) {
	if (part.length == 0) {
		return true;
	}
	for (size_t i = 0; i + part.length <= s.length; ++i) {
		if (std::memcmp(s.u.bytes + i, part.u.bytes, part.length) == 0) {
			return true;
		}
	}
	return false;
}

} // namespace

const char* start_reason_name(StartReason reason) { return reason_name_of_flag(reason_flag(reason)); }

const char* log_code_name(LogCode code) {
	return static_cast<size_t>(code) < sizeof(kLogCodeNames) / sizeof(kLogCodeNames[0])
		? kLogCodeNames[static_cast<size_t>(code)]
		: "?";
}

const char* parameter_status_name(ParameterStatus status) {
	switch (status) {
		case ParameterStatus::ok: return "ok";
		case ParameterStatus::unknown_rule: return "unknown rule";
		case ParameterStatus::unknown_parameter: return "unknown parameter";
		case ParameterStatus::not_adjustable: return "not adjustable";
		case ParameterStatus::invalid_value: return "invalid value";
	}
	return "?";
}

// ------------------------------------------------------------------------------------------
// Runtime structures

struct Engine::RuleRuntime {
	uint64_t last_trigger_ms = 0;
	uint64_t last_action_ms = 0;
	uint64_t last_error_ms = 0;
	DateTime last_trigger_wall;
	Val last_action_value;
	const char* last_error_text = "";
	uint32_t firings = 0;
	uint32_t errors = 0;
	uint32_t peak_cascade_firings = 0;
	uint32_t loop_guards = 0;
	uint16_t cascade_firings = 0;
	uint16_t last_trigger_ref = 0;
	uint16_t last_action_target = 0;
	uint8_t last_trigger_kind = kNoKind;
	uint8_t last_trigger_flags = 0;
	uint8_t last_action_kind = kNoKind;
	LogCode last_error_code = LogCode::log;
	bool has_error = false;
	bool last_trigger_wall_valid = false;
	uint8_t state = kNoState;
	bool has_state = false;
	bool state_dirty = false; ///< The state changed; its write is delayed (spec 8.5).
};

struct Engine::ParamRuntime {
	Val value;
	bool overridden = false;
	uint16_t datapoint = kNone; ///< The parameter datapoint bound to the engine (spec 8.6).
};

struct Engine::TimerRuntime {
	uint64_t deadline = 0;
	uint32_t sequence = 0;
	uint8_t rule = 0;
	bool running = false;
};

struct Engine::ChangeRecord {
	uint64_t time = 0;
	bool known = false;
};

struct Engine::SilenceRuntime {
	uint64_t base = 0;     ///< Monotonic time the silent period counts from.
	uint64_t deadline = 0;
	uint32_t sequence = 0; ///< Arming order, for equal deadlines.
	bool armed = false;
	bool fired = false;    ///< Fired for this period: armed again by the next update.
	bool refused_logged = false;
};

namespace {

/// A table of entries in chunks of kPer (homecontrol #70: the per-rule and per-parameter tables grow
/// by about 100 and 40 bytes per entry; one block of them is what a fragmented heap refuses).
template <class T>
struct Chunks {
	static constexpr size_t kPer = 16;
	T** slots = nullptr;
	size_t chunks = 0;

	explicit operator bool() const { return slots != nullptr; }
	T& operator[](size_t i) const { return slots[i / kPer][i % kPer]; }

	/// Room for `count` entries, zeroed (no objects constructed); false without memory (what it got
	/// is freed by release()). `bytes` grows by what it took.
	bool allocate(size_t count, size_t& bytes) {
		if (count == 0) {
			return true;
		}
		size_t n = (count + kPer - 1) / kPer;
		slots = new (std::nothrow) T*[n]();
		if (slots == nullptr) {
			return false;
		}
		chunks = n;
		bytes += n * sizeof(T*);
		for (size_t k = 0; k < n; ++k) {
			size_t size = (count - k * kPer < kPer ? count - k * kPer : kPer) * sizeof(T);
			void* p = ::operator new(size, std::nothrow);
			if (p == nullptr) {
				return false;
			}
			std::memset(p, 0, size);
			slots[k] = static_cast<T*>(p);
			bytes += size;
		}
		return true;
	}
	void release() {
		for (size_t k = 0; slots != nullptr && k < chunks; ++k) {
			::operator delete(static_cast<void*>(slots[k]), std::nothrow);
		}
		delete[] slots;
		slots = nullptr;
		chunks = 0;
	}
};

} // namespace

struct Engine::Runtime {
	size_t bytes = 0;
	Chunks<RuleRuntime> rules;
	Chunks<ParamRuntime> params;
	TimerRuntime* timers = nullptr;
	uint8_t* node_flags = nullptr;
	ChangeRecord* changes = nullptr;
	uint8_t* interest = nullptr; ///< Bit per datapoint id: referenced by a trigger or a match condition.
	size_t interest_bits = 0;
	uint16_t* matched = nullptr; ///< Match roots that matched the current event.
	SilenceRuntime* silences = nullptr;
	uint8_t* action_flags = nullptr;
	size_t rule_count = 0, param_count = 0, timer_count = 0, silence_count = 0;
	/// One allocation per table (homecontrol #70: not one block of all, which grew with the rules and
	/// which a fragmented heap refuses during a swap); the destructor frees them (and destroys the
	/// objects of the counts set).
	static constexpr size_t kParts = 10;
	void* parts[kParts] = {};

	Runtime() = default;
	Runtime(const Runtime&) = delete;
	Runtime& operator=(const Runtime&) = delete;
	~Runtime() {
		for (size_t i = 0; rules && i < rule_count; ++i) {
			rules[i].~RuleRuntime();
		}
		for (size_t i = 0; params && i < param_count; ++i) {
			params[i].~ParamRuntime();
		}
		rules.release();
		params.release();
		for (void* part : parts) {
			::operator delete(part, std::nothrow);
		}
	}
};

struct Engine::Eval {
	size_t rule;
	bool error = false;
	const char* error_text = "";
};

namespace {

Val eval_fail(bool& error, const char*& error_text, Type type, const char* text) {
	if (!error) {
		error = true;
		error_text = text;
	}
	return Val::none(type);
}

} // namespace

// ------------------------------------------------------------------------------------------
// Construction

Engine::Engine(EventLoop& loop, DatapointModel& model, const Clock& clock, Settings& settings, const TimeSource& time)
	: loop_(loop), model_(model), clock_(clock), settings_(settings), store_(settings), time_(time) {
	listening_ = model_.add_listener(*this) == DatapointStatus::ok;
	if (!listening_) {
		Log::error(kTag, "rule engine cannot listen to the datapoint model: no datapoint triggers");
	}
}

Engine::~Engine() {
	persist_now();
	if (loop_timer_.valid()) {
		loop_.cancel(loop_timer_);
	}
	if (listening_) {
		model_.remove_listener(*this);
	}
	delete runtime_;
	runtime_ = nullptr;
}

// ------------------------------------------------------------------------------------------
// Logging

void Engine::log_rule(size_t rule, LogCode code, LogLevel level, const char* detail, const char* fmt, ...) {
	char text[Limits::kMessage + 56];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	if (runtime_ && rule < runtime_->rule_count) {
		log_code(set_.str(set_.rule(rule).id), &runtime_->rules[rule], code, level, detail, text);
	} else {
		log_code("-", nullptr, code, level, detail, text);
	}
}

void Engine::log_code(const char* rule_id, RuleRuntime* rr, LogCode code, LogLevel level, const char* detail,
	const char* text) {
	Log::write(level, kTag, "%s %s: %s", rule_id, log_code_name(code), text);
	if (observer_) {
		observer_->rule_logged(rule_id, code, level, text);
	}
	if (rr && code != LogCode::log && level <= LogLevel::warn) {
		++rr->errors;
		rr->has_error = true;
		rr->last_error_ms = in_cascade_ ? cascade_ms_ : clock_.monotonic_ms();
		rr->last_error_code = code;
		rr->last_error_text = detail;
	}
}

// ------------------------------------------------------------------------------------------
// Loading and swapping

bool Engine::parameter_valid(const Parameter& d, const Val& v) const {
	if (!v.present || v.type != d.type) {
		return false;
	}
	if (d.min != kNone && order(v, set_.constant(d.min)) < 0) {
		return false;
	}
	if (d.max != kNone && order(v, set_.constant(d.max)) > 0) {
		return false;
	}
	if (d.choice_count > 0) {
		for (size_t i = 0; i < d.choice_count; ++i) {
			if (val_equal(v, set_.constant(set_.choice(d.first_choice + i).value))) {
				return true;
			}
		}
		return false;
	}
	return true;
}

LoadResult Engine::load(const JsonValue& rules, const ScheduleDirectory* schedules, const uint16_t* previous_ids) {
	flush();
	RuleSet next;
	char language[setting::kMaxUiLangLength + 1];
	read_ui_lang(settings_, language, kTag); // labels in the device's language (spec 2.2)
	LoadResult result = next.load(rules, model_, schedules, language);
	if (!result.ok()) {
		return result;
	}
	install(std::move(next), result, previous_ids);
	return result;
}

LoadResult Engine::adopt(RuleSet&& set, const uint16_t* previous_ids) {
	flush();
	LoadResult result;
	install(std::move(set), result, previous_ids);
	return result;
}

namespace {

/// Keeps the stored overrides of the parameters that have one in the new runtime.
template <class Owner>
class KeepOverrides : public ParameterStore::Visitor {
public:
	explicit KeepOverrides(const Owner& owner) : owner_(owner) {}
	bool entry(const char* logical, const Val& value) override { return owner_.keeps(logical, value); }

private:
	const Owner& owner_;
};

} // namespace

bool Engine::install(RuleSet&& next_set, LoadResult& result, const uint16_t* previous_ids) {
	RuleSet next = std::move(next_set);
	size_t rules = next.rule_count();
	size_t params = next.parameter_count();
	size_t timers = next.timer_count();
	size_t nodes = next.node_count();
	size_t tracked = next.tracked_count();
	size_t matches = next.match_count();
	size_t silences = next.silence_count();
	size_t actions = next.action_count();
	size_t interest_bits = model_.size();
	Runtime* rt = new (std::nothrow) Runtime();
	size_t bytes = sizeof(Runtime);
	size_t part = 0;
	size_t refused = 0;
	auto table = [&](size_t size) -> void* {
		if (size == 0 || rt == nullptr || refused > 0) {
			return nullptr;
		}
		void* p = ::operator new(size, std::nothrow);
		if (p == nullptr) {
			refused = size;
			return nullptr;
		}
		std::memset(p, 0, size);
		rt->parts[part++] = p;
		bytes += size;
		return p;
	};
	if (rt != nullptr) {
		if (!rt->rules.allocate(rules, bytes)) {
			refused = rules * sizeof(RuleRuntime);
		}
		if (refused == 0 && !rt->params.allocate(params, bytes)) {
			refused = params * sizeof(ParamRuntime);
		}
		rt->timers = static_cast<TimerRuntime*>(table(timers * sizeof(TimerRuntime)));
		rt->changes = static_cast<ChangeRecord*>(table(tracked * sizeof(ChangeRecord)));
		rt->matched = static_cast<uint16_t*>(table(matches * sizeof(uint16_t)));
		rt->silences = static_cast<SilenceRuntime*>(table(silences * sizeof(SilenceRuntime)));
		rt->action_flags = static_cast<uint8_t*>(table(actions));
		rt->node_flags = static_cast<uint8_t*>(table(nodes));
		rt->interest = static_cast<uint8_t*>(table((interest_bits + 7) / 8));
	}
	if (rt == nullptr || refused > 0) {
		delete rt; // no objects in its tables yet (the counts are 0): only the memory goes
		result.error = LoadError::out_of_memory;
		std::snprintf(result.text, sizeof(result.text), "no memory for the rule runtime (a table of %u bytes in all)",
			static_cast<unsigned>(rt == nullptr ? sizeof(Runtime) : refused));
		Log::error(kTag, "rules refused: %s", result.text);
		return false;
	}
	rt->bytes = bytes;
	rt->silence_count = silences;
	rt->interest_bits = interest_bits;
	rt->rule_count = rules;
	rt->param_count = params;
	rt->timer_count = timers;
	for (size_t i = 0; i < rules; ++i) {
		new (&rt->rules[i]) RuleRuntime();
	}
	for (size_t i = 0; i < params; ++i) {
		new (&rt->params[i]) ParamRuntime();
	}
	for (size_t i = 0; i < timers; ++i) {
		new (&rt->timers[i]) TimerRuntime();
	}
	for (size_t i = 0; i < tracked; ++i) {
		new (&rt->changes[i]) ChangeRecord();
	}
	for (size_t i = 0; i < silences; ++i) {
		new (&rt->silences[i]) SilenceRuntime();
	}

	Runtime* old = runtime_;
	const RuleSet& old_set = set_;
	bool boot = !booted_;

	// Rules: diagnostics, states, timers, parameters.
	for (size_t r = 0; r < rules; ++r) {
		const Rule& rule = next.rule(r);
		const char* rule_id = next.str(rule.id);
		RuleRuntime& nr = rt->rules[r];
		int o = old ? old_set.find_rule(rule_id) : -1;
		for (size_t t = 0; t < rule.timer_count; ++t) {
			rt->timers[rule.first_timer + t].rule = static_cast<uint8_t>(r);
		}
		if (o >= 0) {
			const Rule& old_rule = old_set.rule(static_cast<size_t>(o));
			const RuleRuntime& orr = old->rules[o];
			nr = orr;
			nr.state = kNoState;
			nr.has_state = false;
			nr.cascade_firings = 0;
			if (previous_ids != nullptr) {
				// The model was replaced: follow the datapoint of the last trigger and the last write
				// to its new id; forget it when the datapoint is gone.
				auto follow = [&](uint16_t old_id, bool& found) {
					for (size_t id = 0; id < model_.size(); ++id) {
						if (previous_ids[id] == old_id) {
							found = true;
							return static_cast<uint16_t>(id);
						}
					}
					found = false;
					return old_id;
				};
				bool found = false;
				if (orr.last_trigger_kind == static_cast<uint8_t>(TriggerKind::datapoint)) {
					nr.last_trigger_ref = follow(orr.last_trigger_ref, found);
					if (!found) {
						nr.last_trigger_kind = kNoKind;
					}
				}
				if (orr.last_action_kind == static_cast<uint8_t>(ActionKind::write) ||
					orr.last_action_kind == static_cast<uint8_t>(ActionKind::read)) {
					nr.last_action_target = follow(orr.last_action_target, found);
					if (!found) {
						nr.last_action_kind = kNoKind;
					}
				}
			}
			// Without previous_ids the model stayed: datapoint ids are valid. Other references are by
			// index of the old set.
			if (orr.last_trigger_kind != kNoKind && orr.last_trigger_kind != static_cast<uint8_t>(TriggerKind::datapoint) &&
				orr.last_trigger_kind != static_cast<uint8_t>(TriggerKind::start) &&
				orr.last_trigger_kind != static_cast<uint8_t>(TriggerKind::match)) {
				nr.last_trigger_kind = kNoKind; // timer/parameter/schedule indices of the old set
			}
			if (orr.last_action_kind != kNoKind && orr.last_action_kind != static_cast<uint8_t>(ActionKind::write) &&
				orr.last_action_kind != static_cast<uint8_t>(ActionKind::read) &&
				orr.last_action_kind != static_cast<uint8_t>(ActionKind::log)) {
				nr.last_action_kind = kNoKind;
			}
			if (orr.has_state) {
				const char* state_id = old_set.str(old_set.state(old_rule.first_state + orr.state).id);
				int s = next.find_state(r, state_id);
				if (s >= 0) {
					nr.state = static_cast<uint8_t>(s);
					nr.has_state = true;
				} else {
					char text[96];
					std::snprintf(text, sizeof(text), "state '%s' is not in the new image; the rule enters its initial state",
						state_id);
					log_code(rule_id, &nr, LogCode::state_reset, LogLevel::warn, "state not in the new image", text);
				}
			}
			for (size_t t = 0; t < rule.timer_count; ++t) {
				int ot = old_set.find_timer(static_cast<size_t>(o), next.str(next.timer(rule.first_timer + t).id));
				if (ot >= 0 && old->timers[old_rule.first_timer + ot].running) {
					TimerRuntime& nt = rt->timers[rule.first_timer + t];
					const TimerRuntime& otr = old->timers[old_rule.first_timer + ot];
					nt.deadline = otr.deadline;
					nt.sequence = otr.sequence;
					nt.running = true;
				}
			}
		}
		if (o < 0 && boot) {
			// A reboot: the rule enters its persisted state when the image still declares it; its
			// entry actions do not run again (spec 8.5).
			Val stored;
			if (store_.read(rule_id, ParameterStore::kStateKey, stored)) {
				const char* stored_id = stored.type == Type::string ? stored.str() : "?";
				int s = (stored.type == Type::string && rule.state_count > 0) ? next.find_state(r, stored_id) : -1;
				if (s >= 0) {
					nr.state = static_cast<uint8_t>(s);
					nr.has_state = true;
				} else {
					char text[112];
					if (rule.state_count > 0) {
						std::snprintf(text, sizeof(text),
							"persisted state '%s' is not in the image; the rule enters its initial state", stored_id);
					} else {
						std::snprintf(text, sizeof(text), "persisted state '%s' discarded: the rule has no states",
							stored_id);
					}
					log_code(rule_id, &nr, LogCode::state_reset, LogLevel::warn, "persisted state not in the image", text);
				}
			}
		}
		for (size_t p = 0; p < rule.parameter_count; ++p) {
			const Parameter& decl = next.parameter(rule.first_parameter + p);
			ParamRuntime& np = rt->params[rule.first_parameter + p];
			np.value = next.constant(decl.default_value);
			np.overridden = false;
			const char* param_id = next.str(decl.id);
			Val override_value;
			bool has_override = false;
			if (o >= 0) {
				const Rule& old_rule = old_set.rule(static_cast<size_t>(o));
				int op = old_set.find_parameter(static_cast<size_t>(o), param_id);
				if (op >= 0 && old->params[old_rule.first_parameter + op].overridden) {
					override_value = old->params[old_rule.first_parameter + op].value;
					has_override = true;
				}
			} else if (boot) {
				has_override = store_.read(rule_id, param_id, override_value);
			}
			if (!has_override) {
				continue;
			}
			// Validity against the new declaration (with the new set's constants).
			bool valid = override_value.type == decl.type;
			if (valid && decl.min != kNone && order(override_value, next.constant(decl.min)) < 0) {
				valid = false;
			}
			if (valid && decl.max != kNone && order(override_value, next.constant(decl.max)) > 0) {
				valid = false;
			}
			if (valid && decl.choice_count > 0) {
				bool found = false;
				for (size_t i = 0; i < decl.choice_count && !found; ++i) {
					found = val_equal(override_value, next.constant(next.choice(decl.first_choice + i).value));
				}
				valid = found;
			}
			if (valid) {
				np.value = override_value;
				np.overridden = true;
			} else {
				store_.erase(rule_id, param_id);
				char text[128];
				char value_text[48];
				format_val(override_value, value_text, sizeof(value_text));
				std::snprintf(text, sizeof(text), "override %s of '%s' no longer fits (%s); reset to the default", value_text,
					param_id, override_value.type != decl.type ? "type changed" : "range or choices");
				log_code(rule_id, &nr, LogCode::parameter_reset, LogLevel::warn, "override no longer fits", text);
			}
		}
	}

	// Change tracking continues for datapoints still tracked; new ones start from the model.
	for (size_t i = 0; i < tracked; ++i) {
		uint16_t id = next.tracked(i);
		uint16_t previous = previous_ids != nullptr ? previous_ids[id] : id;
		uint16_t old_index = (old && previous != kNone) ? old_set.find_tracked(previous) : kNone;
		if (old_index != kNone) {
			rt->changes[i] = old->changes[old_index];
		} else {
			const Datapoint* dp = model_.get(id);
			if (dp && dp->quality() != Quality::unknown) {
				rt->changes[i].time = dp->updated_ms();
				rt->changes[i].known = true;
			}
		}
	}

	// Interest: the datapoints a trigger or a match condition references.
	auto mark = [rt](uint16_t id) {
		if (id < rt->interest_bits) {
			rt->interest[id / 8] = static_cast<uint8_t>(rt->interest[id / 8] | (1u << (id % 8)));
		}
	};
	for (size_t i = 0; i < next.trigger_count(); ++i) {
		const Trigger& t = next.trigger(i);
		if (t.kind == TriggerKind::datapoint) {
			mark(t.ref);
		} else if (t.kind == TriggerKind::match) {
			const Node& root = next.node(t.ref);
			for (size_t n = t.ref; n < static_cast<size_t>(t.ref) + root.size; ++n) {
				if (next.node(n).kind == NodeKind::datapoint) {
					mark(next.node(n).a);
				}
			}
		}
	}

	// Swap.
	delete old;
	set_ = std::move(next);
	runtime_ = rt;
	booted_ = true;

	// Delete the stored overrides that no parameter holds any more (removed rules, undeclared
	// parameters, collisions, unreadable entries) and the stored states the image does not declare.
	struct Keeper {
		const Engine& engine;
		bool keeps(const char* logical, const Val& value) const {
			if (logical[0] == '@') {
				return true; // a block's override: the block runtime prunes its own (spec/blocks.md 8.2)
			}
			const char* slash = std::strchr(logical, '/');
			if (slash == nullptr || static_cast<size_t>(slash - logical) > 32) {
				return false;
			}
			char rule_id[33];
			std::memcpy(rule_id, logical, static_cast<size_t>(slash - logical));
			rule_id[slash - logical] = '\0';
			int r = engine.set_.find_rule(rule_id);
			if (r < 0) {
				return false;
			}
			if (std::strcmp(slash + 1, ParameterStore::kStateKey) == 0) {
				return engine.set_.rule(static_cast<size_t>(r)).state_count > 0 && value.type == Type::string &&
					engine.set_.find_state(static_cast<size_t>(r), value.str()) >= 0;
			}
			int p = engine.set_.find_parameter(static_cast<size_t>(r), slash + 1);
			return p >= 0 && engine.runtime_->params[engine.set_.rule(static_cast<size_t>(r)).first_parameter + p].overridden;
		}
	};
	Keeper keeper{*this};
	KeepOverrides<Keeper> visitor(keeper);
	store_.prune(visitor);
	bind_parameters();
	if (observer_) {
		for (size_t r = 0; r < set_.rule_count(); ++r) {
			const Rule& rule = set_.rule(r);
			for (size_t p = 0; p < rule.parameter_count; ++p) {
				observer_->parameter_changed(set_.str(rule.id), set_.str(set_.parameter(rule.first_parameter + p).id),
					runtime_->params[rule.first_parameter + p].value);
			}
		}
	}
	reschedule();
	return true;
}

// ------------------------------------------------------------------------------------------
// Queues

bool Engine::push_inbound(const Event& event) {
	if (inbound_count_ == kInboundCapacity) {
		if (!inbound_overflow_logged_) {
			inbound_overflow_logged_ = true;
			log_rule(SIZE_MAX, LogCode::queue_overflow, LogLevel::error, "external event queue full",
				"%u external events wait already; event dropped", static_cast<unsigned>(kInboundCapacity));
		}
		return false;
	}
	inbound_[(inbound_head_ + inbound_count_) % kInboundCapacity] = event;
	++inbound_count_;
	return true;
}

bool Engine::push_cascade(const Event& event, size_t rule) {
	if (queue_count_ == Limits::kQueue) {
		if (!overflow_logged_) {
			overflow_logged_ = true;
			log_rule(rule, LogCode::queue_overflow, LogLevel::error, "event queue full",
				"%u events wait in the cascade; event dropped", static_cast<unsigned>(Limits::kQueue));
		}
		return false;
	}
	queue_[(queue_head_ + queue_count_) % Limits::kQueue] = event;
	++queue_count_;
	return true;
}

void Engine::post_drain() {
	if (drain_posted_) {
		return;
	}
	if (loop_.post(Callback::member<Engine, &Engine::drain>(this))) {
		drain_posted_ = true;
	} else {
		Log::error(kTag, "cannot post the rule engine's event processing; it runs with the next event");
	}
}

void Engine::flush() {
	if (!in_cascade_) {
		drain();
	}
}

void Engine::drain() {
	drain_posted_ = false;
	if (in_cascade_) {
		return;
	}
	while (inbound_count_ > 0) {
		process_due_timers();
		Event event = inbound_[inbound_head_];
		inbound_head_ = (inbound_head_ + 1) % kInboundCapacity;
		--inbound_count_;
		if (runtime_ == nullptr) {
			continue;
		}
		if (event.kind == kStartAll) {
			// Re-baseline the match memories (7.4), then one cascade per rule in rule order (7.5).
			begin_cascade();
			for (size_t i = 0; i < set_.match_count(); ++i) {
				const Trigger& t = set_.trigger(set_.match(i).trigger);
				bool error = false;
				Val v = evaluate(t.ref, set_.match(i).rule, error);
				uint8_t& flags = runtime_->node_flags[t.ref];
				flags = static_cast<uint8_t>(v.has() && v.u.b ? (flags | kMatchMemory) : (flags & ~kMatchMemory));
			}
			end_cascade();
			StartReason reason = event.flags == TriggerFlags::kBoot ? StartReason::boot : StartReason::image;
			if (reason == StartReason::boot) {
				boot_start_ms_ = clock_.monotonic_ms();
			}
			start_silences(); // after the start cascades, overdue ones fire (spec 7.11)
			for (size_t r = 0; r < set_.rule_count(); ++r) {
				begin_cascade();
				run_start(r, reason);
				process_queue();
				end_cascade();
			}
			// Then the blocks, each in a cascade of its own (spec/blocks.md 5.2).
			for (size_t b = 0; host_ != nullptr && b < host_->start_count(); ++b) {
				begin_cascade();
				host_->start_block(b, reason);
				process_queue();
				end_cascade();
			}
		} else {
			run_cascade(event);
		}
	}
	inbound_overflow_logged_ = false;
	process_due_timers();
	reschedule();
}

void Engine::on_timer() {
	loop_timer_ = TimerHandle();
	process_due_timers();
	reschedule();
}

void Engine::process_due_timers() {
	if (runtime_ == nullptr || in_cascade_) {
		return;
	}
	for (;;) {
		uint64_t now = clock_.monotonic_ms();
		size_t best = SIZE_MAX;
		uint64_t best_deadline = 0;
		uint32_t best_sequence = 0;
		for (size_t i = 0; i < runtime_->timer_count; ++i) {
			const TimerRuntime& t = runtime_->timers[i];
			if (!t.running || t.deadline > now) {
				continue;
			}
			if (best == SIZE_MAX || t.deadline < best_deadline || (t.deadline == best_deadline && t.sequence < best_sequence)) {
				best = i;
				best_deadline = t.deadline;
				best_sequence = t.sequence;
			}
		}
		// Silence triggers take part in the same order (spec 7.11): a best index at or beyond the
		// timer count names a silence slot.
		for (size_t i = 0; i < runtime_->silence_count; ++i) {
			const SilenceRuntime& s = runtime_->silences[i];
			if (!s.armed || s.deadline > now) {
				continue;
			}
			if (best == SIZE_MAX || s.deadline < best_deadline || (s.deadline == best_deadline && s.sequence < best_sequence)) {
				best = runtime_->timer_count + i;
				best_deadline = s.deadline;
				best_sequence = s.sequence;
			}
		}
		// Then the blocks' timers and periods: at an equal deadline the rules' come first (spec/blocks.md B7).
		uint64_t block_deadline = host_ != nullptr ? host_->next_deadline() : EventLoop::kNever;
		if (block_deadline <= now && (best == SIZE_MAX || block_deadline < best_deadline)) {
			run_cascade(Event{kBlockDue, 0, 0, 0, 0});
			continue;
		}
		if (best == SIZE_MAX) {
			return;
		}
		if (best >= runtime_->timer_count) {
			size_t index = best - runtime_->timer_count;
			SilenceRuntime& s = runtime_->silences[index];
			s.armed = false;
			s.fired = true; // once per silent period
			run_cascade(Event{kSilence, 0, static_cast<uint16_t>(index), 0, 0});
			continue;
		}
		TimerRuntime& t = runtime_->timers[best];
		t.running = false; // stopped before its event is processed (7.8)
		size_t rule = t.rule;
		Event event{kTimer, 0, static_cast<uint16_t>(rule),
			static_cast<uint16_t>(best - set_.rule(rule).first_timer), 0};
		run_cascade(event);
	}
}

void Engine::reschedule() {
	uint64_t earliest = EventLoop::kNever;
	if (runtime_) {
		for (size_t i = 0; i < runtime_->timer_count; ++i) {
			if (runtime_->timers[i].running && runtime_->timers[i].deadline < earliest) {
				earliest = runtime_->timers[i].deadline;
			}
		}
		for (size_t i = 0; i < runtime_->silence_count; ++i) {
			if (runtime_->silences[i].armed && runtime_->silences[i].deadline < earliest) {
				earliest = runtime_->silences[i].deadline;
			}
		}
		if (host_ != nullptr) {
			uint64_t block_deadline = host_->next_deadline();
			earliest = block_deadline < earliest ? block_deadline : earliest;
		}
	}
	if (earliest == EventLoop::kNever) {
		if (loop_timer_.valid()) {
			loop_.cancel(loop_timer_);
			loop_timer_ = TimerHandle();
		}
		return;
	}
	if (loop_timer_.valid() && loop_deadline_ == earliest) {
		return;
	}
	if (loop_timer_.valid()) {
		loop_.cancel(loop_timer_);
	}
	loop_deadline_ = earliest;
	loop_timer_ = loop_.schedule_at(earliest, Callback::member<Engine, &Engine::on_timer>(this));
	if (!loop_timer_.valid()) {
		Log::error(kTag, "cannot schedule the rule timers; they run with the next event");
	}
}

// ------------------------------------------------------------------------------------------
// External inputs

void Engine::start(StartReason reason) {
	if (runtime_ == nullptr) {
		Log::error(kTag, "start (%s) refused: no rules loaded", start_reason_name(reason));
		return;
	}
	if (reason == StartReason::enable) {
		Log::error(kTag, "start (enable) is the effect of the enable switch, not an input");
		return;
	}
	if (push_inbound(Event{kStartAll, reason_flag(reason), 0, 0, 0})) {
		post_drain();
	}
}

void Engine::schedule_fired(const char* id) {
	if (runtime_ == nullptr) {
		return;
	}
	uint16_t index = set_.find_schedule(id);
	if (index == kNone) {
		Log::debug(kTag, "schedule '%s' fired; no rule uses it", id);
		return;
	}
	if (push_inbound(Event{kSchedule, 0, index, 0, 0})) {
		post_drain();
	}
}

void Engine::datapoint_updated(const DatapointUpdate& update) {
	if (runtime_ == nullptr) {
		return;
	}
	uint16_t id = update.datapoint.id();
	bool changed = update.changed();
	if (changed) {
		uint16_t index = set_.find_tracked(id);
		if (index != kNone) {
			runtime_->changes[index].time = clock_.monotonic_ms();
			runtime_->changes[index].known = true;
		}
	}
	if (quiet_) {
		return; // the parameter datapoints take their values at load: not an event (spec 8.6)
	}
	if (update.origin != UpdateOrigin::stale && runtime_->silence_count > 0) {
		// A sign of life re-arms the silence triggers of the datapoint (spec 7.11); a stale report is none.
		bool armed = false;
		uint64_t now = clock_.monotonic_ms();
		for (size_t i = 0; i < runtime_->silence_count; ++i) {
			if (set_.silence(i).datapoint == id) {
				SilenceRuntime& s = runtime_->silences[i];
				s.base = now;
				s.fired = false;
				arm_silence(i, now);
				armed = true;
			}
		}
		if (armed && !in_cascade_) {
			reschedule();
		}
	}
	if ((id >= runtime_->interest_bits || (runtime_->interest[id / 8] & (1u << (id % 8))) == 0) &&
		(host_ == nullptr || !host_->wants(id))) {
		return;
	}
	Event event{kDatapoint, static_cast<uint8_t>(changed ? kChanged : 0), id, static_cast<uint16_t>(update.origin), 0};
	if (in_cascade_) {
		push_cascade(event, current_rule_);
	} else if (push_inbound(event)) {
		post_drain();
	}
}

void Engine::refuse_parameter(const char* rule, const char* parameter, const char* reason) {
	int r = set_.find_rule(rule);
	char text[128];
	std::snprintf(text, sizeof(text), "parameter '%s' refused: %s", parameter, reason);
	log_code(rule, (r >= 0 && runtime_) ? &runtime_->rules[r] : nullptr, LogCode::parameter_refused, LogLevel::warn,
		reason, text);
}

ParameterStatus Engine::find_adjustable(const char* rule, const char* parameter, int& r, int& p, const char*& reason) {
	r = runtime_ ? set_.find_rule(rule) : -1;
	if (r < 0) {
		reason = "no such rule";
		return ParameterStatus::unknown_rule;
	}
	p = set_.find_parameter(static_cast<size_t>(r), parameter);
	if (p < 0) {
		reason = "no such parameter";
		return ParameterStatus::unknown_parameter;
	}
	if (!set_.parameter(set_.rule(static_cast<size_t>(r)).first_parameter + p).adjustable) {
		reason = "not adjustable";
		return ParameterStatus::not_adjustable;
	}
	return ParameterStatus::ok;
}

ParameterStatus Engine::set_parameter(const char* rule, const char* parameter, const JsonValue& value,
	const char** reason_out) {
	int r = -1, p = -1;
	const char* reason = "";
	ParameterStatus status = find_adjustable(rule, parameter, r, p, reason);
	Val v;
	if (status == ParameterStatus::ok) {
		const Parameter& d = set_.parameter(set_.rule(static_cast<size_t>(r)).first_parameter + p);
		if (val_from_json(d.type, value, v) != LiteralStatus::ok) {
			status = ParameterStatus::invalid_value;
			reason = "not a value of the parameter's type";
		}
	}
	if (status != ParameterStatus::ok) {
		refuse_parameter(rule, parameter, reason);
		if (reason_out) {
			*reason_out = reason;
		}
		return status;
	}
	return set_parameter(rule, parameter, v, reason_out);
}

ParameterStatus Engine::set_parameter(const char* rule, const char* parameter, const Val& value,
	const char** reason_out) {
	int r = -1, p = -1;
	const char* reason = "";
	ParameterStatus status = find_adjustable(rule, parameter, r, p, reason);
	Val v;
	if (status == ParameterStatus::ok) {
		const Parameter& d = set_.parameter(set_.rule(static_cast<size_t>(r)).first_parameter + p);
		if (!convert_for_parameter(value, d.type, v)) {
			status = ParameterStatus::invalid_value;
			reason = "not a value of the parameter's type";
		} else if (!parameter_valid(d, v)) {
			status = ParameterStatus::invalid_value;
			reason = d.choice_count > 0 ? "not one of the choices" : "outside min..max";
		}
	}
	if (status != ParameterStatus::ok) {
		refuse_parameter(rule, parameter, reason);
		if (reason_out) {
			*reason_out = reason;
		}
		return status;
	}
	apply_parameter(static_cast<size_t>(r), static_cast<size_t>(p), v, false);
	return ParameterStatus::ok;
}

ParameterStatus Engine::reset_parameter(const char* rule, const char* parameter, const char** reason_out) {
	int r = -1, p = -1;
	const char* reason = "";
	ParameterStatus status = find_adjustable(rule, parameter, r, p, reason);
	if (status != ParameterStatus::ok) {
		refuse_parameter(rule, parameter, reason);
		if (reason_out) {
			*reason_out = reason;
		}
		return status;
	}
	const Rule& rr = set_.rule(static_cast<size_t>(r));
	const Parameter& d = set_.parameter(rr.first_parameter + p);
	ParamRuntime& pr = runtime_->params[rr.first_parameter + p];
	Val old = pr.value;
	pr.value = set_.constant(d.default_value);
	pr.overridden = false;
	store_.erase(set_.str(rr.id), set_.str(d.id));
	if (!val_identical(old, pr.value)) {
		if (observer_) {
			observer_->parameter_changed(set_.str(rr.id), set_.str(d.id), pr.value);
		}
		publish_parameter(static_cast<size_t>(r), static_cast<size_t>(p));
		if (p == 0) {
			enable_changed(static_cast<size_t>(r), pr.value.u.b);
		} else {
			rearm_silences(static_cast<size_t>(r));
		}
		if (push_inbound(Event{kParameter, 0, static_cast<uint16_t>(r), static_cast<uint16_t>(p), 0})) {
			post_drain();
		}
	}
	return ParameterStatus::ok;
}

void Engine::apply_parameter(size_t rule, size_t index, const Val& value, bool derived, bool publish) {
	const Rule& r = set_.rule(rule);
	const Parameter& d = set_.parameter(r.first_parameter + index);
	ParamRuntime& pr = runtime_->params[r.first_parameter + index];
	Val old = pr.value;
	bool was_overridden = pr.overridden;
	pr.value = value;
	if (val_identical(value, set_.constant(d.default_value))) {
		// The default is a reset, not an override: the parameter follows later defaults (spec 8.2).
		pr.overridden = false;
		if (was_overridden) {
			store_.erase(set_.str(r.id), set_.str(d.id));
		}
	} else {
		pr.overridden = true;
		if (!was_overridden || !val_identical(old, value)) {
			store_.write(set_.str(r.id), set_.str(d.id), value);
		}
	}
	if (val_identical(old, value)) {
		return;
	}
	if (observer_) {
		observer_->parameter_changed(set_.str(r.id), set_.str(d.id), value);
	}
	if (index == 0) {
		enable_changed(rule, value.u.b); // disabling cancels the running timers (7.9)
	} else {
		rearm_silences(rule); // a duration may depend on the parameter
	}
	Event event{kParameter, 0, static_cast<uint16_t>(rule), static_cast<uint16_t>(index), 0};
	if (derived) {
		push_cascade(event, rule);
	} else if (push_inbound(event)) {
		post_drain();
	}
	// The datapoint's update follows the parameter's event (spec 8.6), as for a command, where the
	// model stores the value after the rule engine took it.
	if (publish) {
		publish_parameter(rule, index);
	}
}

void Engine::state_changed(size_t rule) {
	runtime_->rules[rule].state_dirty = true;
	if (persist_pending_) {
		return;
	}
	persist_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kStatePersistDelayMs,
		Callback::member<Engine, &Engine::on_persist>(this));
	persist_pending_ = persist_timer_.valid();
	if (!persist_pending_) {
		Log::error(kTag, "cannot delay the write of a rule state; written now");
		persist_now();
	}
}

void Engine::on_persist() {
	persist_timer_ = TimerHandle();
	persist_pending_ = false;
	persist_now();
}

void Engine::persist_now() {
	if (persist_pending_) {
		loop_.cancel(persist_timer_);
		persist_timer_ = TimerHandle();
		persist_pending_ = false;
	}
	if (runtime_ == nullptr) {
		return;
	}
	for (size_t r = 0; r < runtime_->rule_count; ++r) {
		RuleRuntime& rr = runtime_->rules[r];
		if (!rr.state_dirty) {
			continue;
		}
		rr.state_dirty = false;
		if (!rr.has_state) {
			continue;
		}
		const char* rule_id = set_.str(set_.rule(r).id);
		const char* state_id = set_.str(set_.state(set_.rule(r).first_state + rr.state).id);
		Val stored;
		if (store_.read(rule_id, ParameterStore::kStateKey, stored) && stored.type == Type::string &&
			std::strcmp(stored.str(), state_id) == 0) {
			continue; // the store holds it already
		}
		store_.write(rule_id, ParameterStore::kStateKey, Val::of_string(state_id, std::strlen(state_id)));
	}
}

void Engine::stop_timers(size_t rule) {
	const Rule& r = set_.rule(rule);
	for (size_t t = 0; t < r.timer_count; ++t) {
		runtime_->timers[r.first_timer + t].running = false;
	}
	for (size_t i = 0; i < runtime_->silence_count; ++i) {
		if (set_.silence(i).rule == rule) {
			runtime_->silences[i].armed = false;
		}
	}
	if (!in_cascade_) {
		reschedule();
	}
}

void Engine::enable_changed(size_t rule, bool enabled) {
	if (!enabled) {
		stop_timers(rule);
		return;
	}
	// Enabling arms the silence triggers anew: the period counts from the later of the last update
	// and the enabling (spec 7.11).
	uint64_t now = clock_.monotonic_ms();
	for (size_t i = 0; i < runtime_->silence_count; ++i) {
		if (set_.silence(i).rule == rule) {
			SilenceRuntime& s = runtime_->silences[i];
			s.base = s.base > now ? s.base : now;
			s.fired = false;
			arm_silence(i, s.base);
		}
	}
	if (!in_cascade_) {
		reschedule();
	}
}

// ------------------------------------------------------------------------------------------
// Silence triggers

void Engine::arm_silence(size_t index, uint64_t base) {
	const SilenceRef& ref = set_.silence(index);
	SilenceRuntime& s = runtime_->silences[index];
	s.armed = false;
	if (!enabled(ref.rule)) {
		return;
	}
	bool error = false;
	Val d = evaluate(ref.after, ref.rule, error);
	const Datapoint* dp = model_.get(ref.datapoint);
	const char* name = dp ? dp->name() : "?";
	if (!d.has() || d.u.i < Limits::kTimerMinMs || d.u.i > Limits::kTimerMaxMs) {
		if (!s.refused_logged) {
			s.refused_logged = true;
			char text[48];
			if (d.has()) {
				duration_text(d.u.i, text, sizeof(text));
			} else {
				std::snprintf(text, sizeof(text), "none");
			}
			log_rule(ref.rule, LogCode::timer_refused, LogLevel::error, "silence duration has no value or lies outside 100ms..30d",
				"silence trigger of '%s' not armed: the duration %s is not in 100ms..30d", name, text);
		}
		return;
	}
	s.refused_logged = false;
	s.armed = true;
	s.deadline = base + static_cast<uint64_t>(d.u.i);
	s.sequence = ++timer_sequence_;
}

void Engine::rearm_silences(size_t rule) {
	bool any = false;
	for (size_t i = 0; i < runtime_->silence_count; ++i) {
		SilenceRuntime& s = runtime_->silences[i];
		if (set_.silence(i).rule == rule && !s.fired) {
			arm_silence(i, s.base);
			any = true;
		}
	}
	if (any && !in_cascade_) {
		reschedule();
	}
}

void Engine::start_silences() {
	for (size_t i = 0; i < runtime_->silence_count; ++i) {
		const Datapoint* dp = model_.get(set_.silence(i).datapoint);
		uint64_t updated = dp != nullptr ? dp->updated_ms() : 0;
		SilenceRuntime& s = runtime_->silences[i];
		s.base = updated > boot_start_ms_ ? updated : boot_start_ms_;
		s.fired = false;
		arm_silence(i, s.base);
	}
}

// ------------------------------------------------------------------------------------------
// Parameters as datapoints

void Engine::bind_parameters() {
	if (runtime_ == nullptr) {
		return;
	}
	size_t mirrored = 0;
	size_t missing = 0;
	quiet_ = true;
	for (size_t r = 0; r < set_.rule_count(); ++r) {
		const Rule& rule = set_.rule(r);
		for (size_t p = 0; p < rule.parameter_count; ++p) {
			const Parameter& decl = set_.parameter(rule.first_parameter + p);
			ParamRuntime& pr = runtime_->params[rule.first_parameter + p];
			pr.datapoint = kNone;
			char name[Datapoint::kMaxName + 1];
			if (!parameter_datapoint_name(set_.str(rule.id), set_.str(decl.id), name, sizeof(name))) {
				continue;
			}
			const Datapoint* dp = model_.find(name);
			if (dp == nullptr) {
				++missing;
				continue;
			}
			if (dp->type() != parameter_datapoint_type(decl.type)) {
				Log::error(kTag, "datapoint '%s' is %s, the parameter needs %s: not mirrored", name,
					value_type_name(dp->type()), value_type_name(parameter_datapoint_type(decl.type)));
				continue;
			}
			if (model_.bind(dp->id(), technology_) != DatapointStatus::ok) {
				Log::error(kTag, "datapoint '%s' is owned by another technology: the parameter is not mirrored", name);
				continue;
			}
			pr.datapoint = dp->id();
			++mirrored;
			publish_parameter(r, p);
		}
	}
	quiet_ = false;
	if (missing > 0) {
		Log::debug(kTag, "%u parameter(s) without a datapoint in the model: not mirrored", static_cast<unsigned>(missing));
	}
	(void)mirrored;
}

bool Engine::parameter_of(uint16_t datapoint, size_t& rule, size_t& index) const {
	if (runtime_ == nullptr || datapoint == kNone) {
		return false;
	}
	for (size_t r = 0; r < set_.rule_count(); ++r) {
		const Rule& rr = set_.rule(r);
		for (size_t p = 0; p < rr.parameter_count; ++p) {
			if (runtime_->params[rr.first_parameter + p].datapoint == datapoint) {
				rule = r;
				index = p;
				return true;
			}
		}
	}
	return false;
}

void Engine::publish_parameter(size_t rule, size_t index) {
	const Rule& r = set_.rule(rule);
	const ParamRuntime& pr = runtime_->params[r.first_parameter + index];
	const Datapoint* dp = pr.datapoint != kNone ? model_.get(pr.datapoint) : nullptr;
	if (dp == nullptr) {
		return;
	}
	std::optional<Value> value;
	if (pr.value.type == Type::duration) {
		if (pr.value.u.i >= 0 && pr.value.u.i <= static_cast<int64_t>(UINT32_MAX)) {
			value = Value::of_unsigned(static_cast<uint32_t>(pr.value.u.i));
		}
	} else {
		value = convert_for_datapoint(pr.value, dp->type());
	}
	if (!value) {
		char text[64];
		format_val(pr.value, text, sizeof(text));
		Log::error(kTag, "%s: the value %s does not fit the %s datapoint; it keeps its previous value", dp->name(), text,
			value_type_name(dp->type()));
		return;
	}
	if (dp->quality() == Quality::good && dp->value() == *value) {
		return;
	}
	DatapointStatus status = model_.publish(technology_, pr.datapoint, *value);
	if (status != DatapointStatus::ok) {
		Log::error(kTag, "%s: the parameter's value was not published: %s", dp->name(), datapoint_status_name(status));
	}
}

bool Engine::parameter_command(const Datapoint& datapoint, const Value& value) {
	size_t r = 0, p = 0;
	if (!parameter_of(datapoint.id(), r, p)) {
		Log::error(kTag, "command to '%s' refused: no rule parameter behind it in the loaded rules", datapoint.name());
		return false;
	}
	const Rule& rule = set_.rule(r);
	const Parameter& d = set_.parameter(rule.first_parameter + p);
	const char* rule_id = set_.str(rule.id);
	const char* param_id = set_.str(d.id);
	const char* reason = nullptr;
	Val v;
	if (!d.adjustable) {
		reason = "not adjustable";
	} else if (d.type == Type::duration) {
		if (value.type() == ValueType::unsigned_int) {
			v = Val::of_duration(value.as_unsigned());
		} else {
			reason = "not a value of the parameter's type";
		}
	} else if (!convert_for_parameter(val_from_datapoint(value), d.type, v)) {
		reason = "not a value of the parameter's type";
	}
	if (reason == nullptr && !parameter_valid(d, v)) {
		reason = d.choice_count > 0 ? "not one of the choices" : "outside min..max";
	}
	if (reason != nullptr) {
		refuse_parameter(rule_id, param_id, reason);
		return false;
	}
	// The model stores the commanded value itself: no publish. Within a cascade (a rule's write) the
	// parameter change is a derived event.
	apply_parameter(r, p, v, in_cascade_, false);
	return true;
}

// ------------------------------------------------------------------------------------------
// Cascades

bool Engine::cascade_sun(SunPosition& out) const {
	if (!wall_valid_) {
		return false;
	}
	const int64_t utc = time_.local_to_utc(wall_);
	if (!sun_cached_ || utc != sun_utc_) {
		sun_known_ = time_.sun_position(utc, sun_);
		sun_utc_ = utc;
		sun_cached_ = true;
	}
	out = sun_;
	return sun_known_;
}

void Engine::begin_cascade() {
	cascade_ms_ = clock_.monotonic_ms();
	wall_valid_ = time_.local_now(wall_);
	derived_firings_ = 0;
	aborted_ = false;
	overflow_logged_ = false;
	queue_head_ = queue_count_ = 0;
	in_cascade_ = true;
	for (size_t i = 0; i < runtime_->rule_count; ++i) {
		runtime_->rules[i].cascade_firings = 0;
	}
	if (host_ != nullptr) {
		host_->cascade_begun();
	}
}

void Engine::end_cascade() {
	in_cascade_ = false;
	queue_head_ = queue_count_ = 0;
	reschedule();
}

void Engine::run_cascade(const Event& event) {
	begin_cascade();
	if (event.kind == kParameter && event.b == 0) {
		// The enable switch: enabling starts the rule with the reason `enable` in this cascade.
		size_t rule = event.a;
		if (rule < set_.rule_count() && enabled(rule)) {
			run_start(rule, StartReason::enable);
		}
	} else {
		process_event(event, false);
	}
	process_queue();
	end_cascade();
}

void Engine::run_start(size_t rule, StartReason reason) {
	const Rule& r = set_.rule(rule);
	RuleRuntime& rr = runtime_->rules[rule];
	if (r.state_count > 0 && !rr.has_state) {
		rr.state = r.initial_state;
		rr.has_state = true;
		state_changed(rule);
		if (enabled(rule)) {
			current_rule_ = rule;
			run_actions(rule, set_.state(r.first_state + r.initial_state).entry);
		}
	}
	if (enabled(rule)) {
		process_event(Event{kStart, reason_flag(reason), static_cast<uint16_t>(rule), 0, 0}, false);
	}
}

void Engine::process_queue() {
	while (queue_count_ > 0 && !aborted_) {
		Event event = queue_[queue_head_];
		queue_head_ = (queue_head_ + 1) % Limits::kQueue;
		--queue_count_;
		process_event(event, (event.flags & kExternal) == 0);
	}
}

bool Engine::trigger_matches(const Trigger& t, size_t rule, const Event& e) const {
	switch (t.kind) {
		case TriggerKind::datapoint: {
			if (e.kind != kDatapoint || e.a != t.ref) {
				return false;
			}
			if ((t.flags & TriggerFlags::kOnUpdate) == 0 && (e.flags & kChanged) == 0) {
				return false;
			}
			UpdateOrigin origin = static_cast<UpdateOrigin>(e.b);
			if ((t.flags & TriggerFlags::kOriginReport) && origin == UpdateOrigin::command) {
				return false;
			}
			if ((t.flags & TriggerFlags::kOriginCommand) && origin != UpdateOrigin::command) {
				return false;
			}
			return true;
		}
		case TriggerKind::match:
			return (e.kind == kDatapoint || e.kind == kParameter) && (runtime_->node_flags[t.ref] & kMatched) != 0;
		case TriggerKind::schedule: return e.kind == kSchedule && e.a == t.ref;
		case TriggerKind::timer: return e.kind == kTimer && e.a == rule && e.b == t.ref;
		case TriggerKind::start: return e.kind == kStart && e.a == rule && (t.flags & e.flags) != 0;
		case TriggerKind::parameter: return e.kind == kParameter && e.a == rule && e.b == t.ref;
		case TriggerKind::silence: return e.kind == kSilence && e.a == t.ref;
	}
	return false;
}

bool Engine::match_references(const Trigger& t, size_t rule, const Event& e) const {
	const Node& root = set_.node(t.ref);
	for (size_t n = t.ref; n < static_cast<size_t>(t.ref) + root.size; ++n) {
		const Node& node = set_.node(n);
		if (e.kind == kDatapoint && node.kind == NodeKind::datapoint && node.a == e.a) {
			return true;
		}
		if (e.kind == kParameter && node.kind == NodeKind::parameter && e.a == rule && node.a == e.b) {
			return true;
		}
	}
	return false;
}

void Engine::process_event(const Event& e, bool derived) {
	Runtime& rt = *runtime_;
	size_t matched = 0;

	// 1. Match memories, of every rule, enabled or not.
	if (e.kind == kDatapoint || e.kind == kParameter) {
		for (size_t i = 0; i < set_.match_count(); ++i) {
			const MatchRef& m = set_.match(i);
			const Trigger& t = set_.trigger(m.trigger);
			if (!match_references(t, m.rule, e)) {
				continue;
			}
			bool error = false;
			Val v = evaluate(t.ref, m.rule, error);
			bool now = v.has() && v.u.b;
			uint8_t& flags = rt.node_flags[t.ref];
			bool was = (flags & kMatchMemory) != 0;
			flags = static_cast<uint8_t>(now ? (flags | kMatchMemory) : (flags & ~kMatchMemory));
			if (now && !was) {
				flags |= kMatched;
				rt.matched[matched++] = t.ref;
			}
		}
	}

	// 2. Rules, in rule order.
	for (size_t r = 0; r < set_.rule_count() && !aborted_; ++r) {
		if (!enabled(r)) {
			continue;
		}
		const Rule& rule = set_.rule(r);
		RuleRuntime& rr = rt.rules[r];
		const Trigger* first = nullptr;
		for (size_t i = 0; i < rule.reaction_count && !first; ++i) {
			const Reaction& reaction = set_.reaction(rule.first_reaction + i);
			uint8_t state = rr.has_state ? rr.state : 0;
			if ((reaction.from_mask & (1u << state)) == 0) {
				continue;
			}
			for (size_t k = 0; k < reaction.trigger_count; ++k) {
				const Trigger& t = set_.trigger(reaction.first_trigger + k);
				if (trigger_matches(t, r, e)) {
					first = &t;
					break;
				}
			}
		}
		if (!first) {
			continue;
		}
		if (derived) {
			if (derived_firings_ + 1 > Limits::kDerivedFirings) {
				// Name the rule refused and the one that fired most in this cascade (the first in rule
				// order on a tie), from the cascade's per-rule counters (spec 9.2).
				const char* culprit = "-";
				uint32_t most = 0;
				guard_culprit(culprit, most);
				++rr.loop_guards;
				log_rule(r, LogCode::loop_guard, LogLevel::error, "cascade exceeded 32 derived firings",
					"cascade refused after %u derived firings (most: %s with %u); the queue is emptied",
					static_cast<unsigned>(Limits::kDerivedFirings), culprit, static_cast<unsigned>(most));
				if (observer_) {
					observer_->loop_guard(set_.str(rule.id), culprit, most);
				}
				aborted_ = true;
				queue_head_ = queue_count_ = 0;
				break;
			}
			++derived_firings_;
			++rr.cascade_firings;
			if (rr.cascade_firings > rr.peak_cascade_firings) {
				rr.peak_cascade_firings = rr.cascade_firings;
			}
		}
		++rr.firings;
		rr.last_trigger_ms = cascade_ms_;
		rr.last_trigger_wall_valid = wall_valid_;
		rr.last_trigger_wall = wall_;
		rr.last_trigger_kind = static_cast<uint8_t>(first->kind);
		rr.last_trigger_ref = first->ref;
		rr.last_trigger_flags = first->kind == TriggerKind::start ? e.flags : first->flags;
		current_rule_ = r;

		for (size_t i = 0; i < rule.reaction_count; ++i) {
			const Reaction& reaction = set_.reaction(rule.first_reaction + i);
			uint8_t state = rr.has_state ? rr.state : 0;
			if ((reaction.from_mask & (1u << state)) == 0) {
				continue;
			}
			bool active = false;
			for (size_t k = 0; k < reaction.trigger_count && !active; ++k) {
				active = trigger_matches(set_.trigger(reaction.first_trigger + k), r, e);
			}
			if (!active) {
				continue;
			}
			bool satisfied = true;
			if (reaction.condition != kNone) {
				bool error = false;
				Val c = evaluate(reaction.condition, r, error);
				uint8_t& flags = rt.node_flags[reaction.condition];
				if (!c.has()) {
					if (!error && (flags & kNoValueLogged) == 0) {
						flags |= kNoValueLogged;
						char text[160];
						print_expression(set_, model_, r, reaction.condition, text, sizeof(text));
						log_rule(r, LogCode::condition_no_value, LogLevel::warn, "condition has no value",
							"condition has no value: %s", text);
					}
					continue;
				}
				flags = static_cast<uint8_t>(flags & ~kNoValueLogged);
				satisfied = c.u.b;
			}
			if (satisfied) {
				if (reaction.to != kNoState) {
					if (rr.has_state) {
						run_actions(r, set_.state(rule.first_state + rr.state).exit);
					}
					run_actions(r, reaction.actions);
					rr.state = reaction.to;
					rr.has_state = true;
					state_changed(r);
					run_actions(r, set_.state(rule.first_state + reaction.to).entry);
					break;
				}
				run_actions(r, reaction.actions);
			} else {
				run_actions(r, reaction.else_actions);
			}
		}
	}

	for (size_t i = 0; i < matched; ++i) {
		rt.node_flags[rt.matched[i]] = static_cast<uint8_t>(rt.node_flags[rt.matched[i]] & ~kMatched);
	}

	// 3. Blocks, in block order (spec/blocks.md 5.1).
	if (host_ != nullptr && !aborted_) {
		if (e.kind == kDatapoint) {
			host_->datapoint_event(e.a, derived);
		} else if (e.kind == kBlock) {
			host_->block_event(e.flags, e.a, e.b, derived);
		} else if (e.kind == kBlockDue) {
			host_->run_due(cascade_ms_);
		}
	}
}

void Engine::guard_culprit(const char*& culprit, uint32_t& firings) const {
	// The rule with the most derived firings in this cascade (the first in rule order on a tie),
	// then the blocks' busiest when it called more (spec/rules.md 9.2, spec/blocks.md 5.1).
	culprit = "-";
	firings = 0;
	for (size_t i = 0; runtime_ != nullptr && i < runtime_->rule_count; ++i) {
		if (i == 0 || runtime_->rules[i].cascade_firings > firings) {
			culprit = set_.str(set_.rule(i).id);
			firings = runtime_->rules[i].cascade_firings;
		}
	}
	const char* block = nullptr;
	uint32_t calls = 0;
	if (host_ != nullptr && host_->busiest(block, calls) && (calls > firings || runtime_->rule_count == 0)) {
		culprit = block;
		firings = calls;
	}
}

bool Engine::derived_call(const char*& culprit, uint32_t& firings) {
	if (derived_firings_ + 1 > Limits::kDerivedFirings) {
		guard_culprit(culprit, firings);
		aborted_ = true;
		queue_head_ = queue_count_ = 0;
		return false;
	}
	++derived_firings_;
	return true;
}

void Engine::queue_block_event(uint8_t kind, uint16_t block, uint16_t arg) {
	Event event{kBlock, kind, block, arg, 0};
	if (in_cascade_) {
		push_cascade(event, current_rule_);
	} else if (push_inbound(event)) {
		post_drain();
	}
}

// ------------------------------------------------------------------------------------------
// Actions

void Engine::run_actions(size_t rule, const ActionList& list) {
	for (size_t i = 0; i < list.count; ++i) {
		current_rule_ = rule;
		run_action(rule, set_.action(list.first + i));
	}
}

void Engine::run_action(size_t rule, const Action& action) {
	switch (action.kind) {
		case ActionKind::write: action_write(rule, action); break;
		case ActionKind::start_timer:
		case ActionKind::restart_timer:
		case ActionKind::cancel_timer: action_timer(rule, action); break;
		case ActionKind::set_parameter: action_set_parameter(rule, action); break;
		case ActionKind::log: action_log(rule, action); break;
		case ActionKind::read: action_read(rule, action); break;
	}
}

void Engine::action_read(size_t rule, const Action& action) {
	uint8_t& flags = runtime_->action_flags[static_cast<size_t>(&action - &set_.action(0))];
	const Datapoint* dp = model_.get(action.target);
	const char* reason = nullptr;
	if (dp == nullptr) {
		reason = "the datapoint is not in the model";
	} else if (dp->technology() == nullptr) {
		reason = "no technology owns it";
	} else {
		DatapointStatus status = model_.read(action.target);
		if (status == DatapointStatus::refused) {
			reason = "the technology refused (its log says why)";
		} else if (status != DatapointStatus::ok) {
			reason = datapoint_status_name(status);
		}
	}
	if (reason != nullptr) {
		if ((flags & kReadRefusedLogged) == 0) {
			flags |= kReadRefusedLogged;
			log_rule(rule, LogCode::read_refused, LogLevel::warn, reason, "read of '%s' refused: %s", dp ? dp->name() : "?",
				reason);
		}
		return;
	}
	flags = static_cast<uint8_t>(flags & ~kReadRefusedLogged);
	if (observer_) {
		observer_->rule_read(set_.str(set_.rule(rule).id), *dp);
	}
	record_action(rule, ActionKind::read, action.target, Val::none());
}

void Engine::record_action(size_t rule, ActionKind kind, uint16_t target, const Val& value) {
	RuleRuntime& rr = runtime_->rules[rule];
	rr.last_action_ms = cascade_ms_;
	rr.last_action_kind = static_cast<uint8_t>(kind);
	rr.last_action_target = target;
	rr.last_action_value = value;
}

void Engine::action_write(size_t rule, const Action& action) {
	bool error = false;
	Val v = evaluate(action.expr, rule, error);
	uint8_t& flags = runtime_->node_flags[action.expr];
	const Datapoint* dp = model_.get(action.target);
	const char* name = dp ? dp->name() : "?";
	if (!v.has()) {
		if (!error && (flags & kNoValueLogged) == 0) {
			flags |= kNoValueLogged;
			log_rule(rule, LogCode::value_no_value, LogLevel::warn, "write value has no value",
				"value for '%s' has no value; nothing written", name);
		}
		return;
	}
	flags = static_cast<uint8_t>(flags & ~kNoValueLogged);
	if (dp == nullptr) {
		log_rule(rule, LogCode::write_refused, LogLevel::error, "datapoint not in the model",
			"datapoint %u is not in the model", static_cast<unsigned>(action.target));
		return;
	}
	std::optional<Value> converted = convert_for_datapoint(v, dp->type());
	if (!converted) {
		char text[64];
		format_val(v, text, sizeof(text));
		log_rule(rule, LogCode::write_refused, LogLevel::error, "value does not convert exactly",
			"%s does not convert exactly to the %s datapoint '%s'", text, datapoint_type_name(dp->type()), name);
		return;
	}
	if ((action.flags & 1) != 0 && dp->quality() == Quality::good && dp->value() == *converted) {
		return;
	}
	DatapointStatus status = model_.command_by_rule(action.target, *converted);
	if (status != DatapointStatus::ok) {
		log_rule(rule, LogCode::write_refused, LogLevel::error, "command refused", "command to '%s' refused: %s", name,
			datapoint_status_name(status));
		return;
	}
	if (observer_) {
		observer_->rule_commanded(set_.str(set_.rule(rule).id), *dp, *converted);
	}
	record_action(rule, ActionKind::write, action.target, v);
}

void Engine::action_timer(size_t rule, const Action& action) {
	const Rule& r = set_.rule(rule);
	TimerRuntime& t = runtime_->timers[r.first_timer + action.target];
	const char* id = set_.str(set_.timer(r.first_timer + action.target).id);
	if (action.kind == ActionKind::cancel_timer) {
		if (t.running) {
			t.running = false;
			record_action(rule, action.kind, action.target, Val::none());
		}
		return;
	}
	if (action.kind == ActionKind::start_timer && t.running) {
		return;
	}
	uint16_t root = action.expr != kNone ? action.expr : set_.timer(r.first_timer + action.target).duration;
	bool error = false;
	Val d = evaluate(root, rule, error);
	if (!d.has()) {
		log_rule(rule, LogCode::timer_refused, LogLevel::error, "timer duration has no value",
			"timer '%s' not started: the duration has no value", id);
		return;
	}
	if (d.u.i < Limits::kTimerMinMs || d.u.i > Limits::kTimerMaxMs) {
		char text[48];
		duration_text(d.u.i, text, sizeof(text));
		log_rule(rule, LogCode::timer_refused, LogLevel::error, "timer duration outside 100ms..30d",
			"timer '%s' not started: %s is outside 100ms..30d", id, text);
		return;
	}
	t.running = true;
	t.deadline = cascade_ms_ + static_cast<uint64_t>(d.u.i);
	t.sequence = ++timer_sequence_;
	record_action(rule, action.kind, action.target, d);
}

void Engine::action_set_parameter(size_t rule, const Action& action) {
	const Rule& r = set_.rule(rule);
	const Parameter& d = set_.parameter(r.first_parameter + action.target);
	const char* id = set_.str(d.id);
	bool error = false;
	Val v = evaluate(action.expr, rule, error);
	if (!v.has()) {
		refuse_parameter(set_.str(r.id), id, "the value has no value");
		return;
	}
	Val converted;
	if (!convert_for_parameter(v, d.type, converted) || !parameter_valid(d, converted)) {
		char text[64];
		format_val(v, text, sizeof(text));
		char reason[112];
		std::snprintf(reason, sizeof(reason), "%s is not valid", text);
		char message[160];
		std::snprintf(message, sizeof(message), "parameter '%s' refused: %s", id, reason);
		log_code(set_.str(r.id), &runtime_->rules[rule], LogCode::parameter_refused, LogLevel::warn, "value not valid",
			message);
		return;
	}
	apply_parameter(rule, action.target, converted, true);
	record_action(rule, ActionKind::set_parameter, action.target, converted);
}

void Engine::action_log(size_t rule, const Action& action) {
	char message[Limits::kMessage + 4];
	size_t total = 0; // full length, also beyond the buffer
	message[0] = '\0';
	auto append = [&](const char* text, size_t length) {
		for (size_t i = 0; i < length; ++i) {
			if (total < Limits::kMessage + 1) {
				message[total] = text[i];
			}
			++total;
		}
	};
	for (size_t i = 0; i < action.expr; ++i) {
		const LogPart& part = set_.log_part(action.target + i);
		if (part.expr == kNone) {
			append(set_.str(part.text), part.length);
		} else {
			bool error = false;
			Val v = evaluate(part.expr, rule, error);
			char text[Limits::kMessage + 2];
			size_t length = format_val(v, text, sizeof(text));
			append(text, length < sizeof(text) ? length : sizeof(text) - 1);
			if (length >= sizeof(text)) {
				total += length - (sizeof(text) - 1);
			}
		}
	}
	size_t end = total;
	if (total > Limits::kMessage) {
		end = Limits::kMessage - 3;
		while (end > 0 && (static_cast<uint8_t>(message[end]) & 0xC0) == 0x80) {
			--end; // do not cut a UTF-8 sequence
		}
		std::memcpy(message + end, "...", 3);
		end += 3;
	}
	message[end] = '\0';
	LogLevel level = action.flags == static_cast<uint8_t>(LogActionLevel::error)
		? LogLevel::error
		: (action.flags == static_cast<uint8_t>(LogActionLevel::warning) ? LogLevel::warn : LogLevel::info);
	log_code(set_.str(set_.rule(rule).id), &runtime_->rules[rule], LogCode::log, level, "", message);
	record_action(rule, ActionKind::log, 0, Val::none());
}

// ------------------------------------------------------------------------------------------
// Evaluation

Val Engine::evaluate(uint16_t root, size_t rule, bool& error) {
	Eval cx{rule};
	Val v = eval(root, cx);
	uint8_t& flags = runtime_->node_flags[root];
	if (cx.error) {
		error = true;
		if ((flags & kEvalErrorLogged) == 0) {
			flags |= kEvalErrorLogged;
			char text[160];
			print_expression(set_, model_, rule, root, text, sizeof(text));
			log_rule(rule, LogCode::eval_error, LogLevel::error, cx.error_text, "%s in %s", cx.error_text, text);
		}
	} else {
		flags = static_cast<uint8_t>(flags & ~kEvalErrorLogged);
	}
	return v;
}

Val Engine::literal(const Node& node) const {
	switch (node.type) {
		case Type::boolean: return Val::of_bool(node.lit.b);
		case Type::integer: return Val::of_int(node.lit.i);
		case Type::duration: return Val::of_duration(node.lit.i);
		case Type::real: return Val::of_real(node.lit.r);
		case Type::enumeration: return Val::of_enum(node.lit.e);
		case Type::time_of_day: return Val::of_time(node.lit.tod);
		case Type::date: return Val::of_date(node.lit.date);
		case Type::controlled: return Val::of_controlled(node.lit.ctl.control, node.lit.ctl.value);
		case Type::color: return Val::of_color(node.lit.color);
		default: return set_.constant(node.a);
	}
}

Val Engine::datapoint_value(uint16_t id) const {
	const Datapoint* dp = model_.get(id);
	if (dp == nullptr || dp->quality() != Quality::good) {
		return Val::none();
	}
	return val_from_datapoint(dp->value());
}

int64_t Engine::cascade_age(uint64_t time) const {
	return cascade_ms_ >= time ? static_cast<int64_t>(cascade_ms_ - time) : 0;
}

Val Engine::eval(uint16_t index, Eval& cx) {
	const Node& n = set_.node(index);
	Val v;
	switch (n.kind) {
		case NodeKind::literal: v = literal(n); break;
		case NodeKind::datapoint: v = datapoint_value(n.a); break;
		case NodeKind::parameter: v = runtime_->params[set_.rule(cx.rule).first_parameter + n.a].value; break;
		case NodeKind::timer: v = Val::none(); break;
		case NodeKind::unary: {
			Val x = eval(n.a, cx);
			if (!x.has()) {
				break;
			}
			if (static_cast<Op>(n.op) == Op::not_) {
				v = Val::of_bool(!x.u.b);
			} else if (x.type == Type::real) {
				v = Val::of_real(-x.u.r);
			} else if (x.u.i == INT64_MIN) {
				v = eval_fail(cx.error, cx.error_text, n.type, "integer overflow");
			} else {
				v = x;
				v.u.i = -x.u.i;
			}
			break;
		}
		case NodeKind::binary: v = eval_binary(n, cx); break;
		case NodeKind::call: v = eval_call(n, cx); break;
	}
	if (!v.present) {
		v.type = n.type;
	}
	return v;
}

Val Engine::eval_binary(const Node& n, Eval& cx) {
	Op op = static_cast<Op>(n.op);
	auto fail = [&](const char* text) { return eval_fail(cx.error, cx.error_text, n.type, text); };
	if (op == Op::and_ || op == Op::or_) {
		bool dominant = op == Op::or_; // true dominates `or`, false dominates `and`
		Val a = eval(n.a, cx);
		if (a.has() && a.u.b == dominant) {
			return Val::of_bool(dominant);
		}
		Val b = eval(static_cast<uint16_t>(n.a + 1), cx);
		if (b.has() && b.u.b == dominant) {
			return Val::of_bool(dominant);
		}
		if (!a.has() || !b.has()) {
			return Val::none(Type::boolean);
		}
		return Val::of_bool(!dominant);
	}
	Val a = eval(n.a, cx);
	Val b = eval(static_cast<uint16_t>(n.a + 1), cx);
	if (!a.has() || !b.has()) {
		return Val::none(n.type);
	}
	switch (op) {
		case Op::eq: return Val::of_bool(equal(a, b, &time_));
		case Op::ne: return Val::of_bool(!equal(a, b, &time_));
		case Op::lt: return Val::of_bool(order(a, b, &time_) < 0);
		case Op::le: return Val::of_bool(order(a, b, &time_) <= 0);
		case Op::gt: return Val::of_bool(order(a, b, &time_) > 0);
		case Op::ge: return Val::of_bool(order(a, b, &time_) >= 0);
		default: break;
	}
	auto real_result = [&](double r) { return std::isfinite(r) ? Val::of_real(r) : fail("a result that is not a finite real"); };
	auto duration_result = [&](double ms) {
		double rounded = std::round(ms);
		if (!std::isfinite(rounded) || std::fabs(rounded) >= 9.2e18) {
			return fail("duration overflow");
		}
		return Val::of_duration(static_cast<int64_t>(rounded));
	};
	bool numbers = (a.type == Type::integer || a.type == Type::real) && (b.type == Type::integer || b.type == Type::real);
	bool integers = a.type == Type::integer && b.type == Type::integer;
	int64_t r = 0;
	switch (op) {
		case Op::add:
		case Op::sub: {
			bool add = op == Op::add;
			if (integers || (a.type == Type::duration && b.type == Type::duration)) {
				bool overflow = add ? __builtin_add_overflow(a.u.i, b.u.i, &r) : __builtin_sub_overflow(a.u.i, b.u.i, &r);
				if (overflow) {
					return fail(integers ? "integer overflow" : "duration overflow");
				}
				return integers ? Val::of_int(r) : Val::of_duration(r);
			}
			if (numbers) {
				return real_result(add ? as_double(a) + as_double(b) : as_double(a) - as_double(b));
			}
			if (a.type == Type::time_of_day && b.type == Type::duration) {
				int64_t seconds = b.u.i / 1000; // truncated toward zero
				int64_t s = (seconds_of_day(a.u.tod) + (add ? seconds : -seconds)) % 86400;
				if (s < 0) {
					s += 86400;
				}
				TimeOfDay t = a.u.tod;
				t.hour = static_cast<uint8_t>(s / 3600);
				t.minute = static_cast<uint8_t>(s % 3600 / 60);
				t.second = static_cast<uint8_t>(s % 60);
				return Val::of_time(t);
			}
			if (a.type == Type::date_time && b.type == Type::duration) {
				// On the instant: a duration is elapsed time, also across a DST change (spec 4.6).
				int64_t seconds = b.u.i / 1000; // truncated toward zero
				int64_t utc = time_.local_to_utc(a.u.dt) + (add ? seconds : -seconds);
				DateTime d;
				if (utc < days_of(1899, 12, 31) * 86400 || utc > days_of(2156, 1, 2) * 86400 ||
					!time_.utc_to_local(utc, d) || d.year < 1900 || d.year > 2155) {
					return fail("date-time out of range");
				}
				d.flags = static_cast<uint16_t>(d.flags & DateTime::kSummerTime);
				return Val::of_date_time(d);
			}
			if (a.type == Type::time_of_day) { // timeOfDay - timeOfDay
				return Val::of_duration(static_cast<int64_t>(seconds_of_day(a.u.tod) - seconds_of_day(b.u.tod)) * 1000);
			}
			if (a.type == Type::date_time) {
				return Val::of_duration((time_.local_to_utc(a.u.dt) - time_.local_to_utc(b.u.dt)) * 1000);
			}
			if (a.type == Type::date) {
				return Val::of_duration((days_of(a.u.date.year, a.u.date.month, a.u.date.day) -
					days_of(b.u.date.year, b.u.date.month, b.u.date.day)) * 86400000LL);
			}
			if (a.type == Type::string) {
				if (static_cast<size_t>(a.length) + b.length > kMaxString) {
					return fail("a string longer than 28 bytes");
				}
				char text[2 * kMaxString];
				std::memcpy(text, a.u.bytes, a.length);
				std::memcpy(text + a.length, b.u.bytes, b.length);
				return Val::of_string(text, static_cast<size_t>(a.length) + b.length);
			}
			break;
		}
		case Op::mul:
			if (integers) {
				if (__builtin_mul_overflow(a.u.i, b.u.i, &r)) {
					return fail("integer overflow");
				}
				return Val::of_int(r);
			}
			if (numbers) {
				return real_result(as_double(a) * as_double(b));
			}
			{
				const Val& d = a.type == Type::duration ? a : b;
				const Val& f = a.type == Type::duration ? b : a;
				if (f.type == Type::integer) {
					if (__builtin_mul_overflow(d.u.i, f.u.i, &r)) {
						return fail("duration overflow");
					}
					return Val::of_duration(r);
				}
				return duration_result(static_cast<double>(d.u.i) * f.u.r);
			}
		case Op::div:
			if (numbers) {
				if (as_double(b) == 0) {
					return fail("division by zero");
				}
				return real_result(as_double(a) / as_double(b));
			}
			if (a.type == Type::duration && b.type == Type::duration) {
				if (b.u.i == 0) {
					return fail("division by zero");
				}
				return real_result(static_cast<double>(a.u.i) / static_cast<double>(b.u.i));
			}
			if (as_double(b) == 0) {
				return fail("division by zero");
			}
			return duration_result(static_cast<double>(a.u.i) / as_double(b));
		case Op::mod:
			if (b.u.i == 0) {
				return fail("remainder by zero");
			}
			r = b.u.i == -1 ? 0 : a.u.i % b.u.i;
			return integers ? Val::of_int(r) : Val::of_duration(r);
		default: break;
	}
	return fail("unsupported operands");
}

namespace {

/// toString(): kept out of the recursive evaluator so that its buffer is not on every frame.
[[gnu::noinline]] Val to_string_value(const Val& v, bool& error, const char*& error_text) {
	char text[Limits::kMessage + 2];
	size_t length = format_val(v, text, sizeof(text));
	if (length > kMaxString) {
		return eval_fail(error, error_text, Type::string, "a string longer than 28 bytes");
	}
	return Val::of_string(text, length);
}

} // namespace

Val Engine::eval_call(const Node& n, Eval& cx) {
	Fn fn = static_cast<Fn>(n.op);
	auto arg = [&](size_t k) { return eval(static_cast<uint16_t>(n.a + k), cx); };
	auto fail = [&](const char* text) { return eval_fail(cx.error, cx.error_text, n.type, text); };
	auto integer_of = [&](double r) {
		if (!(std::fabs(r) < 9.2233720368547758e18)) {
			return fail("a result beyond the integer range");
		}
		return Val::of_int(static_cast<int64_t>(r));
	};
	switch (fn) {
		case Fn::if_: {
			Val c = arg(0);
			if (!c.has()) {
				return Val::none(n.type);
			}
			return promote(arg(c.u.b ? 1 : 2), n.type);
		}
		case Fn::known: return Val::of_bool(arg(0).has());
		case Fn::coalesce: {
			Val x = arg(0);
			return promote(x.has() ? x : arg(1), n.type);
		}
		case Fn::between: {
			Val x = arg(0), low = arg(1), high = arg(2);
			if (!x.has() || !low.has() || !high.has()) {
				return Val::none(n.type);
			}
			if (order(low, high, &time_) > 0) {
				return Val::of_bool(false);
			}
			return Val::of_bool(order(low, x, &time_) <= 0 && order(x, high, &time_) <= 0);
		}
		case Fn::hysteresis: {
			Val x = arg(0), low = arg(1), high = arg(2);
			if (!x.has() || !low.has() || !high.has()) {
				return Val::none(n.type);
			}
			if (order(low, high, &time_) >= 0) {
				return fail("hysteresis with low >= high");
			}
			if (order(x, high, &time_) >= 0) {
				return Val::of_bool(true);
			}
			if (order(x, low, &time_) <= 0) {
				return Val::of_bool(false);
			}
			return arg(3);
		}
		case Fn::abs: {
			Val x = arg(0);
			if (!x.has()) {
				return x;
			}
			if (x.type == Type::real) {
				return Val::of_real(std::fabs(x.u.r));
			}
			if (x.u.i == INT64_MIN) {
				return fail("integer overflow");
			}
			x.u.i = x.u.i < 0 ? -x.u.i : x.u.i;
			return x;
		}
		case Fn::min:
		case Fn::max: {
			Val best;
			bool missing = false;
			for (size_t k = 0; k < n.argc; ++k) {
				Val x = arg(k);
				if (!x.has()) {
					missing = true;
					continue;
				}
				if (!best.has() || (fn == Fn::min ? order(x, best, &time_) < 0 : order(x, best, &time_) > 0)) {
					best = x;
				}
			}
			return missing ? Val::none(n.type) : promote(best, n.type);
		}
		case Fn::clamp: {
			Val x = arg(0), low = arg(1), high = arg(2);
			if (!x.has() || !low.has() || !high.has()) {
				return Val::none(n.type);
			}
			if (order(low, high, &time_) > 0) {
				return fail("clamp with low > high");
			}
			Val result = order(x, high, &time_) > 0 ? high : x;
			if (order(result, low, &time_) < 0) {
				result = low;
			}
			return promote(result, n.type);
		}
		case Fn::round:
		case Fn::floor:
		case Fn::ceil:
		case Fn::trunc: {
			Val x = arg(0);
			if (!x.has() || x.type == Type::integer) {
				return x;
			}
			double r = fn == Fn::round ? std::round(x.u.r)
									   : (fn == Fn::floor ? std::floor(x.u.r) : (fn == Fn::ceil ? std::ceil(x.u.r) : std::trunc(x.u.r)));
			return integer_of(r);
		}
		case Fn::round_to: {
			Val x = arg(0), step = arg(1);
			if (!x.has() || !step.has()) {
				return Val::none(n.type);
			}
			double s = as_double(step);
			if (!(s > 0)) {
				return fail("roundTo with a step not above 0");
			}
			double r = std::round(as_double(x) / s) * s;
			return std::isfinite(r) ? Val::of_real(r) : fail("a result that is not a finite real");
		}
		case Fn::to_int: {
			Val x = arg(0);
			if (!x.has()) {
				return Val::none(n.type);
			}
			return Val::of_int(x.type == Type::boolean ? (x.u.b ? 1 : 0) : static_cast<int64_t>(x.u.e));
		}
		case Fn::now: return wall_valid_ ? Val::of_date_time(wall_) : Val::none(n.type);
		case Fn::today: {
			if (!wall_valid_) {
				return Val::none(n.type);
			}
			Date d;
			d.year = wall_.year;
			d.month = wall_.month;
			d.day = wall_.day;
			return Val::of_date(d);
		}
		case Fn::time: {
			if (!wall_valid_) {
				return Val::none(n.type);
			}
			TimeOfDay t;
			t.day_of_week = wall_.day_of_week;
			t.hour = wall_.hour;
			t.minute = wall_.minute;
			t.second = wall_.second;
			return Val::of_time(t);
		}
		case Fn::weekday: {
			if (n.argc == 0) {
				return wall_valid_ ? Val::of_int(weekday_of(wall_.year, wall_.month, wall_.day)) : Val::none(n.type);
			}
			Val x = arg(0);
			if (!x.has()) {
				return Val::none(n.type);
			}
			switch (x.type) {
				case Type::date: return Val::of_int(weekday_of(x.u.date.year, x.u.date.month, x.u.date.day));
				case Type::date_time: return Val::of_int(weekday_of(x.u.dt.year, x.u.dt.month, x.u.dt.day));
				default: return x.u.tod.day_of_week != 0 ? Val::of_int(x.u.tod.day_of_week) : Val::none(n.type);
			}
		}
		case Fn::hour:
		case Fn::minute:
		case Fn::second: {
			Val x = arg(0);
			if (!x.has()) {
				return Val::none(n.type);
			}
			bool t = x.type == Type::time_of_day;
			uint8_t h = t ? x.u.tod.hour : x.u.dt.hour;
			uint8_t m = t ? x.u.tod.minute : x.u.dt.minute;
			uint8_t s = t ? x.u.tod.second : x.u.dt.second;
			return Val::of_int(fn == Fn::hour ? h : (fn == Fn::minute ? m : s));
		}
		case Fn::year:
		case Fn::month:
		case Fn::day: {
			Val x = arg(0);
			if (!x.has()) {
				return Val::none(n.type);
			}
			bool d = x.type == Type::date;
			int64_t y = d ? x.u.date.year : x.u.dt.year;
			int64_t mo = d ? x.u.date.month : x.u.dt.month;
			int64_t da = d ? x.u.date.day : x.u.dt.day;
			return Val::of_int(fn == Fn::year ? y : (fn == Fn::month ? mo : da));
		}
		case Fn::date_of:
		case Fn::time_of: {
			Val x = arg(0);
			if (!x.has()) {
				return Val::none(n.type);
			}
			if (fn == Fn::date_of) {
				Date d;
				d.year = x.u.dt.year;
				d.month = x.u.dt.month;
				d.day = x.u.dt.day;
				return Val::of_date(d);
			}
			TimeOfDay t;
			t.day_of_week = weekday_of(x.u.dt.year, x.u.dt.month, x.u.dt.day);
			t.hour = x.u.dt.hour;
			t.minute = x.u.dt.minute;
			t.second = x.u.dt.second;
			return Val::of_time(t);
		}
		case Fn::in_window: {
			Val t = arg(0), from = arg(1), until = arg(2);
			if (!t.has() || !from.has() || !until.has()) {
				return Val::none(n.type);
			}
			int s = t.type == Type::time_of_day ? seconds_of_day(t.u.tod)
											   : t.u.dt.hour * 3600 + t.u.dt.minute * 60 + t.u.dt.second;
			int f = seconds_of_day(from.u.tod);
			int u = seconds_of_day(until.u.tod);
			if (f < u) {
				return Val::of_bool(f <= s && s < u);
			}
			if (f > u) {
				return Val::of_bool(s >= f || s < u);
			}
			return Val::of_bool(false);
		}
		case Fn::sunrise:
		case Fn::sunset:
		case Fn::dawn:
		case Fn::dusk: {
			if (!wall_valid_) {
				return Val::none(n.type);
			}
			SunEvent event = fn == Fn::sunrise ? SunEvent::sunrise
											   : (fn == Fn::sunset ? SunEvent::sunset : (fn == Fn::dawn ? SunEvent::dawn : SunEvent::dusk));
			TimeOfDay t;
			if (!time_.sun_time(event, t)) {
				return Val::none(n.type);
			}
			return Val::of_time(t);
		}
		case Fn::sun_azimuth:
		case Fn::sun_elevation: {
			// The blocks' sun() (spec/blocks.md B21): the cascade's wall second, computed once per
			// second for the rules and the blocks together; none without the clock or a location.
			SunPosition position;
			if (!cascade_sun(position)) {
				return Val::none(n.type);
			}
			return Val::of_real(fn == Fn::sun_azimuth ? position.azimuth : position.elevation);
		}
		case Fn::since_update:
		case Fn::last_known:
		case Fn::quality: {
			const Datapoint* dp = model_.get(set_.node(n.a).a);
			Quality q = dp ? dp->quality() : Quality::unknown;
			if (fn == Fn::quality) {
				const char* text = quality_name(q);
				return Val::of_string(text, std::strlen(text));
			}
			if (q == Quality::unknown) {
				return Val::none(n.type);
			}
			return fn == Fn::since_update ? Val::of_duration(cascade_age(dp->updated_ms())) : val_from_datapoint(dp->value());
		}
		case Fn::since_change:
		case Fn::changed_within: {
			uint16_t index = set_.find_tracked(set_.node(n.a).a);
			bool known = index != kNone && runtime_->changes[index].known;
			int64_t age = known ? cascade_age(runtime_->changes[index].time) : 0;
			if (fn == Fn::since_change) {
				return known ? Val::of_duration(age) : Val::none(n.type);
			}
			Val limit = arg(1);
			if (!known) {
				return Val::of_bool(false);
			}
			if (!limit.has()) {
				return Val::none(n.type);
			}
			return Val::of_bool(age <= limit.u.i);
		}
		case Fn::running:
		case Fn::remaining: {
			const TimerRuntime& t = runtime_->timers[set_.rule(cx.rule).first_timer + set_.node(n.a).a];
			if (fn == Fn::running) {
				return Val::of_bool(t.running);
			}
			if (!t.running) {
				return Val::none(n.type);
			}
			return Val::of_duration(t.deadline > cascade_ms_ ? static_cast<int64_t>(t.deadline - cascade_ms_) : 0);
		}
		case Fn::state: {
			size_t target = n.lit.e;
			const RuleRuntime& rr = runtime_->rules[target];
			if (!rr.has_state) {
				return Val::none(n.type);
			}
			const char* id = set_.str(set_.state(set_.rule(target).first_state + rr.state).id);
			return Val::of_string(id, std::strlen(id));
		}
		case Fn::length: {
			Val s = arg(0);
			return s.has() ? Val::of_int(s.length) : Val::none(n.type);
		}
		case Fn::contains:
		case Fn::starts_with:
		case Fn::ends_with: {
			Val s = arg(0), part = arg(1);
			if (!s.has() || !part.has()) {
				return Val::none(n.type);
			}
			if (fn == Fn::contains) {
				return Val::of_bool(contains_bytes(s, part));
			}
			if (part.length > s.length) {
				return Val::of_bool(false);
			}
			size_t offset = fn == Fn::starts_with ? 0 : static_cast<size_t>(s.length - part.length);
			return Val::of_bool(std::memcmp(s.u.bytes + offset, part.u.bytes, part.length) == 0);
		}
		case Fn::to_string: {
			Val x = arg(0);
			if (!x.has()) {
				return Val::none(n.type);
			}
			return to_string_value(x, cx.error, cx.error_text);
		}
		case Fn::rgb:
		case Fn::rgbw: {
			int64_t levels[4] = {0, 0, 0, 0};
			bool missing = false;
			for (size_t k = 0; k < n.argc; ++k) {
				Val x = arg(k);
				if (!x.has()) {
					missing = true;
				} else {
					levels[k] = x.u.i;
				}
			}
			if (missing) {
				return Val::none(n.type);
			}
			for (size_t k = 0; k < n.argc; ++k) {
				if (levels[k] < 0 || levels[k] > 255) {
					return fail("a color level outside 0..255");
				}
			}
			Color c;
			c.red = static_cast<uint8_t>(levels[0]);
			c.green = static_cast<uint8_t>(levels[1]);
			c.blue = static_cast<uint8_t>(levels[2]);
			c.white = static_cast<uint8_t>(levels[3]);
			c.valid = fn == Fn::rgbw ? Color::kAll : Color::kRgb;
			return Val::of_color(c);
		}
		case Fn::red:
		case Fn::green:
		case Fn::blue:
		case Fn::white: {
			Val c = arg(0);
			if (!c.has()) {
				return Val::none(n.type);
			}
			uint8_t bit = fn == Fn::red ? Color::kRed : (fn == Fn::green ? Color::kGreen : (fn == Fn::blue ? Color::kBlue : Color::kWhite));
			if ((c.u.color.valid & bit) == 0) {
				return Val::none(n.type);
			}
			uint8_t level = fn == Fn::red ? c.u.color.red
										  : (fn == Fn::green ? c.u.color.green : (fn == Fn::blue ? c.u.color.blue : c.u.color.white));
			return Val::of_int(level);
		}
		case Fn::controlled: {
			Val bit = arg(0), number = arg(1);
			if (!bit.has() || !number.has()) {
				return Val::none(n.type);
			}
			if (number.u.i < 0 || number.u.i > 255) {
				return fail("a controlled number outside 0..255");
			}
			return Val::of_controlled(bit.u.b, static_cast<uint8_t>(number.u.i));
		}
		case Fn::control_bit:
		case Fn::control_value: {
			Val x = arg(0);
			if (!x.has()) {
				return Val::none(n.type);
			}
			return fn == Fn::control_bit ? Val::of_bool(x.u.ctl.control) : Val::of_int(x.u.ctl.value);
		}
		case Fn::count: break;
	}
	return fail("unknown function");
}

// ------------------------------------------------------------------------------------------
// Accessors

bool Engine::enabled(size_t rule) const {
	return runtime_ && rule < runtime_->rule_count && runtime_->params[set_.rule(rule).first_parameter].value.u.b;
}

const Val& Engine::parameter_value(size_t rule, size_t index) const {
	return runtime_->params[set_.rule(rule).first_parameter + index].value;
}

bool Engine::parameter_overridden(size_t rule, size_t index) const {
	return runtime_->params[set_.rule(rule).first_parameter + index].overridden;
}

bool Engine::timer_remaining(size_t rule, size_t index, int64_t& ms) const {
	const TimerRuntime& t = runtime_->timers[set_.rule(rule).first_timer + index];
	if (!t.running) {
		return false;
	}
	uint64_t now = clock_.monotonic_ms();
	ms = t.deadline > now ? static_cast<int64_t>(t.deadline - now) : 0;
	return true;
}

const char* Engine::current_state(size_t rule) const {
	const RuleRuntime& rr = runtime_->rules[rule];
	if (!rr.has_state) {
		return nullptr;
	}
	return set_.str(set_.state(set_.rule(rule).first_state + rr.state).id);
}

bool Engine::diagnostics(size_t rule, RuleDiagnostics& out) const {
	if (runtime_ == nullptr || rule >= runtime_->rule_count) {
		return false;
	}
	const Rule& r = set_.rule(rule);
	const RuleRuntime& rr = runtime_->rules[rule];
	out = RuleDiagnostics{};
	out.id = set_.str(r.id);
	out.name = set_.str(r.name);
	out.enabled = enabled(rule);
	out.firings = rr.firings;
	out.errors = rr.errors;
	out.peak_cascade_firings = rr.peak_cascade_firings;
	out.loop_guards = rr.loop_guards;
	out.state = current_state(rule);
	if (rr.last_trigger_kind != kNoKind) {
		out.has_trigger = true;
		out.last_trigger_ms = rr.last_trigger_ms;
		out.last_trigger_wall_valid = rr.last_trigger_wall_valid;
		out.last_trigger_wall = rr.last_trigger_wall;
		switch (static_cast<TriggerKind>(rr.last_trigger_kind)) {
			case TriggerKind::datapoint: {
				const Datapoint* dp = model_.get(rr.last_trigger_ref);
				std::snprintf(out.last_trigger, sizeof(out.last_trigger), "datapoint %s", dp ? dp->name() : "?");
				break;
			}
			case TriggerKind::match: std::snprintf(out.last_trigger, sizeof(out.last_trigger), "match"); break;
			case TriggerKind::schedule:
				std::snprintf(out.last_trigger, sizeof(out.last_trigger), "schedule %s",
					rr.last_trigger_ref < set_.schedule_count() ? set_.schedule(rr.last_trigger_ref) : "?");
				break;
			case TriggerKind::timer:
				std::snprintf(out.last_trigger, sizeof(out.last_trigger), "timer %s",
					set_.str(set_.timer(r.first_timer + rr.last_trigger_ref).id));
				break;
			case TriggerKind::start:
				std::snprintf(out.last_trigger, sizeof(out.last_trigger), "start %s",
					reason_name_of_flag(rr.last_trigger_flags));
				break;
			case TriggerKind::parameter:
				std::snprintf(out.last_trigger, sizeof(out.last_trigger), "parameter %s",
					set_.str(set_.parameter(r.first_parameter + rr.last_trigger_ref).id));
				break;
			case TriggerKind::silence: {
				const Datapoint* dp = rr.last_trigger_ref < set_.silence_count()
					? model_.get(set_.silence(rr.last_trigger_ref).datapoint)
					: nullptr;
				std::snprintf(out.last_trigger, sizeof(out.last_trigger), "silence %s", dp ? dp->name() : "?");
				break;
			}
		}
	}
	if (rr.last_action_kind != kNoKind) {
		out.has_action = true;
		out.last_action_ms = rr.last_action_ms;
		char value[64];
		format_val(rr.last_action_value, value, sizeof(value));
		switch (static_cast<ActionKind>(rr.last_action_kind)) {
			case ActionKind::write: {
				const Datapoint* dp = model_.get(rr.last_action_target);
				std::snprintf(out.last_action, sizeof(out.last_action), "write %s = %s", dp ? dp->name() : "?", value);
				break;
			}
			case ActionKind::start_timer:
			case ActionKind::restart_timer:
				std::snprintf(out.last_action, sizeof(out.last_action), "%s %s %s",
					rr.last_action_kind == static_cast<uint8_t>(ActionKind::start_timer) ? "startTimer" : "restartTimer",
					set_.str(set_.timer(r.first_timer + rr.last_action_target).id), value);
				break;
			case ActionKind::cancel_timer:
				std::snprintf(out.last_action, sizeof(out.last_action), "cancelTimer %s",
					set_.str(set_.timer(r.first_timer + rr.last_action_target).id));
				break;
			case ActionKind::set_parameter:
				std::snprintf(out.last_action, sizeof(out.last_action), "setParameter %s = %s",
					set_.str(set_.parameter(r.first_parameter + rr.last_action_target).id), value);
				break;
			case ActionKind::log: std::snprintf(out.last_action, sizeof(out.last_action), "log"); break;
			case ActionKind::read: {
				const Datapoint* dp = model_.get(rr.last_action_target);
				std::snprintf(out.last_action, sizeof(out.last_action), "read %s", dp ? dp->name() : "?");
				break;
			}
		}
	}
	if (rr.has_error) {
		out.has_error = true;
		out.last_error_ms = rr.last_error_ms;
		out.last_error_code = rr.last_error_code;
		out.last_error_text = rr.last_error_text;
	}
	return true;
}

EngineMemory Engine::memory() const {
	EngineMemory m{};
	m.object = sizeof(Engine);
	m.runtime = runtime_ ? runtime_->bytes : 0;
	m.per_rule = sizeof(RuleRuntime);
	m.per_parameter = sizeof(ParamRuntime);
	m.per_timer = sizeof(TimerRuntime);
	m.per_node = 1;
	m.per_tracked = sizeof(ChangeRecord);
	m.per_silence = sizeof(SilenceRuntime);
	m.per_action = 1;
	return m;
}

} // namespace hc::rules
