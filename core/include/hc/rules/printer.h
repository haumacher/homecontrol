/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_RULES_PRINTER_H
#define HC_RULES_PRINTER_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/rules/rule_set.h"

namespace hc::rules {

/// Writes the canonical text (spec/rules.md 4.8) of the expression rooted at `root` of rule `rule`:
/// `@hall.presence and (@hall.light or @hall.lux < $darkBelow)`. Datapoint names come from `model`
/// (the one the set was loaded against). Returns the full length (snprintf convention: at most
/// `size - 1` bytes are written plus a NUL). For the diagnostics and the API; E-Home has its own.
size_t print_expression(const RuleSet& set, const DatapointModel& model, size_t rule, uint16_t root, char* out,
	size_t size);

/// Whether `name` can be written as a bare datapoint reference (`@a.b`) rather than `@"..."`.
bool bare_datapoint_name(const char* name);

/// Whether the parameter id can be written as a bare parameter reference (`$timeout`, an identifier
/// that is not a keyword) rather than `$"max-helligkeit"` (spec/rules.md 4.3, issue #81).
bool bare_parameter_id(const char* id);

} // namespace hc::rules

#endif // HC_RULES_PRINTER_H
