/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_BLOCKS_BLOCK_TYPE_H
#define HC_BLOCKS_BLOCK_TYPE_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"

namespace hc::blocks {

/// The value types of a port (spec/blocks.md B6): every value is a double at the interface.
enum class PortType : uint8_t { boolean, integer, real, enumeration };

/// The value types of a parameter: the port types plus duration (ms) and time of day (seconds of the day).
enum class ParamType : uint8_t { boolean, integer, real, duration, time_of_day, enumeration };

enum class Direction : uint8_t { in, out };

/// Which updates of an input's binding call input(port) (spec/blocks.md 5.1).
enum class InputTrigger : uint8_t { change, update, none };

/// How an input bound to several datapoints combines them (spec/blocks.md 5.3).
enum class Aggregation : uint8_t { none, mean, min, max, any, all };

/// Bit of an aggregation in PortDecl::aggregations.
constexpr uint8_t aggregation_bit(Aggregation a) { return static_cast<uint8_t>(1u << static_cast<unsigned>(a)); }

/// "boolean", "integer", "real", "enumeration".
const char* port_type_name(PortType type);
/// "boolean", "integer", "real", "duration", "timeOfDay", "enumeration".
const char* param_type_name(ParamType type);
/// "mean", "min", "max", "any", "all"; "none".
const char* aggregation_name(Aggregation a);
/// The aggregation named `name`; none if unknown.
Aggregation aggregation_of(const char* name);
/// "change", "update", "none".
const char* input_trigger_name(InputTrigger trigger);

/// A choice of an integer or enumeration parameter: the image names it by `id` or `value`.
struct Choice {
	double value;
	const char* id;
	const char* label;
};

/// A port of a manifest (spec/blocks.md 3).
struct PortDecl {
	const char* id;
	Direction direction;
	PortType type;
	const char* label;
	const char* unit;           ///< "" for none.
	bool required;              ///< Inputs.
	uint8_t multiple;           ///< Inputs: 1..4 datapoints.
	uint8_t aggregations;       ///< Inputs with multiple > 1: aggregation_bit()s allowed.
	Aggregation default_aggregation;
	InputTrigger trigger;       ///< Inputs.
	bool diagnostic;            ///< Outputs: no failsafe.
	double failsafe;            ///< Outputs that are not diagnostic.
	bool has_min;
	double min;                 ///< Outputs: a write outside is a fault.
	bool has_max;
	double max;
};

/// A parameter of a manifest (spec/blocks.md 3; spec/rules.md 8.1).
struct ParamDecl {
	const char* id;
	ParamType type;
	const char* label;
	const char* unit;
	double default_value;
	bool has_min;
	double min;
	bool has_max;
	double max;
	double step;                ///< 0: none (a hint for the UI).
	const Choice* choices;
	uint8_t choice_count;
	bool adjustable;
};

/// A field of the state record (spec/blocks.md 8.1, B12).
struct StateField {
	const char* id;
	bool persist;
	double initial;
};

/// The manifest of a block type (spec/blocks.md 3): static data of a built-in type, or parsed from
/// a loaded module's custom section later.
struct Manifest {
	const char* type;
	uint16_t major;
	uint16_t minor;
	const char* label;
	const char* description;
	const PortDecl* ports;
	uint8_t port_count;
	const ParamDecl* params;
	uint8_t param_count;
	const StateField* state;
	uint8_t state_count;
	const char* const* timers;
	uint8_t timer_count;
	uint32_t period_ms;          ///< 0: no period() calls.
	uint32_t persist_every_ms;   ///< Shortest interval between two writes of the record.

	/// Index of the port / parameter / state field / timer `id`; -1 if none.
	int port(const char* id) const;
	int param(const char* id) const;
	int field(const char* id) const;
	int timer(const char* id) const;
	/// Whether any state field persists.
	bool persists() const;
	/// The number of input ports (they come in any order among the outputs).
	size_t inputs() const;
};

/// The limits of spec/blocks.md 11.
struct BlockLimits {
	static constexpr size_t kBlocks = 32;
	static constexpr size_t kMultiple = 4;
	static constexpr size_t kPorts = 24;
	static constexpr size_t kParams = 32;
	static constexpr size_t kStateFields = 32;
	static constexpr size_t kTimers = 4;
	static constexpr int64_t kTimerMinMs = 1;
	static constexpr int64_t kTimerMaxMs = 30LL * 86400000LL;
	static constexpr size_t kRecordBytes = 512;
	static constexpr uint32_t kRecordWritesPerHour = 30;
	static constexpr size_t kIdLength = 32;
};

/// Checks a manifest against spec/blocks.md 3 (ids, namespaces, reserved `enabled`, limits, the
/// record size); nullptr when valid, else the reason. The tests run it on every built-in type; a
/// loaded module's manifest will be checked at load.
const char* check_manifest(const Manifest& manifest);

/// Why an instance starts (spec/blocks.md 3.1).
enum class StartReason : uint8_t { boot, image, enable, restart };

/// "boot", "image", "enable", "restart".
const char* start_reason_name(StartReason reason);

/// An input as a call reads it (spec/blocks.md 5.3).
struct InputValue {
	double value = 0;
	Quality quality = Quality::unknown;
	uint64_t updated_ms = 0;   ///< Monotonic time of the latest update of the datapoints combined.
	bool bound = false;

