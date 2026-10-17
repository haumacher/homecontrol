/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_JSON_DOCUMENT_H
#define HC_JSON_DOCUMENT_H

#include <cstddef>

#include "hc/json.h"
#include "hc/json_value.h"

namespace hc {

/// The JsonDocument over a text validated by json_parse(): a JsonValue handle is the span's start
/// (node) and length (aux); the kind follows from the first character. Navigation re-scans the text
/// (json_next_member(), json_next_item()), nothing is allocated, nothing is copied: the text must
/// stay valid while handles are in use. The runtime image loader hands its `rules` member to the
/// rule loader through this (hc::rules::RuleSet::load()).
class JsonSpanDocument : public JsonDocument {
public:
	JsonSpanDocument() = default;

	/// The handle of a span of a validated text (an invalid span gives an invalid handle).
	JsonValue value(const JsonSpan& span) const {
		return span.valid() ? JsonValue(this, span.data, span.length) : JsonValue();
	}

	/// The span behind a handle of this document.
	static JsonSpan span(const JsonValue& value);

	JsonKind kind(const JsonValue& value) const override;
	bool get_bool(const JsonValue& value, bool& out) const override;
	bool get_number(const JsonValue& value, double& out) const override;
	bool get_string(const JsonValue& value, char* out, size_t size, size_t* length) const override;
	bool next_item(const JsonValue& array, size_t& position, JsonValue& item) const override;
	bool next_member(const JsonValue& object, size_t& position, char* name, size_t name_size, size_t& name_length,
		JsonValue& value) const override;
	bool member(const JsonValue& object, const char* name, JsonValue& out) const override;
};

} // namespace hc

#endif // HC_JSON_DOCUMENT_H
