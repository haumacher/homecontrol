/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The blocks' API (spec/api.md "Blocks", spec/blocks.md 9 and 12): GET /api/blocks, GET
// /api/blocks/types and PUT /api/blocks/<id>.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "hc/blocks/block_runtime.h"
#include "hc/image/image_service.h"
#include "hc/value_json.h"
#include "hc/web_api.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.web";
constexpr size_t kClosingReserve = 96;

/// A number, or a time of day as {"hour", "minute", "second"}, or a boolean, by the parameter's type.
void write_param_value(JsonWriter& json, blocks::ParamType type, double v) {
	switch (type) {
		case blocks::ParamType::boolean: json.boolean(v != 0); return;
		case blocks::ParamType::time_of_day: {
			uint32_t s = static_cast<uint32_t>(v);
			json.begin_object()
				.member("hour", s / 3600)
				.member("minute", s % 3600 / 60)
				.member("second", s % 60)
				.end_object();
			return;
		}
		case blocks::ParamType::real: json.number(v); return;
		default: json.number(static_cast<int64_t>(v)); return;
	}
}

/// A real with 9 significant digits: the diagnostics' figures, not the record's bits.
void write_short(JsonWriter& json, double v) {
	char text[24];
	int n = std::snprintf(text, sizeof(text), "%.9g", v);
	json.raw(text, n > 0 ? static_cast<size_t>(n) : 0);
}

void write_port_value(JsonWriter& json, blocks::PortType type, double v) {
	if (type == blocks::PortType::boolean) {
		json.boolean(v != 0);
	} else if (type == blocks::PortType::real) {
		write_short(json, v);
	} else {
		json.number(static_cast<int64_t>(v));
	}
}

void write_manifest(JsonWriter& json, const blocks::Manifest& m) {
	char version[16];
	std::snprintf(version, sizeof(version), "%u.%u", static_cast<unsigned>(m.major), static_cast<unsigned>(m.minor));
	json.begin_object().member("type", m.type).member("version", version).member("label", m.label);
	json.member("description", m.description).member("period", m.period_ms).member("persistEvery", m.persist_every_ms);
	json.key("ports").begin_array();
	for (size_t p = 0; p < m.port_count; ++p) {
		const blocks::PortDecl& d = m.ports[p];
		json.begin_object()
			.member("id", d.id)
			.member("direction", d.direction == blocks::Direction::in ? "in" : "out")
			.member("type", blocks::port_type_name(d.type))
			.member("label", d.label);
		if (d.unit[0] != '\0') {
			json.member("unit", d.unit);
		}
		if (d.direction == blocks::Direction::in) {
			json.member("required", d.required).member("multiple", static_cast<uint32_t>(d.multiple));
			if (d.multiple > 1) {
				json.key("aggregations").begin_array();
				json.string(blocks::aggregation_name(d.default_aggregation));
				for (blocks::Aggregation a : {blocks::Aggregation::mean, blocks::Aggregation::min, blocks::Aggregation::max,
						 blocks::Aggregation::any, blocks::Aggregation::all}) {
					if (a != d.default_aggregation && (d.aggregations & blocks::aggregation_bit(a)) != 0) {
						json.string(blocks::aggregation_name(a));
					}
				}
				json.end_array();
			}
			json.member("trigger", blocks::input_trigger_name(d.trigger));
		} else if (d.diagnostic) {
			json.member("diagnostic", true);
		} else {
			json.key("failsafe");
			write_port_value(json, d.type, d.failsafe);
		}
		json.end_object();
	}
	json.end_array();
	json.key("parameters").begin_array();
	for (size_t p = 0; p < m.param_count; ++p) {
		const blocks::ParamDecl& d = m.params[p];
		json.begin_object()
			.member("id", d.id)
			.member("type", blocks::param_type_name(d.type))
			.member("label", d.label)
			.member("adjustable", d.adjustable);
		if (d.unit[0] != '\0') {
			json.member("unit", d.unit);
		}
		json.key("default");
		write_param_value(json, d.type, d.default_value);
		if (d.has_min) {
			json.key("min");
			write_param_value(json, d.type, d.min);
		}
		if (d.has_max) {
			json.key("max");
			write_param_value(json, d.type, d.max);
		}
		if (d.choice_count > 0) {
			json.key("choices").begin_array();
			for (size_t c = 0; c < d.choice_count; ++c) {
				json.begin_object()
					.member("value", static_cast<int64_t>(d.choices[c].value))
					.member("id", d.choices[c].id)
					.member("label", d.choices[c].label)
					.end_object();
			}
			json.end_array();
		}
		json.end_object();
	}
	json.end_array();
	json.key("state").begin_array();
	for (size_t f = 0; f < m.state_count; ++f) {
		json.begin_object().member("id", m.state[f].id).member("persist", m.state[f].persist).end_object();
	}
	json.end_array();
	json.key("timers").begin_array();
	for (size_t t = 0; t < m.timer_count; ++t) {
		json.string(m.timers[t]);
	}
	json.end_array();
	json.end_object();
}

