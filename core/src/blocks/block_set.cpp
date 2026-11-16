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

#include "hc/image/binary.h"
#include "hc/json.h"
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

namespace {

/// The refusal of a block of the binary image (spec/runtime-image.md 10.8): `<code>: <text>` at `path`.
bool refuse(BlockLoadResult& result, BlockError error, const char* path, const char* format, ...)
	HC_PRINTF_FORMAT(4, 5);
bool refuse(BlockLoadResult& result, BlockError error, const char* path, const char* format, ...) {
	result.error = error;
	std::snprintf(result.path, sizeof(result.path), "%s", path);
	va_list args;
	va_start(args, format);
	std::vsnprintf(result.text, sizeof(result.text), format, args);
	va_end(args);
	return false;
}

/// Compiles block `i` of the image into `spec`.
bool bind_block(const image::bin::View& view, size_t i, const BlockRegistry& registry, BlockSpec& spec,
	BlockLoadResult& result) {
	using namespace image::bin;
	const BlockRec& r = view.at<BlockRec>(kBlocks, i);
	char path[sizeof(BlockLoadResult::path)];
	std::snprintf(spec.id, sizeof(spec.id), "%s", view.str(r.id));
	std::snprintf(result.block, sizeof(result.block), "%s", spec.id);
	const char* type = view.str(r.type);
	std::snprintf(path, sizeof(path), "blocks[%u].type", static_cast<unsigned>(i));
	spec.type = registry.find(type, r.major);
	if (spec.type == nullptr || spec.type->manifest().minor < r.minor) {
		const BlockType* any = spec.type != nullptr ? spec.type : registry.find_any(type);
		if (any == nullptr) {
			return refuse(result, BlockError::unknown_type, path, "%s %u.%u (this firmware has no such block type)", type,
				static_cast<unsigned>(r.major), static_cast<unsigned>(r.minor));
		}
		return refuse(result, BlockError::unknown_type, path, "%s %u.%u (this firmware has %u.%u)", type,
			static_cast<unsigned>(r.major), static_cast<unsigned>(r.minor), static_cast<unsigned>(any->manifest().major),
			static_cast<unsigned>(any->manifest().minor));
	}
	spec.minor = r.minor;
	JsonWriter counter = JsonWriter::counter();
	write_label(counter, view, r.label);
	spec.label = new (std::nothrow) char[counter.length() + 1];
	if (spec.label == nullptr) {
		std::snprintf(path, sizeof(path), "blocks[%u]", static_cast<unsigned>(i));
		return refuse(result, BlockError::out_of_memory, path, "no memory for the label");
	}
	JsonWriter json(spec.label, counter.length() + 1);
	write_label(json, view, r.label);
	spec.label[counter.length()] = '\0';
	spec.enabled = (r.flags & 0x01) != 0;
	spec.enabled_adjustable = (r.flags & 0x02) != 0;
	const Manifest& m = spec.type->manifest();
	if (r.port_count > m.port_count) {
		std::snprintf(path, sizeof(path), "blocks.ports[%u].id", static_cast<unsigned>(r.first_port + m.port_count));
		return refuse(result, BlockError::unknown_reference, path, "%u ports, the type %s has %u",
			static_cast<unsigned>(r.port_count), m.type, static_cast<unsigned>(m.port_count));
	}
	if (r.parameter_count > m.param_count) {
		std::snprintf(path, sizeof(path), "blocks.parameters[%u].id",
			static_cast<unsigned>(r.first_parameter + m.param_count));
		return refuse(result, BlockError::unknown_reference, path, "%u parameters, the type %s has %u",
			static_cast<unsigned>(r.parameter_count), m.type, static_cast<unsigned>(m.param_count));
	}
	// The pool of bound ids (spec/blocks.md B20): the ports' runs, one slot per unbound output.
	size_t listed = 0;
	for (size_t p = 0; p < r.port_count; ++p) {
		listed += view.at<BlockPortRec>(kBlockPorts, r.first_port + p).bound_count;
	}
	size_t outputs = m.port_count - m.inputs();
	spec.bound_size = static_cast<uint16_t>(listed + outputs);
	spec.ports = new (std::nothrow) PortBinding[m.port_count > 0 ? m.port_count : 1];
	spec.defaults = new (std::nothrow) double[m.param_count > 0 ? m.param_count : 1];
	spec.bound = new (std::nothrow) uint16_t[spec.bound_size > 0 ? spec.bound_size : 1];
	if (spec.ports == nullptr || spec.defaults == nullptr || spec.bound == nullptr) {
		std::snprintf(path, sizeof(path), "blocks[%u]", static_cast<unsigned>(i));
		return refuse(result, BlockError::out_of_memory, path, "no memory for the block's tables");
	}
	size_t next = 0;
	for (size_t p = 0; p < m.port_count; ++p) {
		const PortDecl& decl = m.ports[p];
		PortBinding& b = spec.ports[p];
		b = PortBinding{spec.bound + next, 0, decl.default_aggregation};
		if (p >= r.port_count) {
			if (decl.direction == Direction::out) {
				spec.bound[next++] = 0; // an unbound output keeps its slot
			}
			continue;
		}
		size_t q = r.first_port + p;
		const BlockPortRec& port = view.at<BlockPortRec>(kBlockPorts, q);
		auto at = [&](const char* field) {
			std::snprintf(path, sizeof(path), "blocks.ports[%u].%s", static_cast<unsigned>(q), field);
			return path;
		};
		if (std::strcmp(view.str(port.id), decl.id) != 0) {
			return refuse(result, BlockError::unknown_reference, at("id"), "port '%s' at position %u, the type has '%s'",
				view.str(port.id), static_cast<unsigned>(p), decl.id);
		}
		if (decl.direction == Direction::out) {
			if (port.bound_count > 1) {
				return refuse(result, BlockError::forbidden, at("boundCount"), "more datapoints than 'multiple'");
			}
			if (port.mirror == kNone) {
				return refuse(result, BlockError::forbidden, at("mirror"), "the output '%s' has no mirror", decl.id);
			}
			if (port.aggregation != 0) {
				return refuse(result, BlockError::forbidden, at("aggregation"), "an output has no aggregation");
			}
			spec.bound[next] = 0;
			if (port.bound_count == 1) {
				uint16_t id = view.at<uint16_t>(kBlockBound, port.first_bound);
				ValueType type = static_cast<ValueType>(view.at<DatapointRec>(kDatapoints, id).type);
				if (!output_accepts(decl.type, type)) {
					return refuse(result, BlockError::type_error, at("firstBound"),
						"the %s port '%s' cannot command the %s datapoint '%s'", port_type_name(decl.type), decl.id,
						value_type_name(type), view.str(view.at<DatapointRec>(kDatapoints, id).name));
				}
				spec.bound[next] = id;
				b.count = 1;
			}
			++next;
			continue;
		}
		if (port.bound_count > decl.multiple) {
			return refuse(result, BlockError::forbidden, at("boundCount"), "more datapoints than 'multiple' (%u)",
				static_cast<unsigned>(decl.multiple));
		}
		if ((port.mirror != kNone) != (port.bound_count > 1)) {
			return refuse(result, BlockError::forbidden, at("mirror"),
				"an input has a mirror exactly when it is bound to more than one datapoint");
		}
		if (port.bound_count > 1) {
			Aggregation a = static_cast<Aggregation>(port.aggregation);
			if (a == Aggregation::none || (decl.aggregations & aggregation_bit(a)) == 0) {
				return refuse(result, BlockError::forbidden, at("aggregation"), "the input '%s' does not allow the aggregation %u",
					decl.id, static_cast<unsigned>(port.aggregation));
			}
			b.aggregation = a;
		} else if (port.aggregation != 0) {
			return refuse(result, BlockError::forbidden, at("aggregation"),
				"the input '%s' does not bind several datapoints: nothing to aggregate", decl.id);
		}
		for (size_t k = 0; k < port.bound_count; ++k) {
			uint16_t id = view.at<uint16_t>(kBlockBound, port.first_bound + k);
			const DatapointRec& dp = view.at<DatapointRec>(kDatapoints, id);
			ValueType type = static_cast<ValueType>(dp.type);
			if (!input_accepts(decl.type, type)) {
				return refuse(result, BlockError::type_error, at("firstBound"),
					"the %s port '%s' cannot read the %s datapoint '%s'", port_type_name(decl.type), decl.id,
					value_type_name(type), view.str(dp.name));
			}
			spec.bound[next + k] = id;
		}
		b.count = port.bound_count;
		next += port.bound_count;
	}
	for (size_t p = 0; p < m.port_count; ++p) {
		if (m.ports[p].direction == Direction::in && m.ports[p].required && spec.ports[p].count == 0) {
			std::snprintf(path, sizeof(path), "blocks[%u].firstPort", static_cast<unsigned>(i));
			return refuse(result, BlockError::missing_input, path, "the required input '%s' of %s is not bound",
				m.ports[p].id, m.type);
		}
	}
	for (size_t p = 0; p < m.param_count; ++p) {
		const ParamDecl& decl = m.params[p];
		spec.defaults[p] = decl.default_value;
		if (p >= r.parameter_count) {
			continue; // a newer minor of the type: the manifest's default, not adjustable (9.15)
		}
		size_t q = r.first_parameter + p;
		const BlockParameterRec& param = view.at<BlockParameterRec>(kBlockParameters, q);
		auto at = [&](const char* field) {
			std::snprintf(path, sizeof(path), "blocks.parameters[%u].%s", static_cast<unsigned>(q), field);
			return path;
		};
		if (std::strcmp(view.str(param.id), decl.id) != 0) {
			return refuse(result, BlockError::unknown_reference, at("id"), "parameter '%s' at position %u, the type has '%s'",
				view.str(param.id), static_cast<unsigned>(p), decl.id);
		}
		if ((param.datapoint != kNone) != decl.adjustable) {
			return refuse(result, BlockError::invalid_parameter, at("datapoint"),
				"the parameter '%s' %s a datapoint", decl.id, decl.adjustable ? "is adjustable and needs" : "is not adjustable and has no");
		}
		if ((param.flags & 0x01) != 0) {
			const char* reason = nullptr;
			if (!param_type_ok(decl, param.value, &reason)) {
				return refuse(result, BlockError::type_error, at("value"), "%s: %s", decl.id, reason);
			}
			if (!param_valid(decl, param.value, &reason)) {
				return refuse(result, BlockError::invalid_parameter, at("value"), "%s: %s", decl.id, reason);
			}
			spec.defaults[p] = param.value;
		}
	}
	size_t blamed = 0;
	const char* reason = spec.type->check(spec.defaults, blamed);
	if (reason != nullptr) {
		if (blamed < m.param_count && blamed < r.parameter_count) {
			std::snprintf(path, sizeof(path), "blocks.parameters[%u].value", static_cast<unsigned>(r.first_parameter + blamed));
		} else {
			std::snprintf(path, sizeof(path), "blocks[%u]", static_cast<unsigned>(i));
		}
		return refuse(result, BlockError::invalid_parameter, path, "%s", reason);
	}
	return true;
}

} // namespace

