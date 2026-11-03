/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_RULES_ENGINE_H
#define HC_RULES_ENGINE_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/json_value.h"
#include "hc/log.h"
#include "hc/platform/clock.h"
#include "hc/platform/settings.h"
#include "hc/rules/parameter_store.h"
#include "hc/rules/rule_set.h"
#include "hc/rules/value.h"
#include "hc/technology.h"

namespace hc::rules {

/// Why rules start (spec 7.5).
enum class StartReason : uint8_t { boot, image, enable };

/// "boot", "image", "enable".
const char* start_reason_name(StartReason reason);

/// The sun events of the time service.
enum class SunEvent : uint8_t { sunrise, sunset, dawn, dusk };

/// The wall clock and the sun as the rules see them (spec D9, 7.7). The time service of issue #9
/// implements it (local time from the time zone of the image, the sun times from the
/// coordinates); the tests use a fake.
class TimeSource {
public:
	/// The current local time with its day of the week (1 Monday .. 7 Sunday); its only flag is
	/// DateTime::kSummerTime while daylight saving time is in effect. False while the wall clock is
	/// not synchronized.
	virtual bool local_now(DateTime& out) const = 0;
	/// Today's local time of `event`; false without coordinates or when it does not occur today.
	virtual bool sun_time(SunEvent event, TimeOfDay& out) const = 0;