int change_http_status(blocks::ChangeStatus status) {
	switch (status) {
		case blocks::ChangeStatus::unknown_block:
		case blocks::ChangeStatus::unknown_parameter: return 404;
		case blocks::ChangeStatus::not_adjustable: return 403;
		case blocks::ChangeStatus::invalid_value: return 422;
		case blocks::ChangeStatus::not_faulted: return 409;
		default: return 500;
	}
}

} // namespace

void WebApi::write_block(JsonWriter& json, size_t index) {
	blocks::BlockRuntime& runtime = image_->blocks();
	const blocks::BlockSpec& spec = runtime.set().at(index);
	const blocks::Manifest& m = spec.type->manifest();
	blocks::BlockDiagnostics d;
	runtime.diagnostics(index, d);
	char version[16];
	std::snprintf(version, sizeof(version), "%u.%u", static_cast<unsigned>(m.major), static_cast<unsigned>(spec.minor));
	json.begin_object().member("id", spec.id).member("type", m.type).member("version", version).key("label");
	json.raw(spec.label, std::strlen(spec.label));
	json.member("enabled", runtime.enabled(index))
		.member("enabledAdjustable", spec.enabled_adjustable)
		.member("enabledOverridden", runtime.enabled_overridden(index))
		.member("status", blocks::block_status_name(d.status));
	json.key("fault");
	if (d.status == blocks::BlockStatus::fault) {
		json.begin_object().member("reason", d.fault).member("ms", d.fault_ms).end_object();
	} else {
		json.null();
	}
	json.member("calls", d.calls);
	json.key("lastCall");
	if (d.last_call != blocks::CallKind::none) {
		json.begin_object().member("kind", blocks::call_kind_name(d.last_call));
		if (d.last_call_arg[0] != '\0') {
			json.member("arg", d.last_call_arg);
		}
		json.member("ms", d.last_call_ms).end_object();
	} else {
		json.null();
	}
	json.member("timeMs", d.time_ms).member("longestMs", d.longest_ms).member("errors", d.errors);
	json.key("lastError");
	if (d.has_error) {
		json.begin_object()
			.member("code", blocks::block_log_code_name(d.last_error_code))
			.member("text", d.last_error)
			.member("ms", d.last_error_ms)
			.end_object();
	} else {
		json.null();
	}
	json.key("state").begin_object();
	const double* state = runtime.state(index);
	for (size_t f = 0; f < m.state_count; ++f) {
		json.key(m.state[f].id);
		write_short(json, state[f]);
	}
	json.end_object();
	json.key("inputs").begin_array();
	for (size_t p = 0; p < m.port_count; ++p) {
		const blocks::PortDecl& decl = m.ports[p];
		const blocks::PortBinding& binding = spec.ports[p];
		if (decl.direction != blocks::Direction::in) {
			continue;
		}
		blocks::InputValue v = runtime.input(index, p);
		json.begin_object().member("id", decl.id).key("datapoints").begin_array();
		for (size_t k = 0; k < binding.count; ++k) {
			const Datapoint* dp = model_.get(binding.datapoints[k]);
			json.string(dp != nullptr ? dp->name() : "?");
		}
		json.end_array();
		if (binding.count > 1) {
			json.member("aggregation", blocks::aggregation_name(binding.aggregation));
		}
		json.key("value");
		if (v.quality != Quality::unknown) {
			write_port_value(json, decl.type, v.value);
		} else {
			json.null();
		}
		json.member("quality", quality_name(v.quality)).end_object();
	}
	json.end_array();
	json.key("outputs").begin_array();
	for (size_t p = 0; p < m.port_count; ++p) {
		const blocks::PortDecl& decl = m.ports[p];
		if (decl.direction != blocks::Direction::out) {
			continue;
		}
		json.begin_object().member("id", decl.id).key("datapoint");
		const Datapoint* bound = spec.ports[p].count > 0 ? model_.get(spec.ports[p].datapoints[0]) : nullptr;
		if (bound != nullptr) {
			json.string(bound->name());
		} else {
			json.null();
		}
		const Datapoint* mirror = model_.get(runtime.mirror(index, p));
		json.key("value");
		if (mirror != nullptr && mirror->quality() != Quality::unknown) {
			write_value_json(json, mirror->value());
		} else {
			json.null();
		}
		if (decl.diagnostic) {
			json.member("diagnostic", true);
		}
		if (decl.unit[0] != '\0') {
			json.member("unit", decl.unit);
		}
		json.end_object();
	}
	json.end_array();
	json.key("timers").begin_array();
	for (size_t t = 0; t < m.timer_count; ++t) {
		int64_t remaining = 0;
		if (runtime.timer_remaining(index, t, remaining)) {
			json.begin_object().member("id", m.timers[t]).member("remaining_ms", remaining).end_object();
		}
	}
	json.end_array();
	// Every parameter's value; the adjustable ones in full below (the rest is the type's manifest,
	// GET /api/blocks/types), so that a block fits the E-Home connection's 4 KB answers.
	json.key("values").begin_object();
	for (size_t p = 0; p < m.param_count; ++p) {
		json.key(m.params[p].id);
		if (m.params[p].type == blocks::ParamType::real) {
			write_short(json, runtime.param(index, p));
		} else {
			write_param_value(json, m.params[p].type, runtime.param(index, p));
		}
	}
	json.end_object();
	json.key("parameters").begin_array();
	for (size_t p = 0; p < m.param_count; ++p) {
		const blocks::ParamDecl& decl = m.params[p];
		if (!decl.adjustable) {
			continue;
		}
		json.begin_object()
			.member("id", decl.id)
			.member("label", decl.label)
			.member("type", blocks::param_type_name(decl.type))
			.member("unit", decl.unit)
			.member("adjustable", decl.adjustable)
			.member("overridden", runtime.overridden(index, p));
		json.key("value");
		write_param_value(json, decl.type, runtime.param(index, p));
		json.key("default");
		write_param_value(json, decl.type, spec.defaults[p]);
		if (decl.has_min) {
			json.key("min");
			write_param_value(json, decl.type, decl.min);
		}
		if (decl.has_max) {
			json.key("max");
			write_param_value(json, decl.type, decl.max);
		}
		if (decl.step > 0) {
			json.member("step", decl.step);
		}
		if (decl.choice_count > 0) {
			json.key("choices").begin_array();
			for (size_t c = 0; c < decl.choice_count; ++c) {
				json.begin_object()
					.member("value", static_cast<int64_t>(decl.choices[c].value))
					.member("id", decl.choices[c].id)
					.member("label", decl.choices[c].label)
					.end_object();
			}
			json.end_array();
		}
		json.end_object();
	}
	json.end_array();
	json.end_object();
}

