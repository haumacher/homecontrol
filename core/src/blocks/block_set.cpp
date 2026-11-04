/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/blocks/block_set.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "hc/rules/parameter_store.h"
#include "hc/log.h"
#include "hc/value_store.h"

namespace hc::blocks {

const char* block_error_name(BlockError error) {
	switch (error) {
		case BlockError::none: return "ok";
		case BlockError::malformed: return "malformed";
		case BlockError::duplicate_id: return "duplicateId";
		case BlockError::unknown_type: return "unknownType";
		case BlockError::unknown_reference: return "unknownReference";
		case BlockError::missing_input: return "missingInput";
		case BlockError::type_error: return "typeError";
		case BlockError::invalid_parameter: return "invalidParameter";
		case BlockError::not_writable: return "notWritable";
		case BlockError::output_conflict: return "outputConflict";
		case BlockError::forbidden: return "forbidden";
		case BlockError::limit_exceeded: return "limitExceeded";
		case BlockError::out_of_memory: return "outOfMemory";
	}
	return "?";
}

ValueType port_datapoint_type(PortType type) {
	switch (type) {
		case PortType::boolean: return ValueType::boolean;
		case PortType::integer: return ValueType::signed_int;
		case PortType::real: return ValueType::real;
		case PortType::enumeration: return ValueType::enumeration;
	}
	return ValueType::none;
}

ValueType param_datapoint_type(ParamType type) {
	switch (type) {
		case ParamType::boolean: return ValueType::boolean;
		case ParamType::integer: return ValueType::signed_int;
		case ParamType::real: return ValueType::real;
		case ParamType::duration: return ValueType::unsigned_int;
		case ParamType::time_of_day: return ValueType::time_of_day;
		case ParamType::enumeration: return ValueType::enumeration;
	}
	return ValueType::none;
}

bool mirror_name(const char* block, const char* member, char* out, size_t size) {
	int n = std::snprintf(out, size, "block/%s/%s", block, member);
	return n > 0 && static_cast<size_t>(n) < size && static_cast<size_t>(n) <= Datapoint::kMaxName;
}

namespace {

bool block_id_valid(const char* id) {
	size_t n = std::strlen(id);
	if (n == 0 || n > BlockLimits::kIdLength) {
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		char c = id[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
			return false;
		}
	}
	return true;
}

/// "1.0" -> 1, 0.
bool parse_version(const char* text, uint32_t& major, uint32_t& minor) {
	const char* p = text;
	auto number = [&p](uint32_t& out) {
		if (*p < '0' || *p > '9') {
			return false;
		}
		uint32_t value = 0;
		while (*p >= '0' && *p <= '9') {
			value = value * 10 + static_cast<uint32_t>(*p - '0');
			if (value > 9999) {
				return false;
			}
			++p;
		}
		out = value;
		return true;
	};
	if (!number(major) || *p != '.') {
		return false;
	}
	++p;
	return number(minor) && *p == '\0';
}

bool printable(const char* text, size_t length) {
	for (size_t i = 0; i < length; ++i) {
		if (static_cast<unsigned char>(text[i]) < 0x20 || text[i] == 0x7F) {
			return false;
		}
	}
	return true;
}

/// A label of spec/rules.md 2.2.
bool label_valid(const JsonSpan& span) {
	char buffer[80];
	size_t length = 0;
	if (span.kind == JsonSpan::Kind::string) {
		return json_string(span, buffer, sizeof(buffer), &length) && length >= 1 && length <= 63 &&
			printable(buffer, length);
	}
	if (span.kind != JsonSpan::Kind::object) {
		return false;
	}
	size_t position = 0;
	JsonSpan key;
	JsonSpan value;
	size_t entries = 0;
	while (json_next_member(span, position, key, value)) {
		char tag[16];
		size_t tag_length = 0;
		if (!json_string(key, tag, sizeof(tag), &tag_length) || tag_length < 2 || tag_length > 8) {
			return false;
		}
		for (size_t i = 0; i < tag_length; ++i) {
			char c = tag[i];
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
				return false;
			}
		}
		if (value.kind != JsonSpan::Kind::string || !json_string(value, buffer, sizeof(buffer), &length) ||
			length < 1 || length > 63 || !printable(buffer, length)) {
			return false;
		}
		++entries;
	}
	return entries >= 1 && entries <= 16;
}

/// Whether an input port of `type` reads a datapoint of `dp` (spec/blocks.md 5.3).
bool input_accepts(PortType type, ValueType dp) {
	switch (type) {
		case PortType::real: return dp == ValueType::real || dp == ValueType::unsigned_int || dp == ValueType::signed_int;
		case PortType::integer: return dp == ValueType::unsigned_int || dp == ValueType::signed_int;
		case PortType::enumeration: return dp == ValueType::enumeration || dp == ValueType::unsigned_int;
		case PortType::boolean: return dp == ValueType::boolean;
	}
	return false;
}

/// Whether an output port of `type` commands a datapoint of `dp` (spec/blocks.md 5.4).
bool output_accepts(PortType type, ValueType dp) {
	switch (type) {
		case PortType::real: return dp == ValueType::real || dp == ValueType::unsigned_int || dp == ValueType::signed_int;
		case PortType::integer: return dp == ValueType::unsigned_int || dp == ValueType::signed_int || dp == ValueType::real;
		case PortType::enumeration: return dp == ValueType::enumeration || dp == ValueType::unsigned_int;
		case PortType::boolean: return dp == ValueType::boolean;
	}
	return false;
}

/// The members of an instance (spec/blocks.md 4).
const char* const kMembers[] = {"id", "type", "version", "label", "enabled", "enabledAdjustable", "inputs",
	"aggregation", "outputs", "parameters", "description", "comment", nullptr};

struct Compiler {
	const DatapointDirectory& model;
	const BlockDirectory& directory;
	const BlockRegistry& registry;
	BlockLoadResult& result;
	char path[sizeof(BlockLoadResult::path)] = {};
	size_t next_bound = 0; ///< The next free slot of the compiled spec's pool of bound ids.