	/// The instant (seconds since the epoch, UTC) of the local civil time `local` (spec 4.6): a time
	/// that occurs twice (the hour after a fall-back) is its daylight-saving occurrence when
	/// DateTime::kSummerTime is set and its standard-time occurrence otherwise; a time that does
	/// not exist (a spring-forward gap) is the instant the gap ends. The other flags and the day of
	/// the week are ignored. The default is the zone UTC (the fields are the instant).
	virtual int64_t local_to_utc(const DateTime& local) const {
		return days_of(local.year, local.month, local.day) * 86400 + local.hour * 3600 + local.minute * 60 +
			local.second;
	}
	/// The local civil time of an instant, with its day of the week and kSummerTime while daylight
	/// saving time is in effect; false if the zone cannot convert it. The default is UTC.
	virtual bool utc_to_local(int64_t utc_seconds, DateTime& out) const {
		int64_t days = utc_seconds >= 0 ? utc_seconds / 86400 : (utc_seconds - 86399) / 86400;
		int64_t rest = utc_seconds - days * 86400;
		int32_t year = 0;
		unsigned month = 0, day = 0;
		civil_of(days, year, month, day);
		if (year < 1900 || year > 2155) {
			return false;
		}
		out = DateTime();
		out.year = static_cast<uint16_t>(year);
		out.month = static_cast<uint8_t>(month);
		out.day = static_cast<uint8_t>(day);
		out.hour = static_cast<uint8_t>(rest / 3600);
		out.minute = static_cast<uint8_t>(rest % 3600 / 60);
		out.second = static_cast<uint8_t>(rest % 60);
		out.day_of_week = weekday_of(year, month, day);
		return true;
	}

protected:
	~TimeSource() = default;
};

/// The log codes of spec 9.2.
enum class LogCode : uint8_t {
	log,
	condition_no_value,
	value_no_value,
	eval_error,
	write_refused,
	timer_refused,
	parameter_refused,
	parameter_reset,
	state_reset,
	loop_guard,
	queue_overflow,
	read_refused,
};

/// The spec's name of a code ("conditionNoValue", ...).
const char* log_code_name(LogCode code);

/// Observes what the engine does, besides its log lines (tag hc.rule). For the corpus runner and the
/// UI's live feed (#14); the parameters are mirrored as datapoints by the engine itself (spec 8.6).
/// Called on the event loop's thread; must not call back into the engine.
class EngineObserver {
public:
	/// Every log entry with a code of spec 9.2: the rule id ("-" when no rule is concerned), the
	/// level and the text (for `log`, the formatted message).
	virtual void rule_logged(const char* rule, LogCode code, LogLevel level, const char* text) {
		(void)rule, (void)code, (void)level, (void)text;
	}
	/// Every successful DatapointModel::command() by a rule.
	virtual void rule_commanded(const char* rule, const Datapoint& datapoint, const Value& value) {
		(void)rule, (void)datapoint, (void)value;
	}
	/// Every read request of a rule that the owning technology accepted (DatapointModel::read()).
	virtual void rule_read(const char* rule, const Datapoint& datapoint) { (void)rule, (void)datapoint; }
	/// A parameter's current value changed (UI, API, action, reset, new image).
	virtual void parameter_changed(const char* rule, const char* parameter, const Val& value) {
		(void)rule, (void)parameter, (void)value;
	}
	/// Right after the loopGuard entry of `rule` (the rule refused): the rule with the most derived
	/// firings in the aborted cascade (the first in rule order on a tie) and that count (spec 9.2).
	virtual void loop_guard(const char* rule, const char* culprit, uint32_t firings) {
		(void)rule, (void)culprit, (void)firings;
	}

protected:
	~EngineObserver() = default;
};

/// Outcome of a parameter change from the UI or API (spec 8.2).
enum class ParameterStatus : uint8_t {
	ok,
	unknown_rule,
	unknown_parameter,
	not_adjustable,
	invalid_value,
};

/// "ok", "unknown rule", ...
const char* parameter_status_name(ParameterStatus status);

/// The per-rule diagnostics of spec 9.1 (the timers and parameters through their own accessors).
struct RuleDiagnostics {
	const char* id = "";
	const char* name = "";
	bool enabled = false;
	uint32_t firings = 0;                ///< Since the rules started at boot.
	bool has_trigger = false;
	uint64_t last_trigger_ms = 0;        ///< Monotonic cascade time.
	bool last_trigger_wall_valid = false;
	DateTime last_trigger_wall;          ///< Local wall time when synchronized.
	char last_trigger[80] = {};          ///< "datapoint hall.presence", "timer off", "start boot", "silence hall.lux", ...
	bool has_action = false;
	uint64_t last_action_ms = 0;
	char last_action[96] = {};           ///< "write hall.light = true", "restartTimer off 3min", "read hall.lux", ...
	uint32_t errors = 0;                 ///< Logged errors and warnings (not `log` actions).
	bool has_error = false;
	uint64_t last_error_ms = 0;
	LogCode last_error_code = LogCode::log;
	const char* last_error_text = "";
	const char* state = nullptr;         ///< Current state id; nullptr without states.
	uint32_t peak_cascade_firings = 0;   ///< Most derived firings of this rule in one cascade (loop-guard culprit).
	uint32_t loop_guards = 0;            ///< Cascades refused while this rule fired.
};

/// What the engine costs (besides the RuleSet).
struct EngineMemory {
	size_t object;    ///< sizeof(Engine): queues and bookkeeping.
	size_t runtime;   ///< The runtime block allocated per loaded set.
	size_t per_rule;  ///< Runtime bytes per rule, per parameter, per timer, per node, per tracked datapoint.
	size_t per_parameter;
	size_t per_timer;
	size_t per_node;
	size_t per_tracked;
	size_t per_silence; ///< Per silence trigger.
	size_t per_action;  ///< Per action (the once-flags of readRefused).
};

/// The rule interpreter (spec/rules.md section 7, doc/rules.md): owns the loaded RuleSet and its
/// runtime (parameter values, timers, states, match memories, change tracking, diagnostics),
/// listens to the datapoint model for triggers, runs cascades on the event loop and commands
/// datapoints through DatapointModel::command().
///
/// External events (datapoint updates from outside, schedule firings, parameter changes from the
/// UI or API, starts) are queued and processed by a posted callback, one cascade each, in arrival
/// order; the expiries of rule timers due before are processed first. A listener callback never
/// commands the model: it only queues.
///
/// Memory: the RuleSet's block and one runtime block per load, both allocated by load(); the
/// queues are part of the object. Nothing is allocated while events are processed.
///
/// Parameters as datapoints (spec 8.6): the engine owns the datapoints `rule/<ruleId>/<parameterId>`
/// that the model holds (the image loader derives them, parameter_datapoints()) through a technology
/// of its own named "rules". It publishes each parameter's value there whenever it changes, and a
/// command on such a datapoint sets the parameter like the API (adjustable, valid; refused with
/// parameterRefused otherwise). Silence triggers (spec 7.11) are slots allocated at load.
///
/// Threading: every method on the event loop's thread.
class Engine : public DatapointListener {
public:
	/// External events waiting for their cascade.
	static constexpr size_t kInboundCapacity = 64;

	/// The name of the technology that owns the parameter datapoints.
	static constexpr const char* kTechnologyName = "rules";

	Engine(EventLoop& loop, DatapointModel& model, const Clock& clock, Settings& settings, const TimeSource& time);
	~Engine();

	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;

	/// Validates and compiles `rules` (the `rules` member of an image) and swaps it in (spec 7.6:
	/// what survives by rule id, timer id and state id; overrides checked against the new
	/// declarations; the first load is a boot and reads the persisted overrides). Does not start
	/// the rules: call start() (boot after the first load, image after a swap). A refused load
	/// changes nothing. Pending external events are processed with the old rules first.
	LoadResult load(const JsonValue& rules, const ScheduleDirectory* schedules) {
		return load(rules, schedules, nullptr);
	}

