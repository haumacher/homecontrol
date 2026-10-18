/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/update_check.h"

#include <cstdio>
#include <cstring>

#include "hc/log.h"
#include "hc/sha.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.update";

/// Largest image a manifest may announce (the largest application partition of any layout is
/// smaller).
constexpr uint32_t kMaxImageSize = 16u * 1024u * 1024u;

/// Nesting depth of unknown members the manifest parser skips.
constexpr int kMaxDepth = 8;

bool refuse(const char** error, const char* text) {
	if (error != nullptr) {
		*error = text;
	}
	return false;
}

bool is_digit(char c) {
	return c >= '0' && c <= '9';
}

bool is_hex(char c) {
	return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_identifier_char(char c) {
	return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

/// Parses a decimal number without leading zeros; advances `p`.
bool parse_number(const char*& p, uint32_t& out) {
	if (!is_digit(*p)) {
		return false;
	}
	if (*p == '0' && is_digit(p[1])) {
		return false;
	}
	uint64_t value = 0;
	while (is_digit(*p)) {
		value = value * 10 + static_cast<uint64_t>(*p - '0');
		if (value > 0xffffffffu) {
			return false;
		}
		++p;
	}
	out = static_cast<uint32_t>(value);
	return true;
}

/// Copies dot-separated identifiers from `p` up to `stop` (or the end) into `out`; advances `p`.
/// Numeric identifiers must not have leading zeros when `numeric_rule` is set (pre-release).
bool parse_identifiers(const char*& p, char stop, bool numeric_rule, char* out, size_t size) {
	const char* start = p;
	for (;;) {
		const char* id = p;
		bool all_digits = true;
		while (is_identifier_char(*p)) {
			all_digits = all_digits && is_digit(*p);
			++p;
		}
		size_t id_length = static_cast<size_t>(p - id);
		if (id_length == 0) {
			return false;
		}
		if (numeric_rule && all_digits && id_length > 1 && *id == '0') {
			return false;
		}
		if (*p != '.') {
			break;
		}
		++p;
	}
	if (*p != '\0' && *p != stop) {
		return false;
	}
	size_t length = static_cast<size_t>(p - start);
	if (length + 1 > size) {
		return false;
	}
	std::memcpy(out, start, length);
	out[length] = '\0';
	return true;
}

/// Compares two pre-release strings by semver's rules (both non-empty).
int compare_prerelease(const char* a, const char* b) {
	for (;;) {
		const char* a_end = a;
		while (*a_end != '\0' && *a_end != '.') {
			++a_end;
		}
		const char* b_end = b;
		while (*b_end != '\0' && *b_end != '.') {
			++b_end;
		}
		bool a_numeric = true;
		for (const char* q = a; q < a_end; ++q) {
			a_numeric = a_numeric && is_digit(*q);
		}
		bool b_numeric = true;
		for (const char* q = b; q < b_end; ++q) {
			b_numeric = b_numeric && is_digit(*q);
		}
		size_t a_length = static_cast<size_t>(a_end - a);
		size_t b_length = static_cast<size_t>(b_end - b);
		int result = 0;
		if (a_numeric && b_numeric) {
			// No leading zeros: the longer number is the larger one.
			if (a_length != b_length) {
				result = a_length < b_length ? -1 : 1;
			} else {
				result = std::strncmp(a, b, a_length);
			}
		} else if (a_numeric != b_numeric) {
			result = a_numeric ? -1 : 1; // numeric identifiers have lower precedence
		} else {
			size_t common = a_length < b_length ? a_length : b_length;
			result = std::strncmp(a, b, common);
			if (result == 0 && a_length != b_length) {
				result = a_length < b_length ? -1 : 1;
			}
		}
		if (result != 0) {
			return result < 0 ? -1 : 1;
		}
		bool a_more = *a_end == '.';
		bool b_more = *b_end == '.';
		if (!a_more || !b_more) {
			if (a_more == b_more) {
				return 0;
			}
			return a_more ? 1 : -1; // a larger set of identifiers has higher precedence
		}
		a = a_end + 1;
		b = b_end + 1;
	}
}

// ---- A minimal JSON reader for the flat manifest object --------------------------------------

class JsonReader {
public:
	JsonReader(const char* text, size_t length) : p_(text), end_(text + length) {}

	void skip_space() {
		while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) {
			++p_;
		}
	}

	bool at_end() const { return p_ >= end_; }
	char peek() const { return p_ < end_ ? *p_ : '\0'; }

	bool consume(char c) {
		skip_space();
		if (p_ < end_ && *p_ == c) {
			++p_;
			return true;
		}
		return false;
	}

	/// Reads a string into `out` (NUL-terminated). Returns false on malformed input (`error_` set)
	/// and sets `too_long` if it did not fit (the string is consumed nonetheless).
	bool read_string(char* out, size_t size, bool& too_long) {
		too_long = false;
		skip_space();
		if (p_ >= end_ || *p_ != '"') {
			error_ = "expected a string";
			return false;
		}
		++p_;
		size_t length = 0;
		while (p_ < end_ && *p_ != '"') {
			unsigned char c = static_cast<unsigned char>(*p_++);
			if (c < 0x20) {
				error_ = "control character in a string";
				return false;
			}
			if (c == '\\') {
				if (p_ >= end_) {
					break;
				}
				char e = *p_++;
				switch (e) {
				case '"': c = '"'; break;
				case '\\': c = '\\'; break;
				case '/': c = '/'; break;
				case 'b': c = '\b'; break;
				case 'f': c = '\f'; break;
				case 'n': c = '\n'; break;
				case 'r': c = '\r'; break;
				case 't': c = '\t'; break;
				case 'u': {
					if (end_ - p_ < 4) {
						error_ = "truncated \\u escape";
						return false;
					}
					unsigned value = 0;
					for (int i = 0; i < 4; ++i) {
						char h = *p_++;
						if (!is_hex(h)) {
							error_ = "invalid \\u escape";
							return false;
						}
						value = value * 16 + static_cast<unsigned>(is_digit(h) ? h - '0' : (h | 0x20) - 'a' + 10);
					}
					if (value == 0 || value > 0x7f) {
						error_ = "\\u escape outside ASCII";
						return false;
					}
					c = static_cast<unsigned char>(value);
					break;
				}
				default:
					error_ = "invalid escape in a string";
					return false;
				}
			}
			if (out != nullptr) {
				if (length + 1 < size) {
					out[length++] = static_cast<char>(c);
				} else {
					too_long = true;
				}
			}
		}
		if (p_ >= end_) {
			error_ = "unterminated string";
			return false;
		}
		++p_; // closing quote
		if (out != nullptr && size > 0) {
			out[length] = '\0';
		}
		return true;
	}

	/// Reads a non-negative integer that fits 32 bits.
	bool read_unsigned(uint32_t& out) {
		skip_space();
		if (p_ >= end_ || !is_digit(*p_)) {
			error_ = "expected a non-negative integer";
			return false;
		}
		uint64_t value = 0;
		while (p_ < end_ && is_digit(*p_)) {
			value = value * 10 + static_cast<uint64_t>(*p_ - '0');
			if (value > 0xffffffffu) {
				error_ = "number too large";
				return false;
			}
			++p_;
		}
		if (p_ < end_ && (*p_ == '.' || *p_ == 'e' || *p_ == 'E')) {
			error_ = "expected an integer";
			return false;
		}
		out = static_cast<uint32_t>(value);
		return true;
	}

	/// Skips any JSON value.
	bool skip_value(int depth) {
		if (depth > kMaxDepth) {
			error_ = "nested too deeply";
			return false;
		}
		skip_space();
		char c = peek();
		if (c == '"') {
			bool too_long = false;
			return read_string(nullptr, 0, too_long);
		}
		if (c == '{' || c == '[') {
			char close = c == '{' ? '}' : ']';
			++p_;
			if (consume(close)) {
				return true;
			}
			for (;;) {
				if (c == '{') {
					bool too_long = false;
					if (!read_string(nullptr, 0, too_long)) {
						return false;
					}
					if (!consume(':')) {
						error_ = "expected ':'";
						return false;
					}
				}
				if (!skip_value(depth + 1)) {
					return false;
				}
				if (consume(close)) {
					return true;
				}
				if (!consume(',')) {
					error_ = "expected ',' or the end of a nested value";
					return false;
				}
			}
		}
		if (c == '-' || is_digit(c)) {
			while (p_ < end_ && (is_digit(*p_) || *p_ == '-' || *p_ == '+' || *p_ == '.' || *p_ == 'e' || *p_ == 'E')) {
				++p_;
			}
			return true;
		}
		static const char* const kWords[] = {"true", "false", "null"};
		for (const char* word : kWords) {
			size_t length = std::strlen(word);
			if (static_cast<size_t>(end_ - p_) >= length && std::strncmp(p_, word, length) == 0) {
				p_ += length;
				return true;
			}
		}
		error_ = "invalid value";
		return false;
	}

	const char* error() const { return error_; }

private:
	const char* p_;
	const char* end_;
	const char* error_ = "invalid JSON";
};

bool copy_text(const char* text, char* out, size_t size) {
	size_t length = std::strlen(text);
	if (length + 1 > size) {
		return false;
	}
	std::memcpy(out, text, length + 1);
	return true;
}

/// Length of `scheme://host[:port]` of a valid URL.
size_t origin_length(const char* url) {
	const char* host = std::strstr(url, "://") + 3;
	const char* p = host;
	while (*p != '\0' && *p != '/') {
		++p;
	}
	return static_cast<size_t>(p - url);
}

} // namespace