	bool refuse(BlockError error, const char* format, ...) HC_PRINTF_FORMAT(3, 4) {
		if (result.error != BlockError::none) {
			return false;
		}
		result.error = error;
		std::snprintf(result.path, sizeof(result.path), "%s", path);
		va_list args;
		va_start(args, format);
		std::vsnprintf(result.text, sizeof(result.text), format, args);
		va_end(args);
		return false;
	}

	void at(const char* format, ...) HC_PRINTF_FORMAT(2, 3) {
		va_list args;
		va_start(args, format);
		std::vsnprintf(path, sizeof(path), format, args);
		va_end(args);
	}

	/// A datapoint name into its id.
	bool datapoint(const JsonSpan& span, uint16_t& id) {
		char name[Datapoint::kMaxName + 2];
		if (span.kind != JsonSpan::Kind::string || !json_string(span, name, sizeof(name))) {
			return refuse(BlockError::malformed, "a datapoint name must be a string of 1..63 bytes");
		}
		DatapointRef dp;
		if (!model.lookup(name, dp)) {
			return refuse(BlockError::unknown_reference, "no datapoint '%s'", name);
		}
		id = dp.id();
		return true;
	}

	bool compile(const JsonSpan& item, size_t index, BlockSpec& spec, const BlockSpec* earlier, size_t earlier_count);
	bool inputs(const JsonSpan& item, size_t index, BlockSpec& spec);
	bool outputs(const JsonSpan& item, size_t index, BlockSpec& spec, const BlockSpec* earlier, size_t earlier_count);
	bool parameters(const JsonSpan& item, size_t index, BlockSpec& spec);
};

bool Compiler::compile(const JsonSpan& item, size_t index, BlockSpec& spec, const BlockSpec* earlier,
	size_t earlier_count) {
	at("blocks[%u]", static_cast<unsigned>(index));
	if (item.kind != JsonSpan::Kind::object) {
		return refuse(BlockError::malformed, "a block must be an object");
	}
	size_t position = 0;
	JsonSpan key;
	JsonSpan value;
	while (json_next_member(item, position, key, value)) {
		bool known = false;
		for (const char* const* m = kMembers; *m != nullptr && !known; ++m) {
			known = json_string_equals(key, *m);
		}
		if (!known) {
			char name[40];
			if (!json_string(key, name, sizeof(name))) {
				std::snprintf(name, sizeof(name), "?");
			}
			result.strict = true;
			return refuse(BlockError::malformed, "unknown member '%s'", name);
		}
	}
	JsonSpan member;
	at("blocks[%u].id", static_cast<unsigned>(index));
	if (!json_member(item, "id", member)) {
		at("blocks[%u]", static_cast<unsigned>(index));
		return refuse(BlockError::malformed, "the member 'id' is missing");
	}
	if (member.kind != JsonSpan::Kind::string || !json_string(member, spec.id, sizeof(spec.id)) ||
		!block_id_valid(spec.id)) {
		spec.id[0] = '\0';
		return refuse(BlockError::malformed, "a block id has 1..32 characters of A-Z a-z 0-9 _ -");
	}
	std::snprintf(result.block, sizeof(result.block), "%s", spec.id);
	for (size_t k = 0; k < earlier_count; ++k) {
		if (std::strcmp(earlier[k].id, spec.id) == 0) {
			return refuse(BlockError::duplicate_id, "blocks[%u] has the id '%s' too", static_cast<unsigned>(k), spec.id);
		}
	}
	char type[72];
	at("blocks[%u].type", static_cast<unsigned>(index));
	if (!json_member(item, "type", member) || member.kind != JsonSpan::Kind::string ||
		!json_string(member, type, sizeof(type))) {
		return refuse(BlockError::malformed, "the member 'type' (a type id) is missing or not a string");
	}
	char version[16];
	uint32_t major = 0, minor = 0;
	at("blocks[%u].version", static_cast<unsigned>(index));
	if (!json_member(item, "version", member) || member.kind != JsonSpan::Kind::string ||
		!json_string(member, version, sizeof(version)) || !parse_version(version, major, minor)) {
		return refuse(BlockError::malformed, "the member 'version' must be \"<major>.<minor>\"");
	}
	at("blocks[%u].type", static_cast<unsigned>(index));
	spec.type = registry.find(type, static_cast<uint16_t>(major));
	if (spec.type == nullptr || spec.type->manifest().minor < minor) {
		const BlockType* any = spec.type != nullptr ? spec.type : registry.find_any(type);
		if (any == nullptr) {
			return refuse(BlockError::unknown_type, "%s %s (this firmware has no such block type)", type, version);
		}
		return refuse(BlockError::unknown_type, "%s %s (this firmware has %u.%u)", type, version,
			static_cast<unsigned>(any->manifest().major), static_cast<unsigned>(any->manifest().minor));
	}
	spec.minor = static_cast<uint16_t>(minor);
	at("blocks[%u].label", static_cast<unsigned>(index));
	if (!json_member(item, "label", member)) {
		at("blocks[%u]", static_cast<unsigned>(index));
		return refuse(BlockError::malformed, "the member 'label' is missing");
	}
	if (!label_valid(member)) {
		return refuse(BlockError::malformed,
			"the label must be a string of 1..63 bytes or an object of language tags to such strings");
	}
	spec.label = new (std::nothrow) char[member.length + 1];
	if (spec.label == nullptr) {
		return refuse(BlockError::out_of_memory, "no memory for the label");
	}
	std::memcpy(spec.label, member.data, member.length);
	spec.label[member.length] = '\0';
	spec.enabled = true;
	spec.enabled_adjustable = true;
	for (const char* flag : {"enabled", "enabledAdjustable"}) {
		if (json_member(item, flag, member)) {
			at("blocks[%u].%s", static_cast<unsigned>(index), flag);
			bool b = false;
			if (member.kind != JsonSpan::Kind::boolean || !json_bool(member, b)) {
				return refuse(BlockError::malformed, "'%s' must be true or false", flag);
			}
			(flag[7] == '\0' ? spec.enabled : spec.enabled_adjustable) = b;
		}
	}
	const Manifest& m = spec.type->manifest();
	// The pool of bound ids (spec/blocks.md B20): one slot per output, and as many per input as the
	// image lists for it (a list longer than the port's multiple is refused below).
	size_t output_count = m.port_count - m.inputs();
	size_t listed = 0;
	JsonSpan inputs_member;
	if (json_member(item, "inputs", inputs_member) && inputs_member.kind == JsonSpan::Kind::object) {
		size_t position = 0;
		JsonSpan key;
		JsonSpan value;
		while (json_next_member(inputs_member, position, key, value)) {
			if (value.kind != JsonSpan::Kind::array) {
				++listed;
				continue;
			}
			size_t at_item = 0;
			JsonSpan dp;
			while (json_next_item(value, at_item, dp)) {
				++listed;
			}
		}
	}
	if (output_count + listed > 0xffff) {
		at("blocks[%u].inputs", static_cast<unsigned>(index));
		return refuse(BlockError::limit_exceeded, "too many bound datapoints");
	}
	spec.bound_size = static_cast<uint16_t>(output_count + listed);
	spec.ports = new (std::nothrow) PortBinding[m.port_count > 0 ? m.port_count : 1];
	spec.defaults = new (std::nothrow) double[m.param_count > 0 ? m.param_count : 1];
	spec.bound = new (std::nothrow) uint16_t[spec.bound_size > 0 ? spec.bound_size : 1];
	if (spec.ports == nullptr || spec.defaults == nullptr || spec.bound == nullptr) {
		return refuse(BlockError::out_of_memory, "no memory for the block's tables");
	}
	size_t slot = listed;
	for (size_t p = 0; p < m.port_count; ++p) {
		uint16_t* own = nullptr;
		if (m.ports[p].direction == Direction::out) {
			own = spec.bound + slot++;
			*own = 0;
		}
		spec.ports[p] = PortBinding{own, 0, m.ports[p].default_aggregation};
	}
	next_bound = 0;
	for (size_t p = 0; p < m.param_count; ++p) {
		spec.defaults[p] = m.params[p].default_value;
	}
	return inputs(item, index, spec) && outputs(item, index, spec, earlier, earlier_count) &&
		parameters(item, index, spec);
}

bool Compiler::inputs(const JsonSpan& item, size_t index, BlockSpec& spec) {
	const Manifest& m = spec.type->manifest();
	JsonSpan list;
	if (json_member(item, "inputs", list)) {
		at("blocks[%u].inputs", static_cast<unsigned>(index));
		if (list.kind != JsonSpan::Kind::object) {
			return refuse(BlockError::malformed, "'inputs' must be an object of port ids to datapoint names");
		}
		size_t position = 0;
		JsonSpan key;
		JsonSpan value;
		while (json_next_member(list, position, key, value)) {
			char port_id[32];
			if (!json_string(key, port_id, sizeof(port_id))) {
				return refuse(BlockError::malformed, "a port id is too long");
			}
			at("blocks[%u].inputs.%s", static_cast<unsigned>(index), port_id);
			int p = m.port(port_id);
			if (p < 0) {
				return refuse(BlockError::unknown_reference, "%s has no port '%s'", m.type, port_id);
			}
			const PortDecl& decl = m.ports[p];
			if (decl.direction != Direction::in) {
				return refuse(BlockError::forbidden, "'%s' is an output of %s: bind it under 'outputs'", port_id, m.type);
			}
			PortBinding& b = spec.ports[p];
			const bool list = value.kind == JsonSpan::Kind::array;
			size_t count = 1;
			if (list) {
				count = 0;
				size_t at_item = 0;
				JsonSpan dp;
				while (json_next_item(value, at_item, dp)) {
					if (count == decl.multiple) {
						return refuse(BlockError::forbidden, "the port '%s' takes at most %u datapoint%s", port_id,
							static_cast<unsigned>(decl.multiple), decl.multiple == 1 ? "" : "s");
					}
					++count;
				}
				if (count == 0) {
					return refuse(BlockError::malformed, "an input binds 1..%u datapoints",
						static_cast<unsigned>(decl.multiple));
				}
			}
			if (next_bound + count > spec.bound_size) {
				return refuse(BlockError::malformed, "the inputs bind more datapoints than they list");
			}
			b.datapoints = spec.bound + next_bound;
			next_bound += count;
			size_t at_item = 0;
			for (size_t k = 0; k < count; ++k) {
				JsonSpan name = value;
				if (list) {
					json_next_item(value, at_item, name);
					at("blocks[%u].inputs.%s[%u]", static_cast<unsigned>(index), port_id, static_cast<unsigned>(k));
				}
				uint16_t id = 0;
				if (!datapoint(name, id)) {
					return false;
				}
				DatapointRef found;
				model.lookup(id, found); // resolved by name just before
				const DatapointRef* dp = &found;
				char own[Datapoint::kMaxName + 2];
				std::snprintf(own, sizeof(own), "block/%s/", spec.id);
				if (std::strncmp(dp->name(), own, std::strlen(own)) == 0) {
					return refuse(BlockError::forbidden, "'%s' is a datapoint of this block: it cannot be its input",
						dp->name());
				}
				if (!input_accepts(decl.type, dp->type())) {
					return refuse(BlockError::type_error, "the %s port '%s' cannot read the %s datapoint '%s'",
						port_type_name(decl.type), port_id, value_type_name(dp->type()), dp->name());
				}
				for (size_t e = 0; e < k; ++e) {
					if (b.datapoints[e] == id) {
						return refuse(BlockError::malformed, "'%s' is listed twice", dp->name());
					}
				}
				b.datapoints[k] = id;
			}
			b.count = static_cast<uint8_t>(count);
		}
	}
	for (size_t p = 0; p < m.port_count; ++p) {
		if (m.ports[p].direction == Direction::in && m.ports[p].required && spec.ports[p].count == 0) {
			at("blocks[%u].inputs.%s", static_cast<unsigned>(index), m.ports[p].id);
			return refuse(BlockError::missing_input, "the required input '%s' of %s is not bound", m.ports[p].id, m.type);
		}
	}
	JsonSpan aggregation;
	if (json_member(item, "aggregation", aggregation)) {
		at("blocks[%u].aggregation", static_cast<unsigned>(index));
		if (aggregation.kind != JsonSpan::Kind::object) {
			return refuse(BlockError::malformed, "'aggregation' must be an object of port ids to aggregations");
		}
		size_t position = 0;
		JsonSpan key;
		JsonSpan value;
		while (json_next_member(aggregation, position, key, value)) {
			char port_id[32];
			if (!json_string(key, port_id, sizeof(port_id))) {
				return refuse(BlockError::malformed, "a port id is too long");
			}
			at("blocks[%u].aggregation.%s", static_cast<unsigned>(index), port_id);
			int p = m.port(port_id);
			if (p < 0 || m.ports[p].direction != Direction::in) {
				return refuse(BlockError::unknown_reference, "%s has no input '%s'", m.type, port_id);
			}
			char name[16];
			Aggregation a = Aggregation::none;
			if (value.kind == JsonSpan::Kind::string && json_string(value, name, sizeof(name))) {
				a = aggregation_of(name);
			}
			if (a == Aggregation::none) {
				return refuse(BlockError::malformed, "an aggregation is one of mean, min, max, any, all");
			}
			if (spec.ports[p].count < 2) {
				return refuse(BlockError::forbidden, "the input '%s' does not bind several datapoints: nothing to aggregate",
					port_id);
			}
			if ((m.ports[p].aggregations & aggregation_bit(a)) == 0) {
				return refuse(BlockError::forbidden, "the input '%s' does not allow the aggregation '%s'", port_id,
					aggregation_name(a));
			}
			spec.ports[p].aggregation = a;
		}
	}
	return true;
}

bool Compiler::outputs(const JsonSpan& item, size_t index, BlockSpec& spec, const BlockSpec* earlier,
	size_t earlier_count) {
	const Manifest& m = spec.type->manifest();
	JsonSpan list;
	if (!json_member(item, "outputs", list)) {
		return true;
	}
	at("blocks[%u].outputs", static_cast<unsigned>(index));
	if (list.kind != JsonSpan::Kind::object) {
		return refuse(BlockError::malformed, "'outputs' must be an object of port ids to datapoint names");
	}
	size_t position = 0;
	JsonSpan key;
	JsonSpan value;
	while (json_next_member(list, position, key, value)) {
		char port_id[32];
		if (!json_string(key, port_id, sizeof(port_id))) {
			return refuse(BlockError::malformed, "a port id is too long");
		}
		at("blocks[%u].outputs.%s", static_cast<unsigned>(index), port_id);
		int p = m.port(port_id);
		if (p < 0) {
			return refuse(BlockError::unknown_reference, "%s has no port '%s'", m.type, port_id);
		}
		const PortDecl& decl = m.ports[p];
		if (decl.direction != Direction::out) {
			return refuse(BlockError::forbidden, "'%s' is an input of %s: bind it under 'inputs'", port_id, m.type);
		}
		uint16_t id = 0;
		if (!datapoint(value, id)) {
			return false;
		}
		DatapointRef found;
				model.lookup(id, found); // resolved by name just before
				const DatapointRef* dp = &found;
		const char* owner = directory.owner(dp->name());
		if (owner != nullptr) {
			return refuse(BlockError::output_conflict, "'%s' is owned by %s", dp->name(), owner);
		}
		if (!dp->writable()) {
			return refuse(BlockError::not_writable, "'%s' is not writable", dp->name());
		}
		if (!output_accepts(decl.type, dp->type())) {
			return refuse(BlockError::type_error, "the %s port '%s' cannot command the %s datapoint '%s'",
				port_type_name(decl.type), port_id, value_type_name(dp->type()), dp->name());
		}
		auto taken = [id](const BlockSpec& s, size_t limit) {
			for (size_t q = 0; q < limit; ++q) {
				if (s.type->manifest().ports[q].direction == Direction::out && s.ports[q].count > 0 &&
					s.ports[q].datapoints[0] == id) {
					return static_cast<int>(q);
				}
			}
			return -1;
		};
		int q = taken(spec, m.port_count);
		if (q >= 0) {
			return refuse(BlockError::output_conflict, "'%s' is bound to the output '%s' already", dp->name(),
				m.ports[q].id);
		}
		for (size_t k = 0; k < earlier_count; ++k) {
			q = taken(earlier[k], earlier[k].type->manifest().port_count);
			if (q >= 0) {
				return refuse(BlockError::output_conflict, "'%s' is the output '%s' of the block '%s'", dp->name(),
					earlier[k].type->manifest().ports[q].id, earlier[k].id);
			}
		}
		spec.ports[p].datapoints[0] = id;
		spec.ports[p].count = 1;
	}
	return true;
}

bool Compiler::parameters(const JsonSpan& item, size_t index, BlockSpec& spec) {
	const Manifest& m = spec.type->manifest();
	JsonSpan list;
	if (json_member(item, "parameters", list)) {
		at("blocks[%u].parameters", static_cast<unsigned>(index));
		if (list.kind != JsonSpan::Kind::object) {
			return refuse(BlockError::malformed, "'parameters' must be an object of parameter ids to values");
		}
		size_t position = 0;
		JsonSpan key;
		JsonSpan value;
		while (json_next_member(list, position, key, value)) {
			char id[32];
			if (!json_string(key, id, sizeof(id))) {
				return refuse(BlockError::malformed, "a parameter id is too long");
			}
			at("blocks[%u].parameters.%s", static_cast<unsigned>(index), id);
			int p = m.param(id);
			if (p < 0) {
				return refuse(BlockError::unknown_reference, "%s has no parameter '%s'", m.type, id);
			}
			const char* reason = nullptr;
			double v = 0;
			if (!param_from_json(m.params[p], value, v, &reason) || !param_type_ok(m.params[p], v, &reason)) {
				return refuse(BlockError::type_error, "%s: %s", id, reason);
			}
			if (!param_valid(m.params[p], v, &reason)) {
				return refuse(BlockError::invalid_parameter, "%s: %s", id, reason);
			}
			spec.defaults[p] = v;
		}
	}
	size_t blamed = 0;
	const char* reason = spec.type->check(spec.defaults, blamed);
	if (reason != nullptr) {
		if (blamed < m.param_count) {
			at("blocks[%u].parameters.%s", static_cast<unsigned>(index), m.params[blamed].id);
		} else {
			at("blocks[%u].parameters", static_cast<unsigned>(index));
		}
		return refuse(BlockError::invalid_parameter, "%s", reason);
	}
	return true;
}

void free_spec(BlockSpec& spec) {
	delete[] spec.label;
	delete[] spec.ports;
	delete[] spec.defaults;
	delete[] spec.bound;
	spec.label = nullptr;
	spec.ports = nullptr;
	spec.defaults = nullptr;
	spec.bound = nullptr;
}

} // namespace

