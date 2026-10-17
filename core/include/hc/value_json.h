/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_VALUE_JSON_H
#define HC_VALUE_JSON_H

#include "hc/datapoint.h"
#include "hc/json.h"

namespace hc {

/// The JSON form of a datapoint value (spec/api.md, "Values"): boolean as true/false; unsigned,
/// signed, real and enumeration as a number; string as a string; raw as a string of hex digits;
/// time of day, date, date-time, controlled and color as objects of their fields; an empty value as
/// null.
void write_value_json(JsonWriter& json, const Value& value);

/// Reads the JSON form of a value of `type`. A number is accepted for every numeric type when it
/// converts exactly (Value::convert: 1.0 for an unsigned, never 1.5). On refusal returns an empty
/// optional and points `error` at the reason.
std::optional<Value> read_value_json(const JsonSpan& span, ValueType type, const char** error);

} // namespace hc

#endif // HC_VALUE_JSON_H
