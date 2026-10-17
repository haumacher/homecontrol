/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hc {

// ---- Writer ----------------------------------------------------------------------------------

JsonWriter::JsonWriter(char* buffer, size_t size) : buffer_(buffer), size_(size) {
	if (size_ > 0) {
		buffer_[0] = '\0';
	} else {
		overflow_ = true;
	}
}

void JsonWriter::put(char c) {
	if (overflow_) {
		return;
	}
	if (length_ + 1 >= size_) {
		overflow_ = true;
		return;
	}
	buffer_[length_++] = c;
	buffer_[length_] = '\0';
}

void JsonWriter::put(const char* text, size_t length) {
	if (overflow_) {
		return;
	}
	if (length_ + length >= size_) {
		overflow_ = true;
		return;
	}
	std::memcpy(buffer_ + length_, text, length);
	length_ += length;
	buffer_[length_] = '\0';
}

void JsonWriter::separate() {
	if (after_key_) {
		after_key_ = false;
		return;
	}
	if (need_comma_) {
		put(',');
	}
}

JsonWriter& JsonWriter::begin_object() {
	separate();
	put('{');
	need_comma_ = false;
	++depth_;
	return *this;
}

JsonWriter& JsonWriter::end_object() {
	put('}');
	need_comma_ = true;
	if (depth_ > 0) {
		--depth_;
	}
	return *this;
}

JsonWriter& JsonWriter::begin_array() {
	separate();
	put('[');
	need_comma_ = false;
	++depth_;
	return *this;
}

JsonWriter& JsonWriter::end_array() {
	put(']');
	need_comma_ = true;
	if (depth_ > 0) {
		--depth_;
	}
	return *this;
}

JsonWriter& JsonWriter::key(const char* name) {
	string(name);
	put(':');
	after_key_ = true;
	return *this;
}

JsonWriter& JsonWriter::string(const char* text) {
	return string(text != nullptr ? text : "", text != nullptr ? std::strlen(text) : 0);
}

namespace {

/// Length of the valid UTF-8 sequence at `p` (at most `available` bytes), 0 if invalid.
size_t utf8_sequence(const unsigned char* p, size_t available) {
	unsigned char c = p[0];
	size_t length;
	uint32_t min;
	uint32_t code;
	if (c < 0x80) {
		return 1;
	} else if ((c & 0xE0) == 0xC0) {
		length = 2;
		min = 0x80;
		code = c & 0x1F;
	} else if ((c & 0xF0) == 0xE0) {
		length = 3;
		min = 0x800;
		code = c & 0x0F;
	} else if ((c & 0xF8) == 0xF0) {
		length = 4;
		min = 0x10000;
		code = c & 0x07;
	} else {
		return 0;
	}
	if (length > available) {
		return 0;
	}
	for (size_t i = 1; i < length; ++i) {
		if ((p[i] & 0xC0) != 0x80) {
			return 0;
		}
		code = (code << 6) | (p[i] & 0x3F);
	}
	if (code < min || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
		return 0;
	}
	return length;
}

} // namespace

JsonWriter& JsonWriter::string(const char* text, size_t length) {
	separate();
	put('"');
	const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
	size_t i = 0;
	while (i < length && !overflow_) {
		unsigned char c = p[i];
		if (c == '"' || c == '\\') {
			put('\\');
			put(static_cast<char>(c));
			++i;
		} else if (c == '\n') {
			put("\\n", 2);
			++i;
		} else if (c == '\r') {
			put("\\r", 2);
			++i;
		} else if (c == '\t') {
			put("\\t", 2);
			++i;
		} else if (c < 0x20 || c == 0x7F) {
			char escape[8];
			std::snprintf(escape, sizeof(escape), "\\u%04x", c);
			put(escape, 6);
			++i;
		} else if (c < 0x80) {
			put(static_cast<char>(c));
			++i;
		} else {
			size_t n = utf8_sequence(p + i, length - i);
			if (n == 0) {
				put("\xEF\xBF\xBD", 3); // U+FFFD
				++i;
			} else {
				put(text + i, n);
				i += n;
			}
		}
	}
	put('"');
	need_comma_ = true;
	return *this;
}

JsonWriter& JsonWriter::number(double value) {
	if (!std::isfinite(value)) {
		return null();
	}
	separate();
	char text[32];
	// The shortest of %.15g and %.17g that reads back exactly.
	int n = std::snprintf(text, sizeof(text), "%.15g", value);
	if (std::strtod(text, nullptr) != value) {
		n = std::snprintf(text, sizeof(text), "%.17g", value);
	}
	put(text, static_cast<size_t>(n));
	need_comma_ = true;
	return *this;
}

JsonWriter& JsonWriter::number(int64_t value) {
	separate();
	char text[24];
	int n = std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
	put(text, static_cast<size_t>(n));
	need_comma_ = true;
	return *this;
}

JsonWriter& JsonWriter::number(uint64_t value) {
	separate();
	char text[24];
	int n = std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
	put(text, static_cast<size_t>(n));
	need_comma_ = true;
	return *this;
}

JsonWriter& JsonWriter::boolean(bool value) {
	separate();
	if (value) {
		put("true", 4);
	} else {
		put("false", 5);
	}
	need_comma_ = true;
	return *this;
}

JsonWriter& JsonWriter::null() {
	separate();
	put("null", 4);
	need_comma_ = true;
	return *this;
}

JsonWriter& JsonWriter::raw(const char* text, size_t length) {
	separate();
	put(text, length);
	need_comma_ = true;
	return *this;
}

void JsonWriter::reserve(size_t bytes) {
	release();
	if (bytes >= size_) {
		bytes = size_ > 0 ? size_ - 1 : 0;
	}
	reserved_ = bytes;
	size_ -= bytes;
}

void JsonWriter::release() {
	size_ += reserved_;
	reserved_ = 0;
}

JsonWriter::Mark JsonWriter::mark() const {
	return Mark{length_, depth_, need_comma_};
}

void JsonWriter::reset(const Mark& mark) {
	length_ = mark.length;
	depth_ = mark.depth;
	need_comma_ = mark.need_comma;
	after_key_ = false;
	overflow_ = false;
	if (size_ > 0) {
		buffer_[length_] = '\0';
	}
}

// ---- Reader ----------------------------------------------------------------------------------

namespace {

constexpr int kMaxNesting = 16;

struct Parser {
	const char* text;
	size_t length;
	size_t pos;
	const char* error;

	bool fail(const char* what) {
		if (error == nullptr) {
			error = what;
		}
		return false;
	}

	void space() {
		while (pos < length && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
			++pos;
		}
	}

	bool literal(const char* word) {
		size_t n = std::strlen(word);
		if (length - pos < n || std::memcmp(text + pos, word, n) != 0) {
			return fail("invalid literal");
		}
		pos += n;
		return true;
	}

	static int hex(char c) {
		if (c >= '0' && c <= '9') {
			return c - '0';
		}
		if (c >= 'a' && c <= 'f') {
			return c - 'a' + 10;
		}
		if (c >= 'A' && c <= 'F') {
			return c - 'A' + 10;
		}
		return -1;
	}

	bool string() {
		++pos; // opening quote
		while (pos < length) {
			unsigned char c = static_cast<unsigned char>(text[pos]);
			if (c == '"') {
				++pos;
				return true;
			}
			if (c < 0x20) {
				return fail("control character in a string");
			}
			if (c == '\\') {
				++pos;
				if (pos >= length) {
					break;
				}
				char e = text[pos];
				if (e == 'u') {
					if (length - pos < 5) {
						return fail("short \\u escape");
					}
					for (int i = 1; i <= 4; ++i) {
						if (hex(text[pos + i]) < 0) {
							return fail("invalid \\u escape");
						}
					}
					pos += 5;
					continue;
				}
				if (std::strchr("\"\\/bfnrt", e) == nullptr || e == '\0') {
					return fail("invalid escape");
				}
				++pos;
				continue;
			}
			if (c >= 0x80) {
				size_t n = utf8_sequence(reinterpret_cast<const unsigned char*>(text + pos), length - pos);
				if (n == 0) {
					return fail("invalid UTF-8 in a string");
				}
				pos += n;
				continue;
			}
			++pos;
		}
		return fail("unterminated string");
	}

	bool digits() {
		size_t start = pos;
		while (pos < length && text[pos] >= '0' && text[pos] <= '9') {
			++pos;
		}
		return pos > start;
	}

	bool number() {
		if (text[pos] == '-') {
			++pos;
		}
		if (pos < length && text[pos] == '0') {
			++pos;
		} else if (!digits()) {
			return fail("invalid number");
		}
		if (pos < length && text[pos] == '.') {
			++pos;
			if (!digits()) {
				return fail("invalid number");
			}
		}
		if (pos < length && (text[pos] == 'e' || text[pos] == 'E')) {
			++pos;
			if (pos < length && (text[pos] == '+' || text[pos] == '-')) {
				++pos;
			}
			if (!digits()) {
				return fail("invalid number");
			}
		}
		return true;
	}

	bool value(int depth, JsonSpan& out) {
		space();
		if (pos >= length) {
			return fail("value expected");
		}
		size_t start = pos;
		char c = text[pos];
		JsonSpan::Kind kind;
		bool ok;
		if (c == '{') {
			kind = JsonSpan::Kind::object;
			ok = container(depth, '}', true);
		} else if (c == '[') {
			kind = JsonSpan::Kind::array;
			ok = container(depth, ']', false);
		} else if (c == '"') {
			kind = JsonSpan::Kind::string;
			ok = string();
		} else if (c == 't') {
			kind = JsonSpan::Kind::boolean;
			ok = literal("true");
		} else if (c == 'f') {
			kind = JsonSpan::Kind::boolean;
			ok = literal("false");
		} else if (c == 'n') {
			kind = JsonSpan::Kind::null;
			ok = literal("null");
		} else if (c == '-' || (c >= '0' && c <= '9')) {
			kind = JsonSpan::Kind::number;
			ok = number();
		} else {
			return fail("unexpected character");
		}
		if (!ok) {
			return false;
		}
		out.data = text + start;
		out.length = pos - start;
		out.kind = kind;
		return true;
	}

	bool container(int depth, char close, bool object) {
		if (depth >= kMaxNesting) {
			return fail("nested too deeply");
		}
		++pos;
		space();
		if (pos < length && text[pos] == close) {
			++pos;
			return true;
		}
		for (;;) {
			JsonSpan item;
			if (object) {
				space();
				if (pos >= length || text[pos] != '"') {
					return fail("member name expected");
				}
				if (!string()) {
					return false;
				}
				space();
				if (pos >= length || text[pos] != ':') {
					return fail("':' expected");
				}
				++pos;
			}
			if (!value(depth + 1, item)) {
				return false;
			}
			space();
			if (pos < length && text[pos] == ',') {
				++pos;
				continue;
			}
			if (pos < length && text[pos] == close) {
				++pos;
				return true;
			}
			return fail(object ? "',' or '}' expected" : "',' or ']' expected");
		}
	}
};

} // namespace

bool json_parse(const char* text, size_t length, JsonSpan& out, const char** error) {
	Parser parser{text, length, 0, nullptr};
	out = JsonSpan{};
	JsonSpan value;
	bool ok = text != nullptr && parser.value(0, value);
	if (ok) {
		parser.space();
		if (parser.pos != length) {
			ok = parser.fail("trailing characters");
		}
	} else if (text == nullptr) {
		parser.fail("no text");
	}
	if (!ok) {
		if (error != nullptr) {
			*error = parser.error;
		}
		return false;
	}
	out = value;
	return true;
}

namespace {

/// Iterates the elements of an already validated container span.
bool next_element(const JsonSpan& container, size_t& position, bool object, JsonSpan* name, JsonSpan& value) {
	Parser parser{container.data, container.length, position == 0 ? 1 : position, nullptr};
	parser.space();
	if (parser.pos >= parser.length || parser.text[parser.pos] == (object ? '}' : ']')) {
		return false;
	}
	if (parser.text[parser.pos] == ',') {
		++parser.pos;
	}
	if (object) {
		parser.space();
		size_t start = parser.pos;
		if (!parser.string()) {
			return false;
		}
		if (name != nullptr) {
			*name = JsonSpan{parser.text + start, parser.pos - start, JsonSpan::Kind::string};
		}
		parser.space();
		++parser.pos; // ':'
	}
	if (!parser.value(1, value)) {
		return false;
	}
	position = parser.pos;
	return true;
}

} // namespace

bool json_next_member(const JsonSpan& object, size_t& position, JsonSpan& name, JsonSpan& value) {
	if (object.kind != JsonSpan::Kind::object) {
		return false;
	}
	return next_element(object, position, true, &name, value);
}

bool json_next_item(const JsonSpan& array, size_t& position, JsonSpan& item) {
	if (array.kind != JsonSpan::Kind::array) {
		return false;
	}
	return next_element(array, position, false, nullptr, item);
}

bool json_member(const JsonSpan& object, const char* name, JsonSpan& out) {
	size_t position = 0;
	JsonSpan key;
	JsonSpan value;
	while (json_next_member(object, position, key, value)) {
		if (json_string_equals(key, name)) {
			out = value;
			return true;
		}
	}
	return false;
}

namespace {

void put_utf8(char* out, size_t& n, uint32_t code) {
	if (code < 0x80) {
		out[n++] = static_cast<char>(code);
	} else if (code < 0x800) {
		out[n++] = static_cast<char>(0xC0 | (code >> 6));
		out[n++] = static_cast<char>(0x80 | (code & 0x3F));
	} else if (code < 0x10000) {
		out[n++] = static_cast<char>(0xE0 | (code >> 12));
		out[n++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
		out[n++] = static_cast<char>(0x80 | (code & 0x3F));
	} else {
		out[n++] = static_cast<char>(0xF0 | (code >> 18));
		out[n++] = static_cast<char>(0x80 | ((code >> 12) & 0x3F));
		out[n++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
		out[n++] = static_cast<char>(0x80 | (code & 0x3F));
	}
}

uint32_t read_hex4(const char* p) {
	uint32_t v = 0;
	for (int i = 0; i < 4; ++i) {
		v = (v << 4) | static_cast<uint32_t>(Parser::hex(p[i]));
	}
	return v;
}

} // namespace

bool json_string(const JsonSpan& span, char* out, size_t size, size_t* length) {
	if (span.kind != JsonSpan::Kind::string || size == 0) {
		return false;
	}
	out[0] = '\0';
	size_t n = 0;
	const char* p = span.data + 1;
	const char* end = span.data + span.length - 1;
	while (p < end) {
		char c = *p;
		char single = 0;
		uint32_t code = 0;
		bool unicode = false;
		if (c == '\\') {
			char e = p[1];
			p += 2;
			switch (e) {
			case 'b': single = '\b'; break;
			case 'f': single = '\f'; break;
			case 'n': single = '\n'; break;
			case 'r': single = '\r'; break;
			case 't': single = '\t'; break;
			case 'u': {
				code = read_hex4(p);
				p += 4;
				if (code >= 0xD800 && code <= 0xDBFF) {
					if (end - p < 6 || p[0] != '\\' || p[1] != 'u') {
						return false;
					}
					uint32_t low = read_hex4(p + 2);
					if (low < 0xDC00 || low > 0xDFFF) {
						return false;
					}
					p += 6;
					code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
				} else if (code >= 0xDC00 && code <= 0xDFFF) {
					return false;
				}
				if (code == 0) {
					return false;
				}
				unicode = true;
				break;
			}
			default: single = e; break;
			}
		} else {
			single = c;
			++p;
		}
		if (unicode) {
			if (n + 4 >= size) {
				out[0] = '\0';
				return false;
			}
			put_utf8(out, n, code);
		} else {
			if (n + 1 >= size) {
				out[0] = '\0';
				return false;
			}
			out[n++] = single;
		}
	}
	out[n] = '\0';
	if (length != nullptr) {
		*length = n;
	}
	return true;
}

bool json_string_equals(const JsonSpan& span, const char* text) {
	if (span.kind != JsonSpan::Kind::string) {
		return false;
	}
	size_t want = std::strlen(text);
	// Fast path: no escapes.
	if (span.length == want + 2 && std::memchr(span.data, '\\', span.length) == nullptr) {
		return std::memcmp(span.data + 1, text, want) == 0;
	}
	char buffer[128];
	size_t n = 0;
	return want < sizeof(buffer) && json_string(span, buffer, sizeof(buffer), &n) && n == want &&
		std::memcmp(buffer, text, n) == 0;
}

bool json_number(const JsonSpan& span, double& out) {
	if (span.kind != JsonSpan::Kind::number || span.length >= 64) {
		return false;
	}
	char buffer[64];
	std::memcpy(buffer, span.data, span.length);
	buffer[span.length] = '\0';
	double value = std::strtod(buffer, nullptr);
	if (!std::isfinite(value)) {
		return false;
	}
	out = value;
	return true;
}

bool json_bool(const JsonSpan& span, bool& out) {
	if (span.kind != JsonSpan::Kind::boolean) {
		return false;
	}
	out = span.data[0] == 't';
	return true;
}

} // namespace hc
