/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_JSON_H
#define HC_JSON_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// Writes JSON (RFC 8259) into a caller's buffer, never touching the heap. Commas are inserted
/// automatically; strings are escaped (control characters as \u00XX, invalid UTF-8 bytes replaced
/// by U+FFFD). When the buffer runs out the writer stops and overflowed() turns true: the output
/// is then incomplete and must not be sent (the caller refuses or pages).
class JsonWriter {
public:
	JsonWriter(char* buffer, size_t size);

	JsonWriter& begin_object();
	JsonWriter& end_object();
	JsonWriter& begin_array();
	JsonWriter& end_array();
	/// A member name; the value follows with the next call.
	JsonWriter& key(const char* name);

	JsonWriter& string(const char* text);
	JsonWriter& string(const char* text, size_t length);
	JsonWriter& number(double value);
	JsonWriter& number(int64_t value);
	JsonWriter& number(uint64_t value);
	JsonWriter& number(int32_t value) { return number(static_cast<int64_t>(value)); }
	JsonWriter& number(uint32_t value) { return number(static_cast<uint64_t>(value)); }
	JsonWriter& boolean(bool value);
	JsonWriter& null();

	/// Shorthands for a member.
	JsonWriter& member(const char* name, const char* text) { return key(name).string(text); }
	JsonWriter& member(const char* name, bool value) { return key(name).boolean(value); }
	JsonWriter& member(const char* name, uint64_t value) { return key(name).number(value); }
	JsonWriter& member(const char* name, uint32_t value) { return key(name).number(value); }
	JsonWriter& member(const char* name, int64_t value) { return key(name).number(value); }
	JsonWriter& member(const char* name, int32_t value) { return key(name).number(value); }
	JsonWriter& member(const char* name, double value) { return key(name).number(value); }

	/// Inserts `text` as a value verbatim (it must be valid JSON).
	JsonWriter& raw(const char* text, size_t length);

	/// Holds back `bytes` of the buffer (for the closing of a paged response); release() returns
	/// them.
	void reserve(size_t bytes);
	void release();

	bool overflowed() const { return overflow_; }
	size_t length() const { return length_; }
	const char* data() const { return buffer_; }

	/// Mark and roll back: lets a pager drop the element that did not fit.
	struct Mark {
		size_t length;
		uint8_t depth;
		bool need_comma;
	};
	Mark mark() const;
	void reset(const Mark& mark);

private:
	static constexpr size_t kMaxDepth = 16;

	void separate();
	void put(char c);
	void put(const char* text, size_t length);

	char* buffer_;
	size_t size_;
	size_t reserved_ = 0;
	size_t length_ = 0;
	bool overflow_ = false;
	bool need_comma_ = false;
	uint8_t depth_ = 0;
	bool after_key_ = false;
};

/// A JSON value inside a text: where it starts and how long it is, and its kind.
struct JsonSpan {
	enum class Kind : uint8_t { invalid, null, boolean, number, string, array, object };

	const char* data = nullptr;
	size_t length = 0;
	Kind kind = Kind::invalid;

	bool valid() const { return kind != Kind::invalid; }
};

/// Default nesting limit of json_parse() (the API's request bodies).
constexpr size_t kJsonDefaultDepth = 16;

/// Largest nesting json_parse() accepts (the runtime image: rule expressions nest deeply).
constexpr size_t kJsonMaxDepth = 64;

/// Validates `text` as one JSON value (whitespace around it allowed), nesting at most `max_depth`
/// containers deep (capped at kJsonMaxDepth); on success `out` spans the value. On refusal `error`
/// names the problem and `error_offset` (if given) the byte offset where it was found. The
/// validation is iterative: its stack use does not grow with the nesting.
bool json_parse(const char* text, size_t length, JsonSpan& out, const char** error = nullptr,
	size_t max_depth = kJsonDefaultDepth, size_t* error_offset = nullptr);

/// The member `name` of an object span (the first one, if repeated); false if absent or `object`
/// is not an object.
bool json_member(const JsonSpan& object, const char* name, JsonSpan& out);

/// Iterates an object: start with `position = 0`; each call yields the next member's name span (a
/// string) and its value; false at the end.
bool json_next_member(const JsonSpan& object, size_t& position, JsonSpan& name, JsonSpan& value);

/// Iterates an array the same way.
bool json_next_item(const JsonSpan& array, size_t& position, JsonSpan& item);

/// Unescapes a string span into `out` (NUL-terminated, UTF-8). False if it is not a string, does
/// not fit `size`, contains a NUL or an invalid escape or surrogate.
bool json_string(const JsonSpan& span, char* out, size_t size, size_t* length = nullptr);

/// Whether a string span equals `text` after unescaping.
bool json_string_equals(const JsonSpan& span, const char* text);

/// A number span as double (false if not a number or not finite).
bool json_number(const JsonSpan& span, double& out);

/// A boolean span.
bool json_bool(const JsonSpan& span, bool& out);

} // namespace hc

#endif // HC_JSON_H