bool param_from_json(const ParamDecl& decl, const JsonSpan& json, double& out, const char** reason) {
	switch (decl.type) {
		case ParamType::boolean: {
			bool b = false;
			if (json.kind != JsonSpan::Kind::boolean || !json_bool(json, b)) {
				*reason = "not true or false";
				return false;
			}
			out = b ? 1 : 0;
			return true;
		}
		case ParamType::time_of_day: {
			JsonSpan h, mi, se;
			double hour = 0, minute = 0, second = 0;
			if (json.kind != JsonSpan::Kind::object || !json_member(json, "hour", h) || !json_member(json, "minute", mi) ||
				!json_member(json, "second", se) || !json_number(h, hour) || !json_number(mi, minute) ||
				!json_number(se, second) || hour != std::floor(hour) || minute != std::floor(minute) ||
				second != std::floor(second) || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
				second > 59) {
				*reason = "not a time of day {\"hour\", \"minute\", \"second\"}";
				return false;
			}
			out = hour * 3600 + minute * 60 + second;
			return true;
		}
		default: break;
	}
	if (json.kind == JsonSpan::Kind::string && decl.choice_count > 0) {
		char id[32];
		if (json_string(json, id, sizeof(id))) {
			for (size_t c = 0; c < decl.choice_count; ++c) {
				if (std::strcmp(decl.choices[c].id, id) == 0) {
					out = decl.choices[c].value;
					return true;
				}
			}
		}
		*reason = "not one of the choices";
		return false;
	}
	if (json.kind != JsonSpan::Kind::number || !json_number(json, out) || !std::isfinite(out)) {
		*reason = decl.choice_count > 0 ? "not a number or a choice's id" : "not a number";
		return false;
	}
	return true;
}

