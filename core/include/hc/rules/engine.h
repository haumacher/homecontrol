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
	/// The current local time with its day of the week (1 Monday .. 7 Sunday) and no flags; false
	/// while the wall clock is not synchronized.
	virtual bool local_now(DateTime& out) const = 0;
	/// Today's local time of `event`; false without coordinates or when it does not occur today.
	virtual bool sun_time(SunEvent event, TimeOfDay& out) const = 0;

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
};

/// The spec's name of a code ("conditionNoValue", ...).
const char* log_code_name(LogCode code);

/// Observes what the engine does, besides its log lines (tag hc.rule). For the corpus runner, the
/// UI's live feed (#14) and later a mirror of the parameters as datapoints (decision ticket #28:
/// parameter_changed is the hook). Called on the event loop's thread; must not call back into the
/// engine.
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
	/// A parameter's current value changed (UI, API, action, reset, new image).
	virtual void parameter_changed(const char* rule, const char* parameter, const Val& value) {
		(void)rule, (void)parameter, (void)value;
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
	char last_trigger[48] = {};          ///< "datapoint hall.presence", "timer off", "start boot", ...
	bool has_action = false;
	uint64_t last_action_ms = 0;
	char last_action[96] = {};           ///< "write hall.light = true", "restartTimer off 3min", ...
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
/// Threading: every method on the event loop's thread.
class Engine : public DatapointListener {
public:
	/// External events waiting for their cascade.
	static constexpr size_t kInboundCapacity = 64;

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
	void stop_timers(size_t rule);
	void record_action(size_t rule, ActionKind kind, uint16_t target, const Val& value);

	// Parameters.
	bool parameter_valid(const Parameter& declaration, const Val& value) const;
	void apply_parameter(size_t rule, size_t index, const Val& value, bool derived);
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
	bool check_parameter_keys(const RuleSet& next, LoadResult& result) const;

	EventLoop& loop_;
	DatapointModel& model_;
	const Clock& clock_;
	ParameterStore store_;
	const TimeSource& time_;
	EngineObserver* observer_ = nullptr;
	bool listening_ = false;

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
	uint32_t timer_sequence_ = 0;
};

} // namespace hc::rules

#endif // HC_RULES_ENGINE_H
