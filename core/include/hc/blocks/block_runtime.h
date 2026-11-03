/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_BLOCKS_BLOCK_RUNTIME_H
#define HC_BLOCKS_BLOCK_RUNTIME_H

#include <cstddef>
#include <cstdint>

#include "hc/blocks/block_set.h"
#include "hc/blocks/block_type.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/json.h"
#include "hc/log.h"
#include "hc/platform/blob_store.h"
#include "hc/platform/clock.h"
#include "hc/platform/settings.h"
#include "hc/rules/engine.h"
#include "hc/rules/parameter_store.h"
#include "hc/technology.h"

namespace hc::blocks {

/// An instance's status (spec/blocks.md 9).
enum class BlockStatus : uint8_t { running, disabled, fault };
/// "running", "disabled", "fault".
const char* block_status_name(BlockStatus status);

/// The log codes of spec/blocks.md 9.
enum class BlockLogCode : uint8_t {
	log,
	fault,
	write_refused,
	timer_refused,
	parameter_refused,
	parameter_reset,
	state_reset,
	loop_guard,
};
/// "log", "fault", "writeRefused", ...
const char* block_log_code_name(BlockLogCode code);

/// The kinds of call (diagnostics).
enum class CallKind : uint8_t { none, start, input, timer, period, parameter };
/// "start", "input", "timer", "period", "parameter"; "none".
const char* call_kind_name(CallKind kind);

/// Observes the runtime besides its log lines (tag hc.block): the corpus runner, later the live
/// feed. Called on the event loop's thread; must not call back into the runtime.
class BlockObserver {
public:
	virtual void block_logged(const char* block, BlockLogCode code, LogLevel level, const char* text) {
		(void)block, (void)code, (void)level, (void)text;
	}
	/// Every successful DatapointModel::command() of a block's output.
	virtual void block_commanded(const char* block, const Datapoint& datapoint, const Value& value) {
		(void)block, (void)datapoint, (void)value;
	}
	/// Right after the loopGuard entry of `block`: the culprit and its derived firings or calls.
	virtual void loop_guard(const char* block, const char* culprit, uint32_t firings) {
		(void)block, (void)culprit, (void)firings;
	}

protected:
	~BlockObserver() = default;
};

/// Outcome of a change from the UI or API (spec/api.md "Blocks").
enum class ChangeStatus : uint8_t { ok, unknown_block, unknown_parameter, not_adjustable, invalid_value, not_faulted };
/// The API's code suffix: "unknown", "unknown_parameter", "not_adjustable", "invalid_value", "not_faulted".
const char* change_status_name(ChangeStatus status);

/// The diagnostics of an instance (spec/blocks.md 9).
struct BlockDiagnostics {
	BlockStatus status = BlockStatus::disabled;
	const char* fault = "";       ///< The fault's reason ("" when none).
	uint64_t fault_ms = 0;
	uint32_t calls = 0;           ///< Since the boot.
	CallKind last_call = CallKind::none;
	uint64_t last_call_ms = 0;
	const char* last_call_arg = ""; ///< The port, timer or parameter of the last call ("" for none).
	uint32_t time_ms = 0;         ///< Monotonic ms spent in calls (total).
	uint32_t longest_ms = 0;
	uint32_t errors = 0;          ///< Logged problems (not `log`).
	bool has_error = false;
	BlockLogCode last_error_code = BlockLogCode::log;
	const char* last_error = "";
	uint64_t last_error_ms = 0;
	bool restored = false;        ///< The last start had its state record from before.
};

/// What the runtime costs (the host test reports it for the C6).
struct BlockMemory {
	size_t object;      ///< sizeof(BlockRuntime).
	size_t runtime;     ///< The block of the instances' runtime data.
	size_t set;         ///< The compiled BlockSet.
};

/// The block runtime (spec/blocks.md, doc/blocks.md): holds the compiled instances and their
/// runtime (state records, parameter values, timers, the period grid, the diagnostics), owns the
/// mirror datapoints `block/<id>/<x>` (technology "blocks"), persists the state records in the
/// BlobStore and the parameter overrides in the store of the rule parameters, and is called by the
/// rule engine inside its cascades (rules::BlockHost): after the rules for every event, for its
/// timers and periods ordered with the rule timers, and after the rules' start cascades.
///
/// Memory: one block per adopt() for the instances' runtime (and the interest bitmap); nothing is
/// allocated while events are processed. Threading: the event loop's thread.
class BlockRuntime : public rules::BlockHost {
public:
	static constexpr const char* kTechnologyName = "blocks";
	/// Prefix of the record blobs: `b.` + the FNV-1a hash of the block id (doc/blocks.md).
	static constexpr const char* kBlobPrefix = "b.";
	/// Kinds of BlockHost::block_event().
	enum EventKind : uint8_t { kParameterChanged, kEnabledChanged, kRestart };