bool parse_version(const char* text, Version& out, const char** error) {
	out = Version();
	if (text == nullptr || text[0] == '\0') {
		return refuse(error, "version is empty");
	}
	if (std::strlen(text) > Version::kMaxText) {
		return refuse(error, "version longer than 63 characters");
	}
	const char* p = text;
	if (!parse_number(p, out.major) || *p++ != '.' || !parse_number(p, out.minor) || *p++ != '.' ||
		!parse_number(p, out.patch)) {
		out = Version();
		return refuse(error, "version is not MAJOR.MINOR.PATCH (numbers without leading zeros)");
	}
	if (*p == '-') {
		++p;
		if (!parse_identifiers(p, '+', true, out.prerelease, sizeof(out.prerelease))) {
			out = Version();
			return refuse(error, "invalid pre-release in version (dot-separated [0-9A-Za-z-] identifiers)");
		}
	}
	if (*p == '+') {
		++p;
		if (!parse_identifiers(p, '\0', false, out.build, sizeof(out.build))) {
			out = Version();
			return refuse(error, "invalid build metadata in version (dot-separated [0-9A-Za-z-] identifiers)");
		}
	}
	if (*p != '\0') {
		out = Version();
		return refuse(error, "unexpected characters after the version");
	}
	return true;
}