	/// As above, after the datapoint model was replaced (the image swap): `previous_ids[id]` is the
	/// id the datapoint `id` of the new model had in the previous one, or kNone for a new datapoint
	/// (one entry per datapoint of the model). The change records (7.7) and the diagnostics follow
	/// a datapoint by it. Call flush() before the model is replaced: queued events carry old ids.
	LoadResult load(const JsonValue& rules, const ScheduleDirectory* schedules, const uint16_t* previous_ids);

	/// As load() with `previous_ids`, for a set the caller compiled and validated already against a
	/// model whose datapoints have the ids of this engine's model (the image loader's check, which
	/// compiles the rules once per image: homecontrol #66). Swaps it in like load(); on refusal (no
	/// memory for the runtime) nothing changes and `set` is gone.
	LoadResult adopt(RuleSet&& set, const uint16_t* previous_ids);

	/// Starts every rule with `reason` (boot or image, spec 7.5), each in its own cascade, after
	/// re-baselining the match memories (7.4). Queued; runs on the loop.
	void start(StartReason reason);

	/// The scheduler (issue #10) fired schedule entry `id`. Queued.
	void schedule_fired(const char* id);

	/// Sets a parameter from the UI or API (spec 8.2): validated, made the override, persisted; a
	/// parameter-changed cascade is queued if the value differs. On refusal `reason` (if given)
	/// points to a static text, and parameterRefused is logged.
	ParameterStatus set_parameter(const char* rule, const char* parameter, const JsonValue& value,
		const char** reason = nullptr);
	ParameterStatus set_parameter(const char* rule, const char* parameter, const Val& value,
		const char** reason = nullptr);
	/// Removes the override: the value returns to the default (an event if that changes it).
	ParameterStatus reset_parameter(const char* rule, const char* parameter, const char** reason = nullptr);

	/// Processes the queued external events now (call before reloading the datapoint model).
	void flush();

	/// Binds the parameter datapoints of the loaded rules that the model holds to the engine and
	/// publishes the parameters' values where the datapoints differ (spec 8.6). load() does it; call
	/// it again after the model was reloaded without a rule load (the load drops every binding).
	/// The publishes are not events for the rules.
	void bind_parameters();

	/// Runs `publish` with the datapoint updates it causes taken as no events for the rules (change
	/// tracking still follows them): another component's datapoints taking their values at load, like
	/// the schedules' time datapoints (spec/schedules.md 2.2), as the parameter datapoints do here.
	template <class F>
	void quietly(F publish) {
		bool was = quiet_;
		quiet_ = true;
		publish();
		quiet_ = was;
	}

	/// The technology that owns the parameter datapoints (named kTechnologyName).
	const Technology& technology() const { return technology_; }
	/// The rule and parameter index behind a parameter datapoint; false for another datapoint.
	bool parameter_of(uint16_t datapoint, size_t& rule, size_t& index) const;

	/// How long a changed rule state waits before it is written (spec 8.4, 8.5: writes are delayed
	/// by up to 10 s and coalesced to protect the flash).
	static constexpr uint32_t kStatePersistDelayMs = 10000;

	/// Writes the rule states whose write is still delayed (an orderly shutdown; the destructor does
	/// it too). Writes nothing for a state the store already holds.
	void persist_now();

	void set_observer(EngineObserver* observer) { observer_ = observer; }

	const RuleSet& rules() const { return set_; }
	bool loaded() const { return runtime_ != nullptr; }

	bool diagnostics(size_t rule, RuleDiagnostics& out) const;
	/// The current value of parameter `index` (0 is `enabled`) of `rule`.
	const Val& parameter_value(size_t rule, size_t index) const;
	bool parameter_overridden(size_t rule, size_t index) const;
	/// Remaining ms of timer `index` of `rule` at the clock's current time; false when stopped.
	bool timer_remaining(size_t rule, size_t index, int64_t& ms) const;
	/// The current state id; nullptr without states.
	const char* current_state(size_t rule) const;
	bool enabled(size_t rule) const;

	EngineMemory memory() const;

	void datapoint_updated(const DatapointUpdate& update) override;

private:
	/// The technology of the parameter datapoints: their commands go to parameter_command().
	class ParameterTechnology : public Technology {
	public:
		explicit ParameterTechnology(Engine& engine) : engine_(engine) {}
		const char* name() const override { return kTechnologyName; }
		bool command(const Datapoint& datapoint, const Value& value) override {
			return engine_.parameter_command(datapoint, value);
		}

	private:
		Engine& engine_;
	};