	BlockRuntime(EventLoop& loop, DatapointModel& model, const Clock& clock, Settings& settings, BlobStore& blobs,
		rules::Engine& engine);
	~BlockRuntime();

	BlockRuntime(const BlockRuntime&) = delete;
	BlockRuntime& operator=(const BlockRuntime&) = delete;

	/// Swaps `set` in (spec/blocks.md 8.1): records and overrides follow the block id (kept for the
	/// same type and major version), the first adopt since construction reads the persisted records
	/// and overrides (the boot), the mirrors are bound and take their values quietly, records and
	/// overrides of blocks no longer in the image are deleted. Does not start the blocks: the
	/// engine's start() does, after the rules. False (and `set` gone) without memory: nothing changed.
	bool adopt(BlockSet&& set);
	/// No blocks: the instances go (their timers with them). With `forget` their persisted records
	/// and overrides are deleted too (the image was removed); without, they stay for the image that
	/// is loaded next (a teardown before a restore).
	void clear(bool forget);
	/// Binds the mirrors again after the model was reloaded without an adopt (dynamic datapoints).
	void bind();
	/// Writes every due record now (an orderly reboot).
	void flush();

	void set_observer(BlockObserver* observer) { observer_ = observer; }

	// --- The UI and the API (spec/api.md "Blocks"); changes run their calls on the loop. ---
	ChangeStatus set_parameter(const char* block, const char* parameter, const JsonSpan& value, const char** reason);
	ChangeStatus set_parameter(const char* block, const char* parameter, double value, const char** reason);
	ChangeStatus reset_parameter(const char* block, const char* parameter, const char** reason);
	ChangeStatus set_enabled(const char* block, bool enabled, const char** reason);
	ChangeStatus restart(const char* block, const char** reason);
	/// The checks of the changes above without changing or logging anything, for a change of several
	/// members applied whole or refused whole (PUT /api/blocks/<id>): `value` nullptr checks a reset;
	/// `out` (optional) receives the value. A refusal is then logged with refused().
	ChangeStatus check_parameter(const char* block, const char* parameter, const JsonSpan* value, double* out,
		const char** reason) const;
	ChangeStatus check_enabled(const char* block, const char** reason) const;
	ChangeStatus check_restart(const char* block, const char** reason) const;
	/// Logs the refusal of a change (parameterRefused) found by a check.
	void refused(const char* block, const char* parameter, const char* reason) { refuse(block, parameter, reason); }

	size_t size() const { return set_.size(); }
	const BlockSet& set() const { return set_; }
	int find(const char* id) const { return set_.find(id); }
	bool diagnostics(size_t block, BlockDiagnostics& out) const;
	const double* state(size_t block) const;
	double param(size_t block, size_t index) const;
	bool overridden(size_t block, size_t index) const;
	bool enabled(size_t block) const;
	bool enabled_overridden(size_t block) const;
	/// An input as a call would read it now.
	InputValue input(size_t block, size_t port) const;
	/// The datapoint id of a port's mirror, or rules::kNone.
	uint16_t mirror(size_t block, size_t port) const;
	/// Whether a timer runs and its remaining ms at the clock's current time.
	bool timer_remaining(size_t block, size_t timer, int64_t& ms) const;
	const Technology& technology() const { return technology_; }
	BlockMemory memory() const;

