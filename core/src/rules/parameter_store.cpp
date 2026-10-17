/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/rules/parameter_store.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "hc/log.h"

namespace hc::rules {

namespace {

constexpr const char* kTag = "hc.rule";
constexpr const char* kPrefix = "rp.";
constexpr size_t kTextSize = Settings::kMaxStringLength + 1;

int hex_value(char c) {
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

bool parse_int(const char* text, int64_t& out) {
	if (*text == '\0') {
		return false;
	}
	char* end = nullptr;
	long long value = std::strtoll(text, &end, 10);
	if (*end != '\0') {
		return false;
	}
	out = value;
	return true;
}

} // namespace

uint64_t ParameterStore::hash(const char* rule, const char* parameter) {
	uint64_t h = 14695981039346656037ull;
	auto mix = [&h](const char* text) {
		for (; *text; ++text) {
			h ^= static_cast<uint8_t>(*text);
			h *= 1099511628211ull;
		}
	};
	mix(rule);
	mix("/");
	mix(parameter);
	return (h ^ (h >> 48)) & 0xFFFFFFFFFFFFull;
}

void ParameterStore::key_for(const char* rule, const char* parameter, char* out) {
	std::snprintf(out, 16, "%s%012" PRIx64, kPrefix, hash(rule, parameter));
}

bool ParameterStore::encode(const char* rule, const char* parameter, const Val& v, char* out, size_t size) {
	if (!v.present) {
		return false;
	}
	int used = std::snprintf(out, size, "%s/%s=", rule, parameter);
	if (used < 0 || static_cast<size_t>(used) >= size) {
		return false;
	}
	char* p = out + used;
	size_t room = size - static_cast<size_t>(used);
	int n = -1;
	switch (v.type) {
		case Type::boolean: n = std::snprintf(p, room, "b:%d", v.u.b ? 1 : 0); break;
		case Type::integer: n = std::snprintf(p, room, "i:%" PRId64, v.u.i); break;
		case Type::real: n = std::snprintf(p, room, "r:%.17g", v.u.r); break;
		case Type::duration: n = std::snprintf(p, room, "d:%" PRId64, v.u.i); break;
		case Type::enumeration: n = std::snprintf(p, room, "e:%" PRIu32, v.u.e); break;
		case Type::time_of_day:
			n = std::snprintf(p, room, "t:%02u:%02u:%02u/%u", v.u.tod.hour, v.u.tod.minute, v.u.tod.second,
				v.u.tod.day_of_week);
			break;
		case Type::color:
			n = std::snprintf(p, room, "c:%02x%02x%02x%02x/%x", v.u.color.red, v.u.color.green, v.u.color.blue,
				v.u.color.white, v.u.color.valid);
			break;
		case Type::string: {
			if (room < 3 + 2 * static_cast<size_t>(v.length)) {
				return false;
			}
			p[0] = 's';
			p[1] = ':';
			for (size_t i = 0; i < v.length; ++i) {
				std::snprintf(p + 2 + 2 * i, 3, "%02x", static_cast<uint8_t>(v.u.bytes[i]));
			}
			p[2 + 2 * v.length] = '\0';
			n = static_cast<int>(2 + 2 * v.length);
			break;
		}
		default: return false;
	}
	return n >= 0 && static_cast<size_t>(n) < room;
}

bool ParameterStore::decode(const char* text, char* logical, size_t logical_size, Val& out) {
	const char* equals = std::strchr(text, '=');
	if (equals == nullptr || static_cast<size_t>(equals - text) >= logical_size || equals[1] == '\0' ||
		equals[2] != ':') {
		return false;
	}
	std::memcpy(logical, text, static_cast<size_t>(equals - text));
	logical[equals - text] = '\0';
	char code = equals[1];
	const char* value = equals + 3;
	int64_t number = 0;
	switch (code) {
		case 'b':
			if (std::strcmp(value, "0") != 0 && std::strcmp(value, "1") != 0) {
				return false;
			}
			out = Val::of_bool(value[0] == '1');
			return true;
		case 'i':
			if (!parse_int(value, number)) {
				return false;
			}
			out = Val::of_int(number);
			return true;
		case 'd':
			if (!parse_int(value, number)) {
				return false;
			}
			out = Val::of_duration(number);
			return true;
		case 'e':
			if (!parse_int(value, number) || number < 0 || number > 4294967295LL) {
				return false;
			}
			out = Val::of_enum(static_cast<uint32_t>(number));
			return true;
		case 'r': {
			char* end = nullptr;
			double r = std::strtod(value, &end);
			if (*value == '\0' || *end != '\0' || !std::isfinite(r)) {
				return false;
			}
			out = Val::of_real(r);
			return true;
		}
		case 't': {
			unsigned h = 0, m = 0, s = 0, d = 0;
			char tail = 0;
			if (std::sscanf(value, "%2u:%2u:%2u/%1u%c", &h, &m, &s, &d, &tail) != 4 || h > 23 || m > 59 || s > 59 ||
				d > 7) {
				return false;
			}
			TimeOfDay t;
			t.hour = static_cast<uint8_t>(h);
			t.minute = static_cast<uint8_t>(m);
			t.second = static_cast<uint8_t>(s);
			t.day_of_week = static_cast<uint8_t>(d);
			out = Val::of_time(t);
			return true;
		}
		case 'c': {
			unsigned r = 0, g = 0, b = 0, w = 0, valid = 0;
			char tail = 0;
			if (std::strlen(value) < 10 ||
				std::sscanf(value, "%2x%2x%2x%2x/%x%c", &r, &g, &b, &w, &valid, &tail) != 5 || valid > Color::kAll) {
				return false;
			}
			Color c;
			c.red = static_cast<uint8_t>(r);
			c.green = static_cast<uint8_t>(g);
			c.blue = static_cast<uint8_t>(b);
			c.white = static_cast<uint8_t>(w);
			c.valid = static_cast<uint8_t>(valid);
			out = Val::of_color(c);
			return true;
		}
		case 's': {
			size_t length = std::strlen(value);
			if (length % 2 != 0 || length / 2 > kMaxString) {
				return false;
			}
			char bytes[kMaxString];
			for (size_t i = 0; i < length / 2; ++i) {
				int high = hex_value(value[2 * i]);
				int low = hex_value(value[2 * i + 1]);
				if (high < 0 || low < 0) {
					return false;
				}
				bytes[i] = static_cast<char>(high * 16 + low);
			}
			out = Val::of_string(bytes, length / 2);
			return out.present;
		}
		default: return false;
	}
}

bool ParameterStore::write(const char* rule, const char* parameter, const Val& value) {
	char key[16];
	char text[kTextSize];
	key_for(rule, parameter, key);
	if (!encode(rule, parameter, value, text, sizeof(text))) {
		Log::error(kTag, "%s override of '%s' not persisted: the value cannot be encoded", rule, parameter);
		return false;
	}
	SettingsStatus status = settings_.set_string(key, text);
	if (status != SettingsStatus::ok) {
		Log::error(kTag, "%s override of '%s' not persisted: %s", rule, parameter, settings_status_name(status));
		return false;
	}
	return true;
}

bool ParameterStore::read(const char* rule, const char* parameter, Val& out) const {
	char key[16];
	char text[kTextSize];
	key_for(rule, parameter, key);
	if (settings_.get_string(key, text, sizeof(text)) != SettingsStatus::ok) {
		return false;
	}
	char logical[kMaxLogicalKey + 1];
	char expected[kMaxLogicalKey + 2];
	std::snprintf(expected, sizeof(expected), "%s/%s", rule, parameter);
	return decode(text, logical, sizeof(logical), out) && std::strcmp(logical, expected) == 0;
}

void ParameterStore::erase(const char* rule, const char* parameter) {
	Val ignored;
	if (read(rule, parameter, ignored)) {
		char key[16];
		key_for(rule, parameter, key);
		settings_.erase(key);
	}
}

namespace {

/// Collects the keys under the prefix whose entries are to be deleted.
class PruneCollector : public SettingsVisitor {
public:
	PruneCollector(const Settings& settings, ParameterStore::Visitor& visitor, char (*keys)[16], size_t capacity)
		: settings_(settings), visitor_(visitor), keys_(keys), capacity_(capacity) {}