BlockLoadResult BlockSet::bind(const image::bin::View& view, const BlockRegistry& registry) {
	clear();
	BlockLoadResult result;
	size_t count = view.count(image::bin::kBlocks);
	if (count == 0) {
		return result;
	}
	BlockSpec* specs = new (std::nothrow) BlockSpec[count];
	if (specs == nullptr) {
		refuse(result, BlockError::out_of_memory, "blocks", "no memory for %u blocks", static_cast<unsigned>(count));
		return result;
	}
	std::memset(static_cast<void*>(specs), 0, sizeof(BlockSpec) * count);
	size_t done = 0;
	bool ok = true;
	while (ok && done < count) {
		ok = bind_block(view, done, registry, specs[done], result);
		++done;
	}
	// The persistence keys of the device must not collide (spec/blocks.md 11): the record blobs and
	// the override keys of the store of the rule parameters.
	for (size_t i = 0; ok && i < count; ++i) {
		for (size_t k = 0; ok && k < i; ++k) {
			if (persist::fnv1a(specs[i].id) == persist::fnv1a(specs[k].id)) {
				char path[24];
				std::snprintf(path, sizeof(path), "blocks[%u]", static_cast<unsigned>(i));
				ok = refuse(result, BlockError::limit_exceeded, path,
					"the persistence keys of '%s' and '%s' collide in the device's store", specs[i].id, specs[k].id);
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
			ok = refuse(result, BlockError::out_of_memory, "blocks", "no memory to check the stored overrides' keys");
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
					ok = refuse(result, BlockError::limit_exceeded, "blocks",
						"two stored parameter overrides of the blocks collide in the device's store");
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

} // namespace hc::blocks
