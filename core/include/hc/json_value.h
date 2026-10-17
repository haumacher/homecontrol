/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_JSON_VALUE_H
#define HC_JSON_VALUE_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hc {

/// The kind of a JSON value (RFC 8259).
enum class JsonKind : uint8_t { null, boolean, number, string, array, object };

class JsonDocument;

/// A read-only view of one value inside a parsed JSON document: a handle (the document plus two
/// opaque words the document interprets), copied by value, never allocating. Whatever parses the
/// JSON (the image loader of issue #8, a span parser over the received text, the tests' tree
/// reader) implements JsonDocument; the consumers (the rule loader) navigate through JsonValue.
/// An invalid handle (default-constructed) behaves as JSON null.
class JsonValue {
public:
	JsonValue() = default;
	JsonValue(const JsonDocument* document, const void* node, size_t aux = 0)
		: document_(document), node_(node), aux_(aux) {}

	bool valid() const { return document_ != nullptr; }
	inline JsonKind kind() const;
	bool is(JsonKind k) const { return kind() == k; }

	/// The value of a boolean; false if it is not one.
	inline bool get_bool(bool& out) const;
	/// The value of a number (finite); false if it is not one.
	inline bool get_number(double& out) const;
	/// Copies a string (unescaped UTF-8, NUL-terminated) into `out`. False if it is not a string,
	/// contains a NUL or does not fit `size` (then `out` is empty); `length` receives the byte length.
	inline bool get_string(char* out, size_t size, size_t* length = nullptr) const;

	/// Iterates the items of an array: start with `position = 0`; false at the end (or if this is
	/// not an array).
	inline bool next_item(size_t& position, JsonValue& item) const;
	/// Iterates the members of an object in document order: the name is copied into `name`
	/// (NUL-terminated; `name_length` receives its full byte length, which may exceed the buffer: the
	/// copy is then cut). False at the end (or if this is not an object).
	inline bool next_member(size_t& position, char* name, size_t name_size, size_t& name_length,
		JsonValue& value) const;
	/// The member `name` of an object; false if absent or this is not an object.
	inline bool member(const char* name, JsonValue& out) const;

	/// Number of items of an array or members of an object (0 otherwise).
	size_t size() const {
		size_t count = 0;
		size_t position = 0;
		JsonValue item;
		if (kind() == JsonKind::array) {
			while (next_item(position, item)) {
				++count;
			}
		} else if (kind() == JsonKind::object) {
			char name[1];
			size_t length = 0;
			while (next_member(position, name, sizeof(name), length, item)) {
				++count;
			}
		}
		return count;
	}

	const JsonDocument* document() const { return document_; }
	const void* node() const { return node_; }
	size_t aux() const { return aux_; }

private:
	const JsonDocument* document_ = nullptr;
	const void* node_ = nullptr;
	size_t aux_ = 0;
};

/// The parser side of JsonValue: answers the questions about the values of one document. The
/// handles it gets are those it created (node and aux are its own).
class JsonDocument {
public:
	virtual JsonKind kind(const JsonValue& value) const = 0;
	virtual bool get_bool(const JsonValue& value, bool& out) const = 0;
	virtual bool get_number(const JsonValue& value, double& out) const = 0;
	virtual bool get_string(const JsonValue& value, char* out, size_t size, size_t* length) const = 0;
	virtual bool next_item(const JsonValue& array, size_t& position, JsonValue& item) const = 0;
	virtual bool next_member(const JsonValue& object, size_t& position, char* name, size_t name_size,
		size_t& name_length, JsonValue& value) const = 0;

	/// Default: a linear search with next_member().
	virtual bool member(const JsonValue& object, const char* name, JsonValue& out) const {
		char buffer[64];
		size_t position = 0;
		size_t length = 0;
		size_t wanted = std::strlen(name);
		JsonValue value;
		while (next_member(object, position, buffer, sizeof(buffer), length, value)) {
			if (length == wanted && length < sizeof(buffer) && std::strcmp(buffer, name) == 0) {
				out = value;
				return true;
			}
		}
		return false;
	}

protected:
	~JsonDocument() = default;
};

inline JsonKind JsonValue::kind() const { return document_ ? document_->kind(*this) : JsonKind::null; }

inline bool JsonValue::get_bool(bool& out) const { return document_ && document_->get_bool(*this, out); }

inline bool JsonValue::get_number(double& out) const { return document_ && document_->get_number(*this, out); }

inline bool JsonValue::get_string(char* out, size_t size, size_t* length) const {
	if (document_ == nullptr) {
		if (size > 0) {
			out[0] = '\0';
		}
		return false;
	}
	return document_->get_string(*this, out, size, length);
}

inline bool JsonValue::next_item(size_t& position, JsonValue& item) const {
	return document_ && document_->next_item(*this, position, item);
}

inline bool JsonValue::next_member(size_t& position, char* name, size_t name_size, size_t& name_length,
	JsonValue& value) const {
	return document_ && document_->next_member(*this, position, name, name_size, name_length, value);
}

inline bool JsonValue::member(const char* name, JsonValue& out) const {
	return document_ && document_->member(*this, name, out);
}

} // namespace hc

#endif // HC_JSON_VALUE_H