int compare_versions(const Version& a, const Version& b) {
	if (a.major != b.major) {
		return a.major < b.major ? -1 : 1;
	}
	if (a.minor != b.minor) {
		return a.minor < b.minor ? -1 : 1;
	}
	if (a.patch != b.patch) {
		return a.patch < b.patch ? -1 : 1;
	}
	if (a.is_prerelease() != b.is_prerelease()) {
		return a.is_prerelease() ? -1 : 1;
	}
	if (!a.is_prerelease()) {
		return 0;
	}
	return compare_prerelease(a.prerelease, b.prerelease);
}

bool parse_update_manifest(const char* text, size_t length, UpdateManifest& out, const char** error) {
	out = UpdateManifest();
	if (text == nullptr || length == 0) {
		return refuse(error, "manifest is empty");
	}
	if (length > UpdateManifest::kMaxLength) {
		return refuse(error, "manifest larger than 1024 bytes");
	}
	JsonReader json(text, length);
	if (!json.consume('{')) {
		return refuse(error, "manifest is not a JSON object");
	}
	bool have_version = false;
	bool have_url = false;
	bool have_sha256 = false;
	bool have_size = false;
	bool have_min = false;
	UpdateManifest result;
	if (!json.consume('}')) {
		for (;;) {
			char key[24];
			bool too_long = false;
			if (!json.read_string(key, sizeof(key), too_long)) {
				return refuse(error, json.error());
			}
			if (!json.consume(':')) {
				return refuse(error, "expected ':' after a member name");
			}
			struct Field {
				const char* name;
				char* buffer;
				size_t size;
				bool* seen;
				const char* not_string_reason;
				const char* too_long_reason;
			};
			const Field fields[] = {
				{"version", result.version, sizeof(result.version), &have_version, "version is not a string",
					"version longer than 63 characters"},
				{"url", result.url, sizeof(result.url), &have_url, "url is not a string",
					"url longer than 127 characters"},
				{"sha256", result.sha256, sizeof(result.sha256), &have_sha256, "sha256 is not a string",
					"sha256 is not 64 hex digits"},
				{"min_version", result.min_version, sizeof(result.min_version), &have_min,
					"min_version is not a string", "min_version longer than 63 characters"},
			};
			bool known = false;
			if (!too_long) {
				for (const Field& field : fields) {
					if (std::strcmp(key, field.name) != 0) {
						continue;
					}
					known = true;
					if (*field.seen) {
						return refuse(error, "duplicate member in manifest");
					}
					*field.seen = true;
					json.skip_space();
					if (json.peek() != '"') {
						return refuse(error, field.not_string_reason);
					}
					bool value_too_long = false;
					if (!json.read_string(field.buffer, field.size, value_too_long)) {
						return refuse(error, json.error());
					}
					if (value_too_long) {
						return refuse(error, field.too_long_reason);
					}
				}
				if (!known && std::strcmp(key, "size") == 0) {
					known = true;
					if (have_size) {
						return refuse(error, "duplicate member in manifest");
					}
					have_size = true;
					if (!json.read_unsigned(result.size)) {
						return refuse(error, "size is not a non-negative integer");
					}
				}
			}
			if (!known && !json.skip_value(1)) {
				return refuse(error, json.error());
			}
			if (json.consume('}')) {
				break;
			}
			if (!json.consume(',')) {
				return refuse(error, "expected ',' or '}' in manifest");
			}
		}
	}
	json.skip_space();
	if (!json.at_end()) {
		return refuse(error, "unexpected text after the manifest object");
	}
	if (!have_version) {
		return refuse(error, "manifest has no version");
	}
	if (!have_url) {
		return refuse(error, "manifest has no url");
	}
	if (!have_sha256) {
		return refuse(error, "manifest has no sha256");
	}
	if (!have_size) {
		return refuse(error, "manifest has no size");
	}
	Version version;
	if (!parse_version(result.version, version, error)) {
		return false;
	}
	if (have_min && !parse_version(result.min_version, version, error)) {
		return false;
	}
	if (result.url[0] == '\0') {
		return refuse(error, "url is empty");
	}
	for (const char* p = result.url; *p != '\0'; ++p) {
		if (static_cast<unsigned char>(*p) <= 0x20 || static_cast<unsigned char>(*p) >= 0x7f) {
			return refuse(error, "url contains a space, control or non-ASCII character");
		}
	}
	if (std::strlen(result.sha256) != 64) {
		return refuse(error, "sha256 is not 64 hex digits");
	}
	for (char& c : result.sha256) {
		if (c == '\0') {
			break;
		}
		if (!is_hex(c)) {
			return refuse(error, "sha256 is not 64 hex digits");
		}
		if (c >= 'A' && c <= 'F') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	if (result.size == 0) {
		return refuse(error, "size is 0");
	}
	if (result.size > kMaxImageSize) {
		return refuse(error, "size larger than 16 MB");
	}
	out = result;
	return true;
}

bool update_url_valid(const char* url, const char** error) {
	if (url == nullptr || url[0] == '\0') {
		return refuse(error, "URL is empty");
	}
	size_t length = std::strlen(url);
	if (length > Settings::kMaxStringLength) {
		return refuse(error, "URL longer than 127 characters");
	}
	const char* host = nullptr;
	if (std::strncmp(url, "http://", 7) == 0) {
		host = url + 7;
	} else if (std::strncmp(url, "https://", 8) == 0) {
		host = url + 8;
	} else {
		return refuse(error, "URL must start with http:// or https://");
	}
	if (*host == '\0' || *host == '/' || *host == ':') {
		return refuse(error, "URL has no host");
	}
	for (const char* p = url; *p != '\0'; ++p) {
		unsigned char c = static_cast<unsigned char>(*p);
		if (c <= 0x20 || c >= 0x7f) {
			return refuse(error, "URL contains a space, control or non-ASCII character");
		}
	}
	return true;
}

bool update_manifest_url(const char* base, char* out, size_t size) {
	size_t length = std::strlen(base);
	while (length > 0 && base[length - 1] == '/') {
		--length;
	}
	static const char kName[] = "/manifest.json";
	if (length + sizeof(kName) > size) {
		return false;
	}
	std::memcpy(out, base, length);
	std::memcpy(out + length, kName, sizeof(kName));
	return true;
}

bool resolve_update_url(const char* base, const char* reference, char* out, size_t size, const char** error) {
	if (size > 0) {
		out[0] = '\0';
	}
	if (!update_url_valid(base, error)) {
		return false;
	}
	if (reference == nullptr || reference[0] == '\0') {
		return refuse(error, "image URL is empty");
	}
	char buffer[2 * (Settings::kMaxStringLength + 1)];
	if (std::strncmp(reference, "http://", 7) == 0 || std::strncmp(reference, "https://", 8) == 0) {
		if (!copy_text(reference, buffer, sizeof(buffer))) {
			return refuse(error, "image URL longer than 127 characters");
		}
	} else if (std::strstr(reference, "://") != nullptr) {
		return refuse(error, "image URL must use http:// or https://");
	} else {
		size_t prefix = reference[0] == '/' ? origin_length(base) : std::strlen(base);
		while (reference[0] != '/' && prefix > 0 && base[prefix - 1] == '/') {
			--prefix;
		}
		size_t ref_length = std::strlen(reference);
		size_t separator = reference[0] == '/' ? 0 : 1;
		if (prefix + separator + ref_length + 1 > sizeof(buffer)) {
			return refuse(error, "image URL longer than 127 characters");
		}
		std::memcpy(buffer, base, prefix);
		if (separator != 0) {
			buffer[prefix] = '/';
		}
		std::memcpy(buffer + prefix + separator, reference, ref_length + 1);
	}
	const char* url_error = nullptr;
	if (!update_url_valid(buffer, &url_error)) {
		return refuse(error, std::strcmp(url_error, "URL longer than 127 characters") == 0
			? "image URL longer than 127 characters"
			: url_error);
	}
	if (!copy_text(buffer, out, size)) {
		return refuse(error, "image URL longer than 127 characters");
	}
	return true;
}

const char* update_verdict_name(UpdateVerdict verdict) {
	switch (verdict) {
	case UpdateVerdict::apply:
		return "apply";
	case UpdateVerdict::up_to_date:
		return "up to date";
	case UpdateVerdict::refused:
		return "refused";
	}
	return "?";
}

UpdateCheckResult check_update(const char* running, const UpdateManifest& manifest, const char* base, bool force) {
	UpdateCheckResult result;
	Version current;
	if (!parse_version(running, current)) {
		result.reason = "the running firmware's version is malformed";
		return result;
	}
	Version offered;
	const char* error = nullptr;
	if (!parse_version(manifest.version, offered, &error)) {
		result.reason = error;
		return result;
	}
	// Whatever produced the manifest structure, an image is installed only with a complete, verifiable
	// description - force included.
	if (manifest.url[0] == '\0') {
		result.reason = "the manifest names no image URL";
		return result;
	}
	if (manifest.size == 0) {
		result.reason = "the manifest gives no image size";
		return result;
	}
	for (size_t i = 0; i < 64; ++i) {
		char c = manifest.sha256[i];
		bool lower_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		if (!lower_hex) {
			result.reason = "the manifest's sha256 is not 64 lower-case hex digits";
			return result;
		}
	}
	if (manifest.sha256[64] != '\0') {
		result.reason = "the manifest's sha256 is not 64 lower-case hex digits";
		return result;
	}
	if (!force) {
		int order = compare_versions(offered, current);
		if (order <= 0) {
			result.verdict = UpdateVerdict::up_to_date;
			result.reason = order == 0 ? "the manifest offers the running version" : "the manifest offers an older version";
			return result;
		}
		if (manifest.min_version[0] != '\0' && !current.is_prerelease()) {
			Version minimum;
			if (!parse_version(manifest.min_version, minimum, &error)) {
				result.reason = error;
				return result;
			}
			if (compare_versions(current, minimum) < 0) {
				result.reason = "the running version is below the manifest's min_version: install an intermediate release first";
				return result;
			}
		}
	}
	if (!resolve_update_url(base, manifest.url, result.image_url, sizeof(result.image_url), &error)) {
		result.reason = error;
		return result;
	}
	result.verdict = UpdateVerdict::apply;
	result.reason = force ? "forced" : "newer version";
	return result;
}

const char* instruction_status_name(InstructionStatus status) {
	switch (status) {
	case InstructionStatus::none:
		return "none";
	case InstructionStatus::pending:
		return "pending";
	case InstructionStatus::corrupt:
		return "corrupt";
	case InstructionStatus::upload:
		return "awaiting upload";
	}
	return "?";
}

bool upload_token_valid(const char* token) {
	if (token == nullptr) {
		return false;
	}
	size_t i = 0;
	for (; token[i] != '\0'; ++i) {
		char c = token[i];
		if (i >= kUploadTokenLength || !(is_digit(c) || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return i == kUploadTokenLength;
}

InstructionStatus load_update_instruction(const Settings& settings, UpdateInstruction& out, const char** error) {
	out = UpdateInstruction();
	SettingsStatus status = settings.get_string(setting::kUpdateImage, out.image_url, sizeof(out.image_url));
	if (status == SettingsStatus::not_found) {
		return InstructionStatus::none;
	}
	if (status != SettingsStatus::ok) {
		refuse(error, "update.image unreadable");
		return InstructionStatus::corrupt;
	}
	if (std::strcmp(out.image_url, kUploadMarker) == 0) {
		out.image_url[0] = '\0';
		if (settings.get_string(setting::kUpdateToken, out.token, sizeof(out.token)) != SettingsStatus::ok ||
			!upload_token_valid(out.token)) {
			out.token[0] = '\0';
			refuse(error, "update.token missing or invalid");
			return InstructionStatus::corrupt;
		}
		return InstructionStatus::upload;
	}
	if (!update_url_valid(out.image_url)) {
		refuse(error, "update.image is not a valid URL");
		return InstructionStatus::corrupt;
	}
	Version version;
	if (settings.get_string(setting::kUpdateVersion, out.version, sizeof(out.version)) != SettingsStatus::ok ||
		!parse_version(out.version, version)) {
		refuse(error, "update.version missing or invalid");
		return InstructionStatus::corrupt;
	}
	if (settings.get_string(setting::kUpdateSha256, out.sha256, sizeof(out.sha256)) != SettingsStatus::ok ||
		std::strlen(out.sha256) != 64) {
		refuse(error, "update.sha256 missing or invalid");
		return InstructionStatus::corrupt;
	}
	for (const char* p = out.sha256; *p != '\0'; ++p) {
		if (!is_hex(*p)) {
			refuse(error, "update.sha256 missing or invalid");
			return InstructionStatus::corrupt;
		}
	}
	if (settings.get_int(setting::kUpdateSize, out.size) != SettingsStatus::ok || out.size <= 0) {
		refuse(error, "update.size missing or invalid");
		return InstructionStatus::corrupt;
	}
	status = settings.get_int(setting::kUpdateAttempts, out.attempts);
	if (status == SettingsStatus::not_found) {
		out.attempts = 0;
	} else if (status != SettingsStatus::ok || out.attempts < 0) {
		refuse(error, "update.attempts invalid");
		return InstructionStatus::corrupt;
	}
	return InstructionStatus::pending;
}

bool store_update_instruction(Settings& settings, const UpdateManifest& manifest, const char* image_url,
	const char** error) {
	if (!update_url_valid(image_url, error)) {
		return false;
	}
	if (manifest.size == 0 || manifest.size > kMaxImageSize) {
		return refuse(error, "image size out of range");
	}
	// The image key marks a complete instruction: drop an old one first, write it last.
	SettingsStatus status = settings.erase(setting::kUpdateImage);
	if (status != SettingsStatus::ok && status != SettingsStatus::not_found) {
		return refuse(error, "cannot erase the previous update.image");
	}
	if (settings.set_string(setting::kUpdateVersion, manifest.version) != SettingsStatus::ok) {
		return refuse(error, "cannot store update.version");
	}
	if (settings.set_string(setting::kUpdateSha256, manifest.sha256) != SettingsStatus::ok) {
		return refuse(error, "cannot store update.sha256");
	}
	if (settings.set_int(setting::kUpdateSize, static_cast<int32_t>(manifest.size)) != SettingsStatus::ok) {
		return refuse(error, "cannot store update.size");
	}
	if (settings.set_int(setting::kUpdateAttempts, 0) != SettingsStatus::ok) {
		return refuse(error, "cannot store update.attempts");
	}
	if (settings.set_string(setting::kUpdateImage, image_url) != SettingsStatus::ok) {
		return refuse(error, "cannot store update.image");
	}
	return true;
}

bool store_upload_instruction(Settings& settings, const char* token, const char** error) {
	if (!upload_token_valid(token)) {
		return refuse(error, "the upload token is not 32 lower-case hex digits");
	}
	// The marker in update.image completes the instruction: everything else first.
	if (!clear_update_instruction(settings)) {
		return refuse(error, "cannot erase the previous update instruction");
	}
	SettingsStatus status = settings.erase(setting::kUpdateUploaded);
	if (status != SettingsStatus::ok && status != SettingsStatus::not_found) {
		return refuse(error, "cannot erase update.uploaded");
	}
	if (settings.set_string(setting::kUpdateToken, token) != SettingsStatus::ok) {
		return refuse(error, "cannot store update.token");
	}
	if (settings.set_string(setting::kUpdateImage, kUploadMarker) != SettingsStatus::ok) {
		return refuse(error, "cannot store update.image");
	}
	return true;
}

bool upload_authorized(const char* expected, const char* authorization) {
	static const char kPrefix[] = "Bearer ";
	constexpr size_t kPrefixLength = sizeof(kPrefix) - 1;
	if (!upload_token_valid(expected) || authorization == nullptr ||
		std::strncmp(authorization, kPrefix, kPrefixLength) != 0) {
		return false;
	}
	const char* presented = authorization + kPrefixLength;
	if (std::strlen(presented) != kUploadTokenLength) {
		return false;
	}
	return constant_time_equal(reinterpret_cast<const uint8_t*>(expected), reinterpret_cast<const uint8_t*>(presented),
		kUploadTokenLength);
}

bool clear_update_instruction(Settings& settings) {
	static const char* const kKeys[] = {setting::kUpdateImage, setting::kUpdateToken, setting::kUpdateVersion,
		setting::kUpdateSha256, setting::kUpdateSize, setting::kUpdateAttempts};
	bool ok = true;
	for (const char* key : kKeys) {
		SettingsStatus status = settings.erase(key);
		if (status != SettingsStatus::ok && status != SettingsStatus::not_found) {
			Log::error(kTag, "cannot erase %s: %s", key, settings_status_name(status));
			ok = false;
		}
	}
	return ok;
}

bool update_instruction_fulfilled(const char* running, const UpdateInstruction& instruction) {
	Version current;
	Version wanted;
	return parse_version(running, current) && parse_version(instruction.version, wanted) &&
		compare_versions(current, wanted) == 0;
}

const char* app_image_state_name(AppImageState state) {
	switch (state) {
	case AppImageState::missing:
		return "missing";
	case AppImageState::valid:
		return "valid";
	case AppImageState::aborted:
		return "aborted";
	}
	return "?";
}

const char* updater_action_name(UpdaterAction action) {
	switch (action) {
	case UpdaterAction::boot_app:
		return "boot application";
	case UpdaterAction::download:
		return "download";
	case UpdaterAction::wait:
		return "wait";
	case UpdaterAction::serve_upload:
		return "serve upload";
	}
	return "?";
}

UpdaterStep decide_updater_step(InstructionStatus instruction, int32_t attempts, AppImageState app) {
	if (instruction == InstructionStatus::upload) {
		if (app == AppImageState::valid) {
			return UpdaterStep{UpdaterAction::serve_upload, 0,
				"firmware upload requested; waiting 10 minutes for the image, then booting the application",
				kUploadTimeoutMs};
		}
		return UpdaterStep{UpdaterAction::serve_upload, 0,
			"firmware upload requested and no bootable application in ota_0: waiting for the image without a limit", 0};
	}
	if (instruction != InstructionStatus::pending) {
		if (app != AppImageState::missing) {
			return UpdaterStep{UpdaterAction::boot_app, 0,
				instruction == InstructionStatus::corrupt ? "the update instruction is unreadable; booting the application"
														  : "no update pending; booting the application"};
		}
		return UpdaterStep{UpdaterAction::wait, kUpdateIdleRetryMs,
			"no application in ota_0 and no update pending: flash over USB or let an application request an update"};
	}
	if (attempts < kUpdateMaxAttempts) {
		return UpdaterStep{UpdaterAction::download, attempts == 0 ? 0u : kUpdateRetryDelayMs,
			attempts == 0 ? "update pending" : "update pending; retrying"};
	}
	if (app == AppImageState::valid) {
		return UpdaterStep{UpdaterAction::boot_app, 0,
			"update attempts used up; the previous application is intact, booting it"};
	}
	return UpdaterStep{UpdaterAction::download, kUpdateIdleRetryMs,
		"update attempts used up and no bootable application; retrying every 5 minutes"};
}

UpdaterStep decide_upload_expired(AppImageState app, bool upload_running) {
	if (upload_running) {
		return UpdaterStep{UpdaterAction::serve_upload, 0, "an upload is being written; waiting for it", 0};
	}
	if (app == AppImageState::valid) {
		return UpdaterStep{UpdaterAction::boot_app, 0,
			"no firmware upload within 10 minutes; the previous application is intact, booting it"};
	}
	return UpdaterStep{UpdaterAction::serve_upload, 0,
		"no firmware upload within 10 minutes and no bootable application in ota_0: waiting for the image without "
		"a limit", 0};
}

namespace {

uint32_t read_le32(const uint8_t* p) {
	return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
		static_cast<uint32_t>(p[3]) << 24;
}

/// Copies a fixed-size, possibly unterminated descriptor string.
void copy_field(char* out, const uint8_t* field, size_t size) {
	size_t n = 0;
	while (n < size && field[n] != 0) {
		unsigned char c = field[n];
		out[n] = c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
		++n;
	}
	out[n] = '\0';
}

} // namespace

bool check_app_image(const uint8_t* data, size_t length, const char* project, uint16_t chip_id, AppImageInfo& out,
	char* reason, size_t reason_size) {
	out = AppImageInfo();
	if (length < app_image::kHeadLength) {
		std::snprintf(reason, reason_size, "image shorter than its header (%u bytes)", static_cast<unsigned>(length));
		return false;
	}
	if (data[0] != app_image::kMagic) {
		std::snprintf(reason, reason_size, "not an ESP firmware image (magic 0x%02x; an error page?)",
			static_cast<unsigned>(data[0]));
		return false;
	}
	out.chip_id = static_cast<uint16_t>(data[app_image::kChipIdOffset] | data[app_image::kChipIdOffset + 1] << 8);
	if (out.chip_id != chip_id) {
		std::snprintf(reason, reason_size, "image is for another chip (chip id %u, this is %u)",
			static_cast<unsigned>(out.chip_id), static_cast<unsigned>(chip_id));
		return false;
	}
	const uint8_t* desc = data + app_image::kDescOffset;
	if (read_le32(desc) != app_image::kDescMagic) {
		std::snprintf(reason, reason_size, "image has no app descriptor");
		return false;
	}
	copy_field(out.version, desc + app_image::kVersionOffset, sizeof(out.version) - 1);
	copy_field(out.project, desc + app_image::kProjectOffset, sizeof(out.project) - 1);
	if (std::strcmp(out.project, project) != 0) {
		std::snprintf(reason, reason_size, "image is \"%s\", not the application \"%s\"", out.project, project);
		return false;
	}
	return true;
}

bool updater_supports_upload(const char* version) {
	Version parsed;
	if (version == nullptr || !parse_version(version, parsed)) {
		return false;
	}
	const size_t feature = std::strlen(kUpdaterUploadFeature);
	for (const char* p = parsed.build; *p != '\0';) {
		const char* end = std::strchr(p, '.');
		size_t n = end != nullptr ? static_cast<size_t>(end - p) : std::strlen(p);
		if (n == feature && std::strncmp(p, kUpdaterUploadFeature, n) == 0) {
			return true;
		}
		if (end == nullptr) {
			break;
		}
		p = end + 1;
	}
	return false;
}

} // namespace hc
