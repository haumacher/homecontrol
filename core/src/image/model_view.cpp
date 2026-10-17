/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/image/model_view.h"

#include <cstring>

#include "hc/json.h"
#include "hc/log.h"

namespace hc {
namespace image {

namespace {

constexpr const char* kTag = "hc.image";

/// The members of a datapoint that bind it to a technology (copied into its binding).
constexpr const char* kTechnologyMembers[] = {"knx", "zigbee", "virtual"};

size_t count_items(const JsonSpan& list) {
	size_t count = 0;
	size_t position = 0;
	JsonSpan item;
	while (json_next_item(list, position, item)) {
		++count;
	}
	return count;
}

} // namespace

void ModelView::clear() {
	arena_.release();
	locations_ = nullptr;
	location_count_ = 0;
	functions_ = nullptr;
	function_count_ = 0;
	datapoints_ = nullptr;
	datapoint_count_ = 0;
	steps_ = nullptr;
	step_count_ = 0;
	fixed_schedules_ = nullptr;
	fixed_schedule_count_ = 0;
	no_memory_ = false;
	++generation_;
}

bool ModelView::build(const char* text, size_t length, const DatapointModel& model) {
	clear();
	JsonSpan root;
	if (!json_parse(text, length, root, nullptr, kJsonMaxDepth) || root.kind != JsonSpan::Kind::object) {
		Log::error(kTag, "the UI model was not built: the image text does not parse");
		return false;
	}
	bool ok = true;
	// A string span copied as text (unescaped), or nullptr on failure.
	auto string_copy = [&](const JsonSpan& span) -> const char* {
		char buffer[128];
		size_t n = 0;
		if (!json_string(span, buffer, sizeof(buffer), &n)) {
			return "";
		}
		const char* copy = arena_.copy(buffer, n);
		ok = ok && copy != nullptr;
		return copy != nullptr ? copy : "";
	};
	auto raw_copy = [&](const JsonSpan& span) -> const char* {
		const char* copy = arena_.copy(span.data, span.length);
		ok = ok && copy != nullptr;
		return copy;
	};
	auto member_string = [&](const JsonSpan& object, const char* name) -> const char* {
		JsonSpan value;
		return json_member(object, name, value) && value.kind == JsonSpan::Kind::string ? string_copy(value) : "";
	};

	// Locations.
	JsonSpan list;
	if (json_member(root, "locations", list) && list.kind == JsonSpan::Kind::array) {
		size_t count = count_items(list);
		locations_ = arena_.array<Location>(count);
		ok = ok && (count == 0 || locations_ != nullptr);
		size_t position = 0;
		JsonSpan item;
		while (ok && json_next_item(list, position, item)) {
			Location& l = locations_[location_count_++];
			l.id = member_string(item, "id");
			JsonSpan value;
			l.label = json_member(item, "label", value) ? raw_copy(value) : "\"\"";
			l.kind = member_string(item, "kind");
			l.parent = -1;
		}
		// Parents by id (validated: they exist).
		position = 0;
		size_t i = 0;
		while (ok && json_next_item(list, position, item) && i < location_count_) {
			JsonSpan value;
			if (json_member(item, "parent", value)) {
				for (size_t k = 0; k < location_count_; ++k) {
					if (json_string_equals(value, locations_[k].id)) {
						locations_[i].parent = static_cast<int16_t>(k);
					}
				}
			}
			++i;
		}
	}

	// Functions.
	if (ok && json_member(root, "functions", list) && list.kind == JsonSpan::Kind::array) {
		size_t count = count_items(list);
		functions_ = arena_.array<Function>(count);
		ok = ok && (count == 0 || functions_ != nullptr);
		size_t position = 0;
		JsonSpan item;
		while (ok && json_next_item(list, position, item)) {
			Function& f = functions_[function_count_++];
			f.id = member_string(item, "id");
			JsonSpan value;
			f.label = json_member(item, "label", value) ? raw_copy(value) : "\"\"";
			f.kind = member_string(item, "kind");
			f.unit = member_string(item, "unit");
			f.room = -1;
			if (json_member(item, "room", value)) {
				for (size_t k = 0; k < location_count_; ++k) {
					if (json_string_equals(value, locations_[k].id)) {
						f.room = static_cast<int16_t>(k);
					}
				}
			}
			double scene = 0;
			f.scene = json_member(item, "scene", value) && json_number(value, scene) && scene >= 1 && scene <= 64
				? static_cast<uint8_t>(scene) : 0;
			f.roles = nullptr;
			f.role_count = 0;
			JsonSpan roles;
			if (!json_member(item, "datapoints", roles) || roles.kind != JsonSpan::Kind::object) {
				continue;
			}
			size_t n = 0;
			size_t p = 0;
			JsonSpan name;
			while (json_next_member(roles, p, name, value)) {
				++n;
			}
			Role* table = arena_.array<Role>(n);
			ok = ok && (n == 0 || table != nullptr);
			p = 0;
			while (ok && json_next_member(roles, p, name, value) && f.role_count < 255) {
				char dp_name[Datapoint::kMaxName + 1];
				if (!json_string(value, dp_name, sizeof(dp_name))) {
					continue;
				}
				const Datapoint* dp = model.find(dp_name);
				if (dp == nullptr) {
					continue; // an unknown role's datapoint (ignored by the loader, I5)
				}
				Role& r = table[f.role_count++];
				r.name = string_copy(name);
				r.datapoint = dp->id();
			}
			f.roles = table;
		}
	}

	// Datapoints: labels, units, bindings by the model's ids.
	if (ok && model.size() > 0) {
		datapoints_ = arena_.array<DatapointInfo>(model.size());
		ok = datapoints_ != nullptr;
		if (ok) {
			datapoint_count_ = model.size();
		}
	}
	if (ok && json_member(root, "datapoints", list) && list.kind == JsonSpan::Kind::array) {
		size_t position = 0;
		JsonSpan item;
		while (ok && json_next_item(list, position, item)) {
			JsonSpan value;
			char name[Datapoint::kMaxName + 1];
			if (!json_member(item, "id", value) || !json_string(value, name, sizeof(name))) {
				continue;
			}
			const Datapoint* dp = model.find(name);
			if (dp == nullptr || dp->id() >= datapoint_count_) {
				continue;
			}
			DatapointInfo& info = datapoints_[dp->id()];
			if (json_member(item, "label", value)) {
				info.label = raw_copy(value);
			}
			if (json_member(item, "unit", value) && value.kind == JsonSpan::Kind::string) {
				info.unit = string_copy(value);
			}
			// {"knx": {...}}: the technology members verbatim.
			size_t size = 2;
			for (const char* member : kTechnologyMembers) {
				if (json_member(item, member, value)) {
					size += std::strlen(member) + 4 + value.length;
				}
			}
			if (size == 2) {
				continue;
			}
			char* binding = static_cast<char*>(arena_.allocate(size + 1));
			ok = binding != nullptr;
			if (!ok) {
				break;
			}
			size_t at = 0;
			binding[at++] = '{';
			for (const char* member : kTechnologyMembers) {
				if (!json_member(item, member, value)) {
					continue;
				}
				if (at > 1) {
					binding[at++] = ',';
				}
				binding[at++] = '"';
				std::memcpy(binding + at, member, std::strlen(member));
				at += std::strlen(member);
				binding[at++] = '"';
				binding[at++] = ':';
				std::memcpy(binding + at, value.data, value.length);
				at += value.length;
			}
			binding[at++] = '}';
			binding[at] = '\0';
			info.binding = binding;
		}
	}

	// Rule parameter steps.
	if (ok && json_member(root, "rules", list) && list.kind == JsonSpan::Kind::array) {
		size_t count = 0;
		size_t position = 0;
		JsonSpan rule;
		while (json_next_item(list, position, rule)) {
			JsonSpan parameters;
			if (json_member(rule, "parameters", parameters) && parameters.kind == JsonSpan::Kind::array) {
				size_t p = 0;
				JsonSpan parameter;
				JsonSpan value;
				while (json_next_item(parameters, p, parameter)) {
					count += json_member(parameter, "step", value) ? 1 : 0;
				}
			}
		}
		steps_ = arena_.array<ParameterStep>(count);
		ok = count == 0 || steps_ != nullptr;
		position = 0;
		while (ok && count > 0 && json_next_item(list, position, rule)) {
			JsonSpan parameters;
			if (!json_member(rule, "parameters", parameters) || parameters.kind != JsonSpan::Kind::array) {
				continue;
			}
			const char* rule_id = nullptr;
			size_t p = 0;
			JsonSpan parameter;
			while (ok && json_next_item(parameters, p, parameter) && step_count_ < count) {
				JsonSpan value;
				double step = 0;
				if (!json_member(parameter, "step", value) || !json_number(value, step)) {
					continue;
				}
				if (rule_id == nullptr) {
					rule_id = member_string(rule, "id");
				}
				ParameterStep& s = steps_[step_count_++];
				s.rule = rule_id;
				s.parameter = member_string(parameter, "id");
				s.step = step;
			}
		}
	}

	// Schedule entries whose switch is fixed.
	if (ok && json_member(root, "schedules", list) && list.kind == JsonSpan::Kind::array) {
		size_t count = count_items(list);
		fixed_schedules_ = arena_.array<const char*>(count);
		ok = count == 0 || fixed_schedules_ != nullptr;
		size_t position = 0;
		JsonSpan item;
		while (ok && json_next_item(list, position, item)) {
			JsonSpan value;
			bool adjustable = true;
			if (json_member(item, "enabledAdjustable", value) && json_bool(value, adjustable) && !adjustable) {
				fixed_schedules_[fixed_schedule_count_++] = member_string(item, "id");
			}
		}
	}

	if (!ok) {
		Log::error(kTag, "the UI model was not built: no memory (the image runs; GET /api/model refuses)");
		clear();
		no_memory_ = true;
		return false;
	}
	return true;
}

double ModelView::parameter_step(const char* rule, const char* parameter) const {
	for (size_t i = 0; i < step_count_; ++i) {
		if (std::strcmp(steps_[i].rule, rule) == 0 && std::strcmp(steps_[i].parameter, parameter) == 0) {
			return steps_[i].step;
		}
	}
	return 0;
}

bool ModelView::schedule_adjustable(const char* id) const {
	for (size_t i = 0; i < fixed_schedule_count_; ++i) {
		if (std::strcmp(fixed_schedules_[i], id) == 0) {
			return false;
		}
	}
	return true;
}

} // namespace image
} // namespace hc