void WebApi::blocks_list(const WebRequest& request, WebResponse& response) {
	uint32_t from = 0;
	query_uint(request.query, "from", from);
	blocks::BlockRuntime& runtime = image_->blocks();
	size_t count = runtime.size();
	JsonWriter json(response.body, response.capacity);
	json.reserve(kClosingReserve);
	json.begin_object()
		.member("now_ms", controller_.clock().monotonic_ms())
		.member("count", static_cast<uint32_t>(count))
		.member("from", from);
	json.key("blocks").begin_array();
	size_t index = from;
	size_t written = 0;
	for (; index < count; ++index) {
		JsonWriter::Mark mark = json.mark();
		write_block(json, index);
		if (json.overflowed()) {
			json.reset(mark);
			break;
		}
		++written;
	}
	if (written == 0 && index < count) {
		refuse(response, 500, "api.response_too_large", "a block does not fit the server's buffer");
		return;
	}
	json.release();
	json.end_array();
	json.key("next");
	if (index < count) {
		json.number(static_cast<uint32_t>(index));
	} else {
		json.null();
	}
	json.end_object();
	finish(response, json);
}

void WebApi::block_types(const WebRequest& request, WebResponse& response) {
	uint32_t from = 0;
	query_uint(request.query, "from", from);
	const blocks::BlockRegistry& registry = blocks::BlockRegistry::device();
	JsonWriter json(response.body, response.capacity);
	json.reserve(kClosingReserve);
	json.begin_object().member("count", static_cast<uint32_t>(registry.size())).member("from", from);
	json.key("types").begin_array();
	size_t index = from;
	size_t written = 0;
	for (; index < registry.size(); ++index) {
		JsonWriter::Mark mark = json.mark();
		write_manifest(json, registry.at(index).manifest());
		if (json.overflowed()) {
			json.reset(mark);
			break;
		}
		++written;
	}
	if (written == 0 && index < registry.size()) {
		refuse(response, 500, "api.response_too_large", "a block type does not fit the server's buffer");
		return;
	}
	json.release();
	json.end_array();
	json.key("next");
	if (index < registry.size()) {
		json.number(static_cast<uint32_t>(index));
	} else {
		json.null();
	}
	json.end_object();
	finish(response, json);
}

