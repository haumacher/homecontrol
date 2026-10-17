/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/json_document.h"

#include <cstring>

namespace hc {

namespace {

JsonSpan::Kind kind_of(const char* data, size_t length) {
	if (data == nullptr || length == 0) {
		return JsonSpan::Kind::invalid;
	}
	switch (data[0]) {
		case '{': return JsonSpan::Kind::object;
		case '[': return JsonSpan::Kind::array;
		case '"': return JsonSpan::Kind::string;
		case 't':
		case 'f': return JsonSpan::Kind::boolean;
		case 'n': return JsonSpan::Kind::null;
		default: return JsonSpan::Kind::number;
	}
}

} // namespace

JsonSpan JsonSpanDocument::span(const JsonValue& value) {
	JsonSpan out;
	out.data = static_cast<const char*>(value.node());
	out.length = value.aux();
	out.kind = kind_of(out.data, out.length);
	return out;
}

JsonKind JsonSpanDocument::kind(const JsonValue& value) const {
	switch (span(value).kind) {
		case JsonSpan::Kind::boolean: return JsonKind::boolean;
		case JsonSpan::Kind::number: return JsonKind::number;
		case JsonSpan::Kind::string: return JsonKind::string;
		case JsonSpan::Kind::array: return JsonKind::array;
		case JsonSpan::Kind::object: return JsonKind::object;
		default: return JsonKind::null;
	}
}

bool JsonSpanDocument::get_bool(const JsonValue& value, bool& out) const {
	return json_bool(span(value), out);
}

bool JsonSpanDocument::get_number(const JsonValue& value, double& out) const {
	return json_number(span(value), out);
}

bool JsonSpanDocument::get_string(const JsonValue& value, char* out, size_t size, size_t* length) const {
	size_t n = 0;
	bool ok = json_string(span(value), out, size, &n);
	if (!ok && size > 0) {
		out[0] = '\0';
	}
	if (length != nullptr) {
		*length = ok ? n : 0;
	}
	return ok;
}

bool JsonSpanDocument::next_item(const JsonValue& array, size_t& position, JsonValue& item) const {
	JsonSpan element;
	if (!json_next_item(span(array), position, element)) {
		return false;
	}
	item = value(element);
	return true;
}

bool JsonSpanDocument::next_member(const JsonValue& object, size_t& position, char* name, size_t name_size,
	size_t& name_length, JsonValue& out) const {
	JsonSpan key;
	JsonSpan element;
	if (!json_next_member(span(object), position, key, element)) {
		return false;
	}
	size_t n = 0;
	if (json_string(key, name, name_size, &n)) {
		name_length = n;
	} else {
		// Longer than the buffer (or not representable): the consumers compare the full length, so
		// report the escaped length (at least the buffer's size) and a cut copy.
		name_length = key.length >= 2 ? key.length - 2 : 0;
		if (name_length < name_size) {
			name_length = name_size;
		}
		if (name_size > 0) {
			size_t copy = name_size - 1;
			if (copy > key.length - 2) {
				copy = key.length - 2;
			}
			std::memcpy(name, key.data + 1, copy);
			name[copy] = '\0';
		}
	}
	out = value(element);
	return true;
}

bool JsonSpanDocument::member(const JsonValue& object, const char* name, JsonValue& out) const {
	JsonSpan element;
	if (!json_member(span(object), name, element)) {
		return false;
	}
	out = value(element);
	return true;
}

} // namespace hc
