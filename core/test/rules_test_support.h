/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_TEST_RULES_TEST_SUPPORT_H
#define HC_TEST_RULES_TEST_SUPPORT_H

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "hc/json_value.h"
#include "hc/rules/value.h"
#include "hc/technology.h"
#include "json_reader.h"

#ifndef HC_SPEC_DIR
#error "HC_SPEC_DIR must name the spec directory"
#endif

namespace hc_test {

/// hc::JsonDocument over the tests' tree reader: a handle's node is a `const Json*`.
class JsonTree : public hc::JsonDocument {
public:
	hc::JsonValue value(const Json& json) const { return hc::JsonValue(this, &json); }

	hc::JsonKind kind(const hc::JsonValue& v) const override {
		switch (node(v).kind) {
			case Json::Kind::null: return hc::JsonKind::null;
			case Json::Kind::boolean: return hc::JsonKind::boolean;
			case Json::Kind::number: return hc::JsonKind::number;
			case Json::Kind::string: return hc::JsonKind::string;
			case Json::Kind::array: return hc::JsonKind::array;
			case Json::Kind::object: return hc::JsonKind::object;
		}
		return hc::JsonKind::null;
	}

	bool get_bool(const hc::JsonValue& v, bool& out) const override {
		if (node(v).kind != Json::Kind::boolean) {
			return false;
		}
		out = node(v).boolean;
		return true;
	}

	bool get_number(const hc::JsonValue& v, double& out) const override {
		if (node(v).kind != Json::Kind::number || !std::isfinite(node(v).number)) {
			return false;
		}
		out = node(v).number;
		return true;
	}

	bool get_string(const hc::JsonValue& v, char* out, size_t size, size_t* length) const override {
		if (size > 0) {
			out[0] = '\0';
		}
		const Json& j = node(v);
		if (j.kind != Json::Kind::string) {
			return false;
		}
		if (length) {
			*length = j.text.size();
		}
		if (j.text.size() + 1 > size || std::memchr(j.text.data(), '\0', j.text.size()) != nullptr) {
			return false;
		}
		std::memcpy(out, j.text.c_str(), j.text.size() + 1);
		return true;
	}

	bool next_item(const hc::JsonValue& array, size_t& position, hc::JsonValue& item) const override {
		const Json& j = node(array);
		if (j.kind != Json::Kind::array || position >= j.items.size()) {
			return false;
		}
		item = value(j.items[position++]);
		return true;
	}

	bool next_member(const hc::JsonValue& object, size_t& position, char* name, size_t name_size,
		size_t& name_length, hc::JsonValue& out) const override {
		const Json& j = node(object);
		if (j.kind != Json::Kind::object || position >= j.members.size()) {
			return false;
		}
		auto it = j.members.begin();
		std::advance(it, static_cast<long>(position++));
		name_length = it->first.size();
		if (name_size > 0) {
			size_t n = name_length < name_size - 1 ? name_length : name_size - 1;
			std::memcpy(name, it->first.data(), n);
			name[n] = '\0';
		}
		out = value(it->second);
		return true;
	}

	bool member(const hc::JsonValue& object, const char* name, hc::JsonValue& out) const override {
		const Json& j = node(object);
		if (j.kind != Json::Kind::object) {
			return false;
		}
		auto it = j.members.find(name);
		if (it == j.members.end()) {
			return false;
		}
		out = value(it->second);
		return true;
	}

private:
	static const Json& node(const hc::JsonValue& v) { return *static_cast<const Json*>(v.node()); }
};

inline bool read_text(const std::string& path, std::string& out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	out = buffer.str();
	return true;
}

/// Parses JSON text; an empty (null) tree and a message on error.
inline bool parse_json(const std::string& text, Json& out, std::string& error) {
	JsonReader reader(text);
	return reader.parse(out, error);
}

/// The rule corpus files (spec/fixtures/rules/NN-*.json), sorted.
std::vector<std::string> rule_corpus_files();

/// A technology that accepts every command (without reporting it back), or refuses all.
class TestTechnology : public hc::Technology {
public:
	const char* name() const override { return "test"; }
	bool command(const hc::Datapoint& datapoint, const hc::Value& value) override {
		(void)value;
		if (refuse) {
			return false;
		}
		(void)datapoint;
		return true;
	}

	bool refuse = false;
};

} // namespace hc_test

#endif // HC_TEST_RULES_TEST_SUPPORT_H
