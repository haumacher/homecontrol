/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_TEST_JSON_READER_H
#define HC_TEST_JSON_READER_H

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hc_test {

/// A parsed JSON value, for reading test fixtures (allocates freely; tests only).
struct Json {
	enum class Kind { null, boolean, number, string, array, object };

	Kind kind = Kind::null;
	bool boolean = false;
	double number = 0;
	std::string text; ///< The string (UTF-8), or the literal text of a number.
	std::vector<Json> items;
	std::map<std::string, Json> members;

	bool has(const std::string& key) const { return kind == Kind::object && members.count(key) > 0; }

	/// The member `key`; a null value if there is none.
	const Json& operator[](const std::string& key) const {
		static const Json kNull;
		auto it = members.find(key);
		return it == members.end() ? kNull : it->second;
	}
};

/// A strict recursive-descent JSON reader (RFC 8259). On a syntax error `error` names the offset.
class JsonReader {
public:
	explicit JsonReader(const std::string& text) : text_(text) {}

	bool parse(Json& out, std::string& error) {
		pos_ = 0;
		error_.clear();
		bool ok = value(out);
		skip_space();
		if (ok && pos_ != text_.size()) {
			fail("trailing characters");
			ok = false;
		}
		error = error_;
		return ok;
	}

private:
	bool fail(const char* what) {
		if (error_.empty()) {
			error_ = std::string(what) + " at offset " + std::to_string(pos_);
		}
		return false;
	}

	void skip_space() {
		while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
				text_[pos_] == '\r')) {
			++pos_;
		}
	}

	bool literal(const char* word) {
		size_t n = std::char_traits<char>::length(word);
		if (text_.compare(pos_, n, word) != 0) {
			return fail("invalid literal");
		}
		pos_ += n;
		return true;
	}

	bool value(Json& out) {
		skip_space();
		if (pos_ >= text_.size()) {
			return fail("unexpected end");
		}
		char c = text_[pos_];
		if (c == '{') {
			return object(out);
		}
		if (c == '[') {
			return array(out);
		}
		if (c == '"') {
			out.kind = Json::Kind::string;
			return string(out.text);
		}
		if (c == 't') {
			out.kind = Json::Kind::boolean;
			out.boolean = true;
			return literal("true");
		}
		if (c == 'f') {
			out.kind = Json::Kind::boolean;
			out.boolean = false;
			return literal("false");
		}
		if (c == 'n') {
			out.kind = Json::Kind::null;
			return literal("null");
		}
		return number(out);
	}

	bool object(Json& out) {
		out.kind = Json::Kind::object;
		++pos_;
		skip_space();
		if (pos_ < text_.size() && text_[pos_] == '}') {
			++pos_;
			return true;
		}
		for (;;) {
			skip_space();
			std::string key;
			if (pos_ >= text_.size() || text_[pos_] != '"' || !string(key)) {
				return fail("member name expected");
			}
			skip_space();
			if (pos_ >= text_.size() || text_[pos_] != ':') {
				return fail("':' expected");
			}
			++pos_;
			Json member;
			if (!value(member)) {
				return false;
			}
			if (out.members.count(key) > 0) {
				return fail("duplicate member");
			}
			out.members[key] = std::move(member);
			skip_space();
			if (pos_ < text_.size() && text_[pos_] == ',') {
				++pos_;
				continue;
			}
			if (pos_ < text_.size() && text_[pos_] == '}') {
				++pos_;
				return true;
			}
			return fail("',' or '}' expected");
		}
	}

	bool array(Json& out) {
		out.kind = Json::Kind::array;
		++pos_;
		skip_space();
		if (pos_ < text_.size() && text_[pos_] == ']') {
			++pos_;
			return true;
		}
		for (;;) {
			Json item;
			if (!value(item)) {
				return false;
			}
			out.items.push_back(std::move(item));
			skip_space();
			if (pos_ < text_.size() && text_[pos_] == ',') {
				++pos_;
				continue;
			}
			if (pos_ < text_.size() && text_[pos_] == ']') {
				++pos_;
				return true;
			}
			return fail("',' or ']' expected");
		}
	}

	static void append_utf8(std::string& out, uint32_t cp) {
		if (cp < 0x80) {
			out += static_cast<char>(cp);
		} else if (cp < 0x800) {
			out += static_cast<char>(0xC0 | cp >> 6);
			out += static_cast<char>(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out += static_cast<char>(0xE0 | cp >> 12);
			out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		} else {
			out += static_cast<char>(0xF0 | cp >> 18);
			out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
			out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}

	bool hex4(uint32_t& out) {
		if (pos_ + 4 > text_.size()) {
			return fail("short \\u escape");
		}
		out = 0;
		for (int i = 0; i < 4; ++i) {
			char c = text_[pos_++];
			out <<= 4;
			if (c >= '0' && c <= '9') {
				out |= static_cast<uint32_t>(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				out |= static_cast<uint32_t>(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				out |= static_cast<uint32_t>(c - 'A' + 10);
			} else {
				return fail("invalid \\u escape");
			}
		}
		return true;
	}

	bool string(std::string& out) {
		++pos_; // opening quote
		out.clear();
		while (pos_ < text_.size()) {
			char c = text_[pos_++];
			if (c == '"') {
				return true;
			}
			if (static_cast<unsigned char>(c) < 0x20) {
				return fail("control character in string");
			}
			if (c != '\\') {
				out += c;
				continue;
			}
			if (pos_ >= text_.size()) {
				break;
			}
			char e = text_[pos_++];
			switch (e) {
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u': {
					uint32_t cp = 0;
					if (!hex4(cp)) {
						return false;
					}
					if (cp >= 0xD800 && cp <= 0xDBFF) {
						uint32_t low = 0;
						if (text_.compare(pos_, 2, "\\u") != 0) {
							return fail("lone surrogate");
						}
						pos_ += 2;
						if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) {
							return fail("invalid surrogate pair");
						}
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					}
					append_utf8(out, cp);
					break;
				}
				default: return fail("invalid escape");
			}
		}
		return fail("unterminated string");
	}

	bool number(Json& out) {
		size_t start = pos_;
		if (pos_ < text_.size() && text_[pos_] == '-') {
			++pos_;
		}
		size_t digits = pos_;
		while (pos_ < text_.size() && ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.' ||
				text_[pos_] == 'e' || text_[pos_] == 'E' || text_[pos_] == '+' || text_[pos_] == '-')) {
			++pos_;
		}
		if (pos_ == digits) {
			return fail("value expected");
		}
		out.kind = Json::Kind::number;
		out.text = text_.substr(start, pos_ - start);
		char* end = nullptr;
		out.number = std::strtod(out.text.c_str(), &end);
		if (end != out.text.c_str() + out.text.size()) {
			return fail("invalid number");
		}
		return true;
	}

	const std::string& text_;
	size_t pos_ = 0;
	std::string error_;
};

} // namespace hc_test

#endif // HC_TEST_JSON_READER_H
