/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/blocks/block_type.h"

#include <cmath>
#include <cstring>

#include "hc/blocks/heating_curve.h"
#include "hc/blocks/heating_demand.h"
#include "hc/blocks/house_mode.h"
#include "hc/blocks/room_controller.h"
#include "hc/blocks/sun_shading.h"

namespace hc::blocks {

const char* port_type_name(PortType type) {
	switch (type) {
		case PortType::boolean: return "boolean";
		case PortType::integer: return "integer";
		case PortType::real: return "real";
		case PortType::enumeration: return "enumeration";
	}
	return "?";
}

const char* param_type_name(ParamType type) {
	switch (type) {
		case ParamType::boolean: return "boolean";
		case ParamType::integer: return "integer";
		case ParamType::real: return "real";
		case ParamType::duration: return "duration";
		case ParamType::time_of_day: return "timeOfDay";
		case ParamType::enumeration: return "enumeration";
	}
	return "?";
}

const char* aggregation_name(Aggregation a) {
	switch (a) {
		case Aggregation::none: return "none";
		case Aggregation::mean: return "mean";
		case Aggregation::min: return "min";
		case Aggregation::max: return "max";
		case Aggregation::any: return "any";
		case Aggregation::all: return "all";
	}
	return "?";
}

Aggregation aggregation_of(const char* name) {
	static const Aggregation kAll[] = {Aggregation::mean, Aggregation::min, Aggregation::max, Aggregation::any,
		Aggregation::all};
	for (Aggregation a : kAll) {
		if (std::strcmp(name, aggregation_name(a)) == 0) {
			return a;
		}
	}
	return Aggregation::none;
}

const char* input_trigger_name(InputTrigger trigger) {
	switch (trigger) {
		case InputTrigger::change: return "change";
		case InputTrigger::update: return "update";
		case InputTrigger::none: return "none";
	}
	return "?";
}

const char* start_reason_name(StartReason reason) {
	switch (reason) {
		case StartReason::boot: return "boot";
		case StartReason::image: return "image";
		case StartReason::enable: return "enable";
		case StartReason::restart: return "restart";
	}
	return "?";
}

namespace {

int index_of(const char* id, size_t count, const char* (*at)(const Manifest&, size_t), const Manifest& m) {
	for (size_t i = 0; i < count; ++i) {
		if (std::strcmp(at(m, i), id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

/// An identifier of spec/rules.md 2.1 (without the keyword check: block ids never meet expressions).
bool identifier(const char* id) {
	size_t n = std::strlen(id);
	if (n == 0 || n > 24) {
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		char c = id[i];
		bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
		if (!letter && !(i > 0 && c >= '0' && c <= '9')) {
			return false;
		}
	}
	return true;
}

} // namespace

int Manifest::port(const char* id) const {
	return index_of(id, port_count, [](const Manifest& m, size_t i) { return m.ports[i].id; }, *this);
}

int Manifest::param(const char* id) const {
	return index_of(id, param_count, [](const Manifest& m, size_t i) { return m.params[i].id; }, *this);
}

int Manifest::field(const char* id) const {
	return index_of(id, state_count, [](const Manifest& m, size_t i) { return m.state[i].id; }, *this);
}

int Manifest::timer(const char* id) const {
	return index_of(id, timer_count, [](const Manifest& m, size_t i) { return m.timers[i]; }, *this);
}

bool Manifest::persists() const {
	for (size_t i = 0; i < state_count; ++i) {
		if (state[i].persist) {
			return true;
		}
	}
	return false;
}

size_t Manifest::inputs() const {
	size_t n = 0;
	for (size_t i = 0; i < port_count; ++i) {
		n += ports[i].direction == Direction::in ? 1 : 0;
	}
	return n;
}

const char* check_manifest(const Manifest& m) {
	size_t dots = 0;
	size_t length = std::strlen(m.type);
	for (size_t i = 0; i < length; ++i) {
		char c = m.type[i];
		dots += c == '.' ? 1 : 0;
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
			return "the type id has a character outside a-z 0-9 . -";
		}
	}
	if (length < 3 || length > 63 || dots < 2) {
		return "the type id is not a reverse-DNS id of 3..63 characters";
	}
	if (m.port_count > BlockLimits::kPorts || m.param_count > BlockLimits::kParams ||
		m.state_count > BlockLimits::kStateFields || m.timer_count > BlockLimits::kTimers) {
		return "more ports, parameters, state fields or timers than the limits";
	}
	if (m.period_ms != 0 && (m.period_ms < 1000 || m.period_ms > 86400000)) {
		return "the period is not 1 s .. 1 d";
	}
	if (m.persists() && (m.persist_every_ms < 60000 || m.persist_every_ms > 86400000)) {
		return "persistEvery is not 1 min .. 1 d";
	}
	// Ports and parameters share the namespace of block/<id>/<x>, which also holds `enabled`.
	for (size_t i = 0; i < m.port_count + m.param_count; ++i) {
		const char* id = i < m.port_count ? m.ports[i].id : m.params[i - m.port_count].id;
		if (!identifier(id)) {
			return "a port or parameter id is not an identifier";
		}
		if (std::strcmp(id, "enabled") == 0) {
			return "the id 'enabled' is reserved";
		}
		for (size_t k = 0; k < i; ++k) {
			const char* other = k < m.port_count ? m.ports[k].id : m.params[k - m.port_count].id;
			if (std::strcmp(id, other) == 0) {
				return "two ports or parameters share an id";
			}
		}
	}
	for (size_t i = 0; i < m.port_count; ++i) {
		const PortDecl& p = m.ports[i];
		if (p.direction == Direction::in) {
			if (p.multiple < 1 || p.multiple > BlockLimits::kMultiple) {
				return "an input's multiple is not 1..32";
			}
			if (p.multiple > 1) {
				uint8_t allowed = p.type == PortType::boolean
					? (aggregation_bit(Aggregation::any) | aggregation_bit(Aggregation::all))
					: (p.type == PortType::enumeration ? 0
														: (aggregation_bit(Aggregation::mean) | aggregation_bit(Aggregation::min) |
															aggregation_bit(Aggregation::max)));
				if (p.aggregations == 0 || (p.aggregations & ~allowed) != 0 ||
					(p.aggregations & aggregation_bit(p.default_aggregation)) == 0) {
					return "an aggregated input's aggregations do not fit its type";
				}
			}
		} else if (!p.diagnostic && !std::isfinite(p.failsafe)) {
			return "an output's failsafe value is not finite";
		}
	}
	size_t record = 4 + 1 + 1 + length + 2 + 1 + BlockLimits::kIdLength + 1 + 4;
	for (size_t i = 0; i < m.state_count; ++i) {
		if (!identifier(m.state[i].id)) {
			return "a state field id is not an identifier";
		}
		for (size_t k = 0; k < i; ++k) {
			if (std::strcmp(m.state[i].id, m.state[k].id) == 0) {
				return "two state fields share an id";
			}
		}
		if (m.state[i].persist) {
			record += 1 + std::strlen(m.state[i].id) + 8;
		}
	}
	if (record > BlockLimits::kRecordBytes) {
		return "the persisted record would exceed 512 bytes";
	}
	for (size_t i = 0; i < m.timer_count; ++i) {
		if (!identifier(m.timers[i])) {
			return "a timer id is not an identifier";
		}
	}
	for (size_t i = 0; i < m.param_count; ++i) {
		const ParamDecl& p = m.params[i];
		if ((p.has_min && p.default_value < p.min) || (p.has_max && p.default_value > p.max) ||
			(p.has_min && p.has_max && p.min > p.max)) {
			return "a parameter's default is outside its range";
		}
		if (p.choice_count > 0 && p.type != ParamType::integer && p.type != ParamType::enumeration) {
			return "choices of a parameter that is neither integer nor enumeration";
		}
	}
	return nullptr;
}

BlockRegistry::BlockRegistry(bool builtins) {
	if (builtins) {
		add(RoomController::instance());
		add(HeatingDemand::instance());
		add(HeatingCurve::instance());
		add(SunShading::instance());
		add(HouseMode::instance());
	}
}

BlockRegistry& BlockRegistry::device() {
	static BlockRegistry registry(true);
	return registry;
}

bool BlockRegistry::add(const BlockType& type) {
	if (count_ == kCapacity || find(type.manifest().type, type.manifest().major) != nullptr) {
		return false;
	}
	types_[count_++] = &type;
	return true;
}

bool BlockRegistry::remove(const BlockType& type) {
	for (size_t i = 0; i < count_; ++i) {
		if (types_[i] == &type) {
			for (size_t k = i + 1; k < count_; ++k) {
				types_[k - 1] = types_[k];
			}
			types_[--count_] = nullptr;
			return true;
		}
	}
	return false;
}

const BlockType* BlockRegistry::find(const char* type, uint16_t major) const {
	for (size_t i = 0; i < count_; ++i) {
		const Manifest& m = types_[i]->manifest();
		if (m.major == major && std::strcmp(m.type, type) == 0) {
			return types_[i];
		}
	}
	return nullptr;
}

const BlockType* BlockRegistry::find_any(const char* type) const {
	for (size_t i = 0; i < count_; ++i) {
		if (std::strcmp(types_[i]->manifest().type, type) == 0) {
			return types_[i];
		}
	}
	return nullptr;
}

} // namespace hc::blocks