	bool good() const { return quality == Quality::good; }
	/// Good and true (a boolean input).
	bool is_true() const { return quality == Quality::good && value != 0; }
};

/// What a call of a block type may use (spec/blocks.md 3.1): the imports of a loaded module, the
/// runtime's services for a built-in type. Valid only during the call.
class BlockContext {
public:
	/// The cascade time, monotonic ms.
	virtual uint64_t now() const = 0;
	/// The local wall time with its day of the week (1 Monday .. 7 Sunday); false while not synchronized.
	virtual bool wall(DateTime& out) const = 0;
	virtual InputValue read(size_t port) const = 0;
	virtual double param(size_t index) const = 0;
	/// Writes an output (spec/blocks.md 5.4); an invalid value faults the instance.
	virtual void write(size_t port, double value) = 0;
	/// Starts or restarts a timer `ms` after the cascade time (1 ms .. 30 d, else a fault).
	virtual void set_timer(size_t timer, int64_t ms) = 0;
	virtual void cancel_timer(size_t timer) = 0;
	virtual bool timer_running(size_t timer) const = 0;
	/// The state record: one double per manifest state field, read and written in place.
	virtual double* state() = 0;
	/// A log line of the block (code `log`).
	virtual void log(const char* text) = 0;
	/// Stops the instance (spec/blocks.md 7); the rest of the call is ignored.
	virtual void fault(const char* reason) = 0;

protected:
	~BlockContext() = default;
};

/// A block type (spec/blocks.md 3.1): its manifest and its calls. The same interface serves a
/// built-in C++ type and, later, a WebAssembly module (its exports hc_start, hc_input, hc_timer,
/// hc_period, hc_parameter with `self` selecting the instance's state record). A type keeps no
/// per-instance data of its own: everything an instance needs between calls is in its state record,
/// which the runtime owns, persists and shows in the diagnostics.
///
/// Calls run on the event loop's thread, inside a cascade; they must take bounded time and must not
/// allocate.
class BlockType {
public:
	virtual const Manifest& manifest() const = 0;
	/// Cross-parameter checks at load (spec/blocks.md B17): nullptr, or the reason the values (one
	/// per manifest parameter) are refused, with `parameter` the index of the parameter to blame.
	virtual const char* check(const double* params, size_t& parameter) const {
		(void)params;
		(void)parameter;
		return nullptr;
	}
	virtual void start(BlockContext& cx, StartReason reason, bool restored) const = 0;
	virtual void input(BlockContext& cx, size_t port) const = 0;
	virtual void timer(BlockContext& cx, size_t timer) const = 0;
	virtual void period(BlockContext& cx) const = 0;
	virtual void parameter(BlockContext& cx, size_t index) const = 0;

protected:
	~BlockType() = default;
};

/// The block types an image may use: the firmware's built-in library, plus what a test (the
/// conformance type of spec/blocks.md 15) or later a module loader adds. Not thread-safe; set up
/// before images load.
class BlockRegistry {
public:
	static constexpr size_t kCapacity = 16;

	/// Empty, or with the built-in types.
	explicit BlockRegistry(bool builtins = true);

	/// The registry the device uses (the built-in types).
	static BlockRegistry& device();

	/// False when full or when a type with this id and major version is there.
	bool add(const BlockType& type);
	bool remove(const BlockType& type);
	/// The type with this id and major version; nullptr if none.
	const BlockType* find(const char* type, uint16_t major) const;
	/// Any version of the type (for the refusal's "this firmware has ...").
	const BlockType* find_any(const char* type) const;
	size_t size() const { return count_; }
	const BlockType& at(size_t index) const { return *types_[index]; }

private:
	const BlockType* types_[kCapacity] = {};
	size_t count_ = 0;
};

} // namespace hc::blocks

#endif // HC_BLOCKS_BLOCK_TYPE_H