	void entry(const char* key, SettingType type) override {
		if (std::strncmp(key, kPrefix, std::strlen(kPrefix)) != 0) {
			return;
		}
		bool keep = false;
		if (type == SettingType::string) {
			char text[kTextSize];
			char logical[ParameterStore::kMaxLogicalKey + 1];
			Val value;
			if (settings_.get_string(key, text, sizeof(text)) == SettingsStatus::ok &&
				ParameterStore::decode(text, logical, sizeof(logical), value)) {
				keep = visitor_.entry(logical, value);
			}
		}
		if (!keep) {
			if (keys_ != nullptr && count < capacity_) {
				std::snprintf(keys_[count], 16, "%s", key);
			}
			++count;
		}
	}

	size_t count = 0;

private:
	const Settings& settings_;
	ParameterStore::Visitor& visitor_;
	char (*keys_)[16];
	size_t capacity_;
};

} // namespace

void ParameterStore::prune(Visitor& visitor) {
	// Pass 1 counts the entries that go, pass 2 collects their keys; erasing happens after the
	// visits (a visitor must not modify the settings).
	PruneCollector count_pass(settings_, visitor, nullptr, 0);
	settings_.visit(count_pass);
	if (count_pass.count == 0) {
		return;
	}
	char (*keys)[16] = new (std::nothrow) char[count_pass.count][16];
	if (keys == nullptr) {
		Log::error(kTag, "no memory to prune %u stored parameter overrides", static_cast<unsigned>(count_pass.count));
		return;
	}
	PruneCollector collect(settings_, visitor, keys, count_pass.count);
	settings_.visit(collect);
	size_t n = collect.count < count_pass.count ? collect.count : count_pass.count;
	for (size_t i = 0; i < n; ++i) {
		settings_.erase(keys[i]);
	}
	delete[] keys;
}

} // namespace hc::rules