bool param_valid(const ParamDecl& decl, double v, const char** reason) {
	if (!param_type_ok(decl, v, reason)) {
		return false;
	}
	if ((decl.has_min && v < decl.min) || (decl.has_max && v > decl.max)) {
		*reason = "outside min..max";
		return false;
	}
	if (decl.choice_count > 0) {
		for (size_t c = 0; c < decl.choice_count; ++c) {
			if (decl.choices[c].value == v) {
				return true;
			}
		}
		*reason = "not one of the choices";
		return false;
	}
	return true;
}

bool param_type_ok(const ParamDecl& decl, double v, const char** reason) {
	if (!std::isfinite(v)) {
		*reason = "not a finite number";
		return false;
	}
	bool integral = decl.type == ParamType::boolean || decl.type == ParamType::integer ||
		decl.type == ParamType::duration || decl.type == ParamType::time_of_day || decl.type == ParamType::enumeration;
	if (integral && v != std::floor(v)) {
		*reason = "not a whole number";
		return false;
	}
	if (decl.type == ParamType::boolean && v != 0 && v != 1) {
		*reason = "not true or false";
		return false;
	}
	if ((decl.type == ParamType::duration || decl.type == ParamType::enumeration) && (v < 0 || v > 4294967295.0)) {
		*reason = "outside 0..4294967295";
		return false;
	}
	if (decl.type == ParamType::integer && (v < -2147483648.0 || v > 2147483647.0)) {
		*reason = "outside the signed 32-bit range";
		return false;
	}
	if (decl.type == ParamType::time_of_day && (v < 0 || v > 86399)) {
		*reason = "not a time of day";
		return false;
	}
	return true;
}