void WebApi::put_block(const char* id, const WebRequest& request, WebResponse& response) {
	blocks::BlockRuntime& runtime = image_->blocks();
	int index = runtime.find(id);
	if (index < 0) {
		refuse(response, 404, "block.unknown", "no block with this id");
		return;
	}
	JsonSpan body;
	const char* error = nullptr;
	if (!json_parse(request.body, request.body_length, body, &error) || body.kind != JsonSpan::Kind::object) {
		refuse(response, 400, "api.bad_json", error != nullptr ? error : "the body must be a JSON object");
		return;
	}
	// Checked whole, then applied (spec/api.md "Blocks"): parameters, reset, enabled, restart.
	JsonSpan parameters, reset, enabled, restart, key, value;
	bool has_parameters = json_member(body, "parameters", parameters);
	bool has_reset = json_member(body, "reset", reset);
	bool has_enabled = json_member(body, "enabled", enabled);
	bool has_restart = json_member(body, "restart", restart);
	size_t position = 0;
	while (json_next_member(body, position, key, value)) {
		if (!json_string_equals(key, "parameters") && !json_string_equals(key, "reset") &&
			!json_string_equals(key, "enabled") && !json_string_equals(key, "restart")) {
			refuse(response, 400, "block.malformed", "the body has a member other than parameters, reset, enabled, restart");
			return;
		}
	}
	bool enable_to = false;
	bool restart_to = false;
	if ((has_parameters && parameters.kind != JsonSpan::Kind::object) ||
		(has_reset && reset.kind != JsonSpan::Kind::array) || (has_enabled && !json_bool(enabled, enable_to)) ||
		(has_restart && (!json_bool(restart, restart_to) || !restart_to))) {
		refuse(response, 400, "block.malformed",
			"parameters is an object, reset an array of ids, enabled true or false, restart true");
		return;
	}
	if (!has_parameters && !has_reset && !has_enabled && !has_restart) {
		refuse(response, 400, "block.value_missing", "the body needs parameters, reset, enabled or restart");
		return;
	}
	blocks::ChangeStatus status = blocks::ChangeStatus::ok;
	const char* reason = "";
	char member[40] = "";
	position = 0;
	while (has_parameters && status == blocks::ChangeStatus::ok && json_next_member(parameters, position, key, value)) {
		if (!json_string(key, member, sizeof(member))) {
			status = blocks::ChangeStatus::unknown_parameter;
			reason = "no such parameter";
			break;
		}
		status = runtime.check_parameter(id, member, value.kind == JsonSpan::Kind::null ? nullptr : &value, nullptr, &reason);
	}
	position = 0;
	while (has_reset && status == blocks::ChangeStatus::ok && json_next_item(reset, position, value)) {
		if (!json_string(value, member, sizeof(member))) {
			status = blocks::ChangeStatus::unknown_parameter;
			reason = "no such parameter";
			break;
		}
		status = runtime.check_parameter(id, member, nullptr, nullptr, &reason);
	}
	if (status == blocks::ChangeStatus::ok && has_enabled) {
		std::snprintf(member, sizeof(member), "enabled");
		status = runtime.check_enabled(id, &reason);
	}
	if (status == blocks::ChangeStatus::ok && has_restart) {
		std::snprintf(member, sizeof(member), "restart");
		status = runtime.check_restart(id, &reason);
	}
	if (status != blocks::ChangeStatus::ok) {
		if (status != blocks::ChangeStatus::not_faulted) {
			runtime.refused(id, member, reason);
		}
		char code[40];
		std::snprintf(code, sizeof(code), "block.%s", blocks::change_status_name(status));
		Log::warn(kTag, "block %s: %s refused: %s", id, member, reason);
		JsonWriter json(response.body, response.capacity);
		json.begin_object().member("error", reason).member("code", code).member("member", member).end_object();
		response.status = change_http_status(status);
		response.content_type = "application/json";
		response.length = json.overflowed() ? 0 : json.length();
		return;
	}
	position = 0;
	while (has_parameters && json_next_member(parameters, position, key, value)) {
		json_string(key, member, sizeof(member));
		if (value.kind == JsonSpan::Kind::null) {
			runtime.reset_parameter(id, member, &reason);
		} else {
			runtime.set_parameter(id, member, value, &reason);
		}
	}
	position = 0;
	while (has_reset && json_next_item(reset, position, value)) {
		json_string(value, member, sizeof(member));
		runtime.reset_parameter(id, member, &reason);
	}
	if (has_enabled) {
		runtime.set_enabled(id, enable_to, &reason);
	}
	if (has_restart) {
		runtime.restart(id, &reason);
	}
	// The calls the change causes run now, so the answer shows their effect.
	image_->engine().flush();
	Log::info(kTag, "block %s changed from the API", id);
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).member("now_ms", controller_.clock().monotonic_ms()).key("block");
	write_block(json, static_cast<size_t>(index));
	json.end_object();
	finish(response, json);
}

} // namespace hc