	struct Event {
		uint8_t kind;
		uint8_t flags;
		uint16_t a;
		uint16_t b;
		uint16_t c;
	};

	struct RuleRuntime;
	struct ParamRuntime;
	struct TimerRuntime;
	struct ChangeRecord;
	struct SilenceRuntime;
	struct Eval;
	struct Runtime;

	// Queues.
	bool push_inbound(const Event& event);
	bool push_cascade(const Event& event, size_t rule);
	void post_drain();
	void drain();
	void on_timer();
	void process_due_timers();
	void reschedule();

	// Cascades.
	void begin_cascade();
	void end_cascade();
	void run_cascade(const Event& event);
	void run_start(size_t rule, StartReason reason);
	void process_queue();
	void process_event(const Event& event, bool derived);
	bool trigger_matches(const Trigger& trigger, size_t rule, const Event& event) const;
	bool match_references(const Trigger& trigger, size_t rule, const Event& event) const;
	void run_actions(size_t rule, const ActionList& list);
	void run_action(size_t rule, const Action& action);
	void action_write(size_t rule, const Action& action);
	void action_timer(size_t rule, const Action& action);
	void action_set_parameter(size_t rule, const Action& action);
	void action_log(size_t rule, const Action& action);
	void action_read(size_t rule, const Action& action);
	void stop_timers(size_t rule);
	void enable_changed(size_t rule, bool enabled);

	// Silence triggers (spec 7.11).
	void arm_silence(size_t index, uint64_t base);
	void rearm_silences(size_t rule);
	void start_silences();
	void record_action(size_t rule, ActionKind kind, uint16_t target, const Val& value);

	// Parameters.
	bool parameter_valid(const Parameter& declaration, const Val& value) const;
	/// `publish`: tell the parameter's datapoint (not when the change came as its command).
	void apply_parameter(size_t rule, size_t index, const Val& value, bool derived, bool publish = true);
	void publish_parameter(size_t rule, size_t index);
	bool parameter_command(const Datapoint& datapoint, const Value& value);
	ParameterStatus find_adjustable(const char* rule, const char* parameter, int& r, int& p, const char*& reason);
	void refuse_parameter(const char* rule, const char* parameter, const char* reason);

	// Evaluation.
	Val evaluate(uint16_t root, size_t rule, bool& error);
	Val eval(uint16_t index, Eval& cx);
	Val eval_call(const Node& node, Eval& cx);
	Val eval_binary(const Node& node, Eval& cx);
	Val literal(const Node& node) const;
	Val datapoint_value(uint16_t id) const;
	int64_t cascade_age(uint64_t time) const;

	// Logging.
	void log_rule(size_t rule, LogCode code, LogLevel level, const char* detail, const char* fmt, ...)
		HC_PRINTF_FORMAT(6, 7);
	void log_code(const char* rule_id, RuleRuntime* runtime, LogCode code, LogLevel level, const char* detail,
		const char* text);

	// Swap.
	bool install(RuleSet&& next, LoadResult& result, const uint16_t* previous_ids);

	// State persistence (spec 8.5).
	void state_changed(size_t rule);
	void on_persist();

	EventLoop& loop_;
	DatapointModel& model_;
	const Clock& clock_;
	Settings& settings_;
	ParameterStore store_;
	const TimeSource& time_;
	EngineObserver* observer_ = nullptr;
	bool listening_ = false;
	ParameterTechnology technology_{*this};
	bool quiet_ = false;          ///< Publishing at load: no events.
	uint64_t boot_start_ms_ = 0;  ///< When the rules started at boot (silence counts from it).

	RuleSet set_;
	Runtime* runtime_ = nullptr;
	bool booted_ = false;

	Event inbound_[kInboundCapacity] = {};
	size_t inbound_head_ = 0;
	size_t inbound_count_ = 0;
	bool inbound_overflow_logged_ = false;
	bool drain_posted_ = false;

	Event queue_[Limits::kQueue] = {};
	size_t queue_head_ = 0;
	size_t queue_count_ = 0;

	bool in_cascade_ = false;
	bool aborted_ = false;
	bool overflow_logged_ = false;
	size_t derived_firings_ = 0;
	uint64_t cascade_ms_ = 0;
	bool wall_valid_ = false;
	DateTime wall_;
	size_t current_rule_ = 0; ///< The rule whose actions run (for queueOverflow).

	TimerHandle loop_timer_;
	uint64_t loop_deadline_ = 0;
	TimerHandle persist_timer_;
	bool persist_pending_ = false;
	uint32_t timer_sequence_ = 0;
};

} // namespace hc::rules

#endif // HC_RULES_ENGINE_H