BlockSet::BlockSet(BlockSet&& other) noexcept
	: specs_(other.specs_), count_(other.count_), bytes_(other.bytes_) {
	other.specs_ = nullptr;
	other.count_ = 0;
	other.bytes_ = 0;
}

BlockSet& BlockSet::operator=(BlockSet&& other) noexcept {
	if (this != &other) {
		clear();
		specs_ = other.specs_;
		count_ = other.count_;
		bytes_ = other.bytes_;
		other.specs_ = nullptr;
		other.count_ = 0;
		other.bytes_ = 0;
	}
	return *this;
}

void BlockSet::clear() {
	for (size_t i = 0; i < count_; ++i) {
		free_spec(specs_[i]);
	}
	delete[] specs_;
	specs_ = nullptr;
	count_ = 0;
	bytes_ = 0;
}

int BlockSet::find(const char* id) const {
	for (size_t i = 0; i < count_; ++i) {
		if (std::strcmp(specs_[i].id, id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

BlockLoadResult BlockSet::load(const JsonSpan& blocks, const DatapointModel& model, const BlockDirectory& directory,
	const BlockRegistry& registry) {
	ModelDirectory datapoints(model);
	return load(blocks, datapoints, directory, registry);
}

BlockLoadResult BlockSet::load(const JsonSpan& blocks, const DatapointDirectory& model, const BlockDirectory& directory,
	const BlockRegistry& registry) {
	clear();
	BlockLoadResult result;
	Compiler c{model, directory, registry, result};
	c.at("blocks");
	if (blocks.kind != JsonSpan::Kind::array) {
		c.refuse(BlockError::malformed, "'blocks' must be an array");
		return result;
	}
	size_t count = 0;
	size_t position = 0;
	JsonSpan item;
	while (json_next_item(blocks, position, item)) {
		++count;
	}
	if (count > BlockLimits::kBlocks) {
		c.refuse(BlockError::limit_exceeded, "%u blocks exceed the limit of %u", static_cast<unsigned>(count),
			static_cast<unsigned>(BlockLimits::kBlocks));
		return result;
	}
	if (count == 0) {
		return result;
	}
	BlockSpec* specs = new (std::nothrow) BlockSpec[count];
	if (specs == nullptr) {
		c.refuse(BlockError::out_of_memory, "no memory for %u blocks", static_cast<unsigned>(count));
		return result;
	}
	std::memset(static_cast<void*>(specs), 0, sizeof(BlockSpec) * count);
	position = 0;
	size_t done = 0;
	bool ok = true;
	while (ok && json_next_item(blocks, position, item)) {
		ok = c.compile(item, done, specs[done], specs, done);
		++done;
	}
	// The persistence keys of the device must not collide (spec/blocks.md 11): the record blobs and
	// the override keys of the store of the rule parameters.
	for (size_t i = 0; ok && i < count; ++i) {
		for (size_t k = 0; ok && k < i; ++k) {
			if (persist::fnv1a(specs[i].id) == persist::fnv1a(specs[k].id)) {
				c.at("blocks[%u].id", static_cast<unsigned>(i));
				ok = c.refuse(BlockError::limit_exceeded, "the state records of '%s' and '%s' collide in the device's storage",
					specs[i].id, specs[k].id);
			}
		}
	}
	if (ok) {
		size_t keys = 0;
		for (size_t i = 0; i < count; ++i) {
			keys += specs[i].type->manifest().param_count + 1u;
		}
		uint64_t* hashes = new (std::nothrow) uint64_t[keys];
		if (hashes == nullptr) {
			ok = c.refuse(BlockError::out_of_memory, "no memory to check the stored overrides' keys");
		} else {
			size_t n = 0;
			for (size_t i = 0; i < count; ++i) {
				const Manifest& m = specs[i].type->manifest();
				char key[48];
				std::snprintf(key, sizeof(key), "@%s", specs[i].id);
				for (size_t p = 0; p <= m.param_count; ++p) {
					hashes[n++] = rules::ParameterStore::hash(key, p == m.param_count ? "enabled" : m.params[p].id);
				}
			}
			std::sort(hashes, hashes + n);
			for (size_t i = 1; ok && i < n; ++i) {
				if (hashes[i] == hashes[i - 1]) {
					c.at("blocks");
					ok = c.refuse(BlockError::limit_exceeded, "two stored parameter overrides of the blocks collide in the "
						"device's storage");
				}
			}
			delete[] hashes;
		}
	}
	if (!ok) {
		for (size_t i = 0; i < done; ++i) {
			free_spec(specs[i]);
		}
		delete[] specs;
		return result;
	}
	specs_ = specs;
	count_ = count;
	bytes_ = sizeof(BlockSpec) * count;
	for (size_t i = 0; i < count; ++i) {
		const Manifest& m = specs[i].type->manifest();
		bytes_ += std::strlen(specs[i].label) + 1 + sizeof(PortBinding) * m.port_count + sizeof(double) * m.param_count +
			sizeof(uint16_t) * specs[i].bound_size;
	}
	return result;
}

size_t implied_datapoints(const JsonSpan& blocks, const BlockRegistry& registry, ImpliedDatapoint* out,
	size_t capacity, size_t first) {
	if (blocks.kind != JsonSpan::Kind::array) {
		return 0;
	}
	size_t count = 0;
	auto add = [&](const char* id, const char* member, ValueType type, uint8_t flags) {
		if (out != nullptr && count >= first && count - first < capacity) {
			ImpliedDatapoint& d = out[count - first];
			if (!mirror_name(id, member, d.name, sizeof(d.name))) {
				return;
			}
			d.type = type;
			d.flags = flags;
		}
		++count;
	};
	size_t position = 0;
	JsonSpan item;
	while (json_next_item(blocks, position, item)) {
		JsonSpan member;
		char id[BlockLimits::kIdLength + 2];
		char type[72];
		char version[16];
		uint32_t major = 0, minor = 0;
		if (item.kind != JsonSpan::Kind::object || !json_member(item, "id", member) ||
			!json_string(member, id, sizeof(id)) || !block_id_valid(id) || !json_member(item, "type", member) ||
			!json_string(member, type, sizeof(type)) || !json_member(item, "version", member) ||
			!json_string(member, version, sizeof(version)) || !parse_version(version, major, minor)) {
			continue;
		}
		const BlockType* t = registry.find(type, static_cast<uint16_t>(major));
		if (t == nullptr) {
			continue;
		}
		const Manifest& m = t->manifest();
		bool adjustable = true;
		bool b = false;
		if (json_member(item, "enabledAdjustable", member) && json_bool(member, b)) {
			adjustable = b;
		}
		uint8_t commandable = DatapointFlags::kWritable | DatapointFlags::kTransmit;
		add(id, "enabled", ValueType::boolean, adjustable ? commandable : 0);
		for (size_t p = 0; p < m.param_count; ++p) {
			if (m.params[p].adjustable) {
				add(id, m.params[p].id, param_datapoint_type(m.params[p].type), commandable);
			}
		}
		for (size_t p = 0; p < m.port_count; ++p) {
			if (m.ports[p].direction == Direction::out) {
				add(id, m.ports[p].id, port_datapoint_type(m.ports[p].type), 0);
			}
		}
		JsonSpan inputs;
		if (json_member(item, "inputs", inputs) && inputs.kind == JsonSpan::Kind::object) {
			for (size_t p = 0; p < m.port_count; ++p) {
				JsonSpan bound;
				if (m.ports[p].direction != Direction::in || !json_member(inputs, m.ports[p].id, bound) ||
					bound.kind != JsonSpan::Kind::array) {
					continue;
				}
				size_t n = 0;
				size_t at = 0;
				JsonSpan dp;
				while (json_next_item(bound, at, dp)) {
					++n;
				}
				if (n > 1) {
					add(id, m.ports[p].id, port_datapoint_type(m.ports[p].type), 0);
				}
			}
		}
	}
	return count;
}

} // namespace hc::blocks