	// --- rules::BlockHost ---
	bool wants(uint16_t datapoint) const override;
	void datapoint_event(uint16_t datapoint, bool derived) override;
	void block_event(uint8_t kind, uint16_t block, uint16_t arg, bool derived) override;
	uint64_t next_deadline() const override;
	void run_due(uint64_t now) override;
	size_t start_count() const override { return set_.size(); }
	void start_block(size_t index, rules::StartReason reason) override;
	void cascade_begun() override;
	bool busiest(const char*& id, uint32_t& calls) const override;

	/// The record format (frozen, doc/blocks.md): encodes the persisted fields of `values` (one per
	/// manifest field). False if it does not fit `size`.
	static bool encode_record(const Manifest& manifest, const char* block, const double* values, uint8_t* out,
		size_t size, size_t& length);
	/// Decodes a record: its type and major version, and each persisted field of `manifest` found by
	/// id into `values` (others untouched). False for a damaged record or another type or major.
	static bool decode_record(const Manifest& manifest, const char* block, const uint8_t* data, size_t length,
		double* values, const char** reason);

private:
	class MirrorTechnology : public Technology {
	public:
		explicit MirrorTechnology(BlockRuntime& runtime) : runtime_(runtime) {}
		const char* name() const override { return kTechnologyName; }
		bool command(const Datapoint& datapoint, const Value& value) override {
			return runtime_.mirror_command(datapoint, value);
		}

	private:
		BlockRuntime& runtime_;
	};

	struct Instance;
	struct Runtime;
	class Context;
	friend class Context;

	bool call(size_t block, CallKind kind, size_t arg, bool derived, StartReason reason = StartReason::boot,
		bool restored = false);
	void fault(size_t block, const char* reason);
	void stop(size_t block, BlockStatus status);
	void write_failsafe(size_t block);
	void write_output(size_t block, size_t port, double value);
	void publish_mirror(uint16_t id, ValueType type, double value, Quality quality);
	InputValue aggregate(const Instance& instance, size_t port) const;
	bool mirror_command(const Datapoint& datapoint, const Value& value);
	ChangeStatus find_param(const char* block, const char* parameter, int& b, int& p, const char*& reason) const;
	void apply_param(size_t block, size_t index, double value);
	void apply_enabled(size_t block, bool enabled);
	void refuse(const char* block, const char* parameter, const char* reason);
	void log(size_t block, BlockLogCode code, LogLevel level, const char* format, ...) HC_PRINTF_FORMAT(5, 6);
	void log_text(const char* id, Instance* instance, BlockLogCode code, LogLevel level, const char* text);
	bool load_record(size_t block, bool& mismatch);
	void record_changed(size_t block);
	void write_record(size_t block);
	void schedule_persist();
	void on_persist();
	void release(Runtime* runtime);
	void prune(bool forget_all);

	EventLoop& loop_;
	DatapointModel& model_;
	const Clock& clock_;
	Settings& settings_;
	BlobStore& blobs_;
	rules::Engine& engine_;
	rules::ParameterStore store_;
	BlockObserver* observer_ = nullptr;
	MirrorTechnology technology_{*this};
	BlockSet set_;
	Runtime* runtime_ = nullptr;
	bool booted_ = false;
	uint32_t persist_interval_ms_ = 0; ///< The pacing of spec/blocks.md B13, per instance.
	TimerHandle persist_timer_;
	uint64_t persist_deadline_ = 0;
};

} // namespace hc::blocks

#endif // HC_BLOCKS_BLOCK_RUNTIME_H
