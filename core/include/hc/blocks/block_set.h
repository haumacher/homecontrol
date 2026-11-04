/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_BLOCKS_BLOCK_SET_H
#define HC_BLOCKS_BLOCK_SET_H

#include <cstddef>
#include <cstdint>

#include "hc/blocks/block_type.h"
#include "hc/datapoint.h"
#include "hc/json.h"

namespace hc::blocks {

/// The refusal reasons of spec/blocks.md 11 (those of spec/rules.md 10.1, plus unknownType and
/// missingInput).
enum class BlockError : uint8_t {
	none,
	malformed,
	duplicate_id,
	unknown_type,
	unknown_reference,
	missing_input,
	type_error,
	invalid_parameter,
	not_writable,
	output_conflict,
	forbidden,
	limit_exceeded,
	out_of_memory,
};

/// "malformed", "duplicateId", "unknownType", ...
const char* block_error_name(BlockError error);

struct BlockLoadResult {
	BlockError error = BlockError::none;
	char block[BlockLimits::kIdLength + 1] = {}; ///< The block's id ("" when unknown).
	char path[112] = {};                         ///< "blocks[2].inputs.temperature".
	char text[176] = {};
	bool strict = false; ///< An unknown member: the image loader adds the hint of spec/runtime-image.md I4.
	bool ok() const { return error == BlockError::none; }
};

/// What the loader needs to know about the datapoints beyond the model (spec/blocks.md B14).
class BlockDirectory {
public:
	/// Non-null when no block output may bind `datapoint` because another component owns it: the
	/// owner for the refusal ("a schedule entry's output").
	virtual const char* owner(const char* datapoint) const = 0;

protected:
	~BlockDirectory() = default;
};

/// The binding of one port of an instance.
struct PortBinding {
	uint16_t* datapoints;    ///< Ids in the model, in the spec's pool of bound ids; an output uses [0].
	uint8_t count;           ///< 0: unbound.
	Aggregation aggregation; ///< Inputs bound to more than one datapoint.
};

/// One compiled instance (spec/blocks.md 4).
struct BlockSpec {
	char id[BlockLimits::kIdLength + 1];
	const BlockType* type;
	uint16_t minor;          ///< The image's minor version.
	char* label;             ///< The label's JSON text, as the image gives it.
	bool enabled;
	bool enabled_adjustable;
	PortBinding* ports;      ///< One per manifest port.
	uint16_t* bound;         ///< The pool of the ports' datapoint ids: two bytes per bound datapoint (B20).
	uint16_t bound_size;     ///< Its capacity.
	double* defaults;        ///< One per manifest parameter: the image's value or the manifest's default.
};

/// The `blocks` member of an image, compiled against a datapoint model: every reference resolved to
/// a datapoint id, every value checked (spec/blocks.md 11). Movable, not copyable. Allocates at
/// load only.
class BlockSet {
public:
	BlockSet() = default;
	~BlockSet() { clear(); }
	BlockSet(const BlockSet&) = delete;
	BlockSet& operator=(const BlockSet&) = delete;
	BlockSet(BlockSet&& other) noexcept;
	BlockSet& operator=(BlockSet&& other) noexcept;

	/// Compiles `blocks` (an array) against `model`; `directory` says which datapoints another
	/// component owns. On a refusal the set stays empty and the result names the first problem.
	BlockLoadResult load(const JsonSpan& blocks, const DatapointModel& model, const BlockDirectory& directory,
		const BlockRegistry& registry);
	/// The same against a directory of the datapoints (the image loader's tables: no scratch model).
	BlockLoadResult load(const JsonSpan& blocks, const DatapointDirectory& datapoints, const BlockDirectory& directory,
		const BlockRegistry& registry);

	size_t size() const { return count_; }
	const BlockSpec& at(size_t index) const { return specs_[index]; }
	/// The index of the block `id`; -1 if none.
	int find(const char* id) const;
	/// Heap bytes the set holds.
	size_t bytes() const { return bytes_; }
	void clear();

private:
	BlockSpec* specs_ = nullptr;
	size_t count_ = 0;
	size_t bytes_ = 0;
};

/// The datapoint value type of a port's mirror and of a parameter's (spec/blocks.md 6).
ValueType port_datapoint_type(PortType type);
ValueType param_datapoint_type(ParamType type);

/// `block/<block>/<member>`; false if it does not fit.
bool mirror_name(const char* block, const char* member, char* out, size_t size);

/// A datapoint the image implies for a block (spec/blocks.md 6), as the loader and the tests derive
/// it from the `blocks` member.
struct ImpliedDatapoint {
	char name[Datapoint::kMaxName + 1];
	ValueType type;
	uint8_t flags;
};

/// The implied datapoints of `blocks`, in block order (`enabled`, the adjustable parameters, the
/// outputs, the aggregated inputs), from the `first` on into `out` up to `capacity` (a caller fills
/// its tables in batches); returns how many there are.
/// Lenient: an instance that is not well-formed (no id, an unknown type) is skipped, since load()
/// refuses it with the precise reason.
size_t implied_datapoints(const JsonSpan& blocks, const BlockRegistry& registry, ImpliedDatapoint* out,
	size_t capacity, size_t first = 0);

/// The value of a parameter in its image form (spec/blocks.md 4): a number, a boolean, a choice's
/// id or a time of day object. False (with `reason`) when it is not of the parameter's type; the
/// bounds are checked by param_valid().
bool param_from_json(const ParamDecl& decl, const JsonSpan& json, double& out, const char** reason);
/// Whether `value` is a value of the parameter's type (finite; integral for every type but real; 0 or
/// 1 for a boolean; within the range of the type): a typeError at load otherwise.
bool param_type_ok(const ParamDecl& decl, double value, const char** reason);
/// Whether `value` is valid for the parameter: of its type, within its bounds, one of its choices.
bool param_valid(const ParamDecl& decl, double value, const char** reason);

} // namespace hc::blocks

#endif // HC_BLOCKS_BLOCK_SET_H
