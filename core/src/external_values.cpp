/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/external_values.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "hc/backoff.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.ext";

constexpr const char* kOpenMeteo = "open-meteo";

/// The Open-Meteo forecast API (no key; https://open-meteo.com/en/docs). `current` holds the
/// values of the last 15-minute step, `hourly` starts at the current hour, so index n is n hours
/// ahead. Wind speed in m/s (the KNX DPT 9.005 unit), times in UTC.
const ExternalPresetVariable kOpenMeteoVariables[] = {
	{"temperature", "current.temperature_2m", "\xc2\xb0" "C", "temperature_2m", false, 0},
	{"apparent_temperature", "current.apparent_temperature", "\xc2\xb0" "C", "apparent_temperature", false, 0},
	{"humidity", "current.relative_humidity_2m", "%", "relative_humidity_2m", false, 0},
	{"precipitation", "current.precipitation", "mm", "precipitation", false, 0},
	{"rain", "current.rain", "mm", "rain", false, 0},
	{"wind_speed", "current.wind_speed_10m", "m/s", "wind_speed_10m", false, 0},
	{"wind_direction", "current.wind_direction_10m", "\xc2\xb0", "wind_direction_10m", false, 0},
	{"cloud_cover", "current.cloud_cover", "%", "cloud_cover", false, 0},
	{"pressure", "current.pressure_msl", "hPa", "pressure_msl", false, 0},
	{"weather_code", "current.weather_code", "WMO", "weather_code", false, 0},
	{"temperature_1h", "hourly.temperature_2m[1]", "\xc2\xb0" "C", "temperature_2m", true, 1},
	{"temperature_3h", "hourly.temperature_2m[3]", "\xc2\xb0" "C", "temperature_2m", true, 3},
	{"temperature_6h", "hourly.temperature_2m[6]", "\xc2\xb0" "C", "temperature_2m", true, 6},
	{"precipitation_1h", "hourly.precipitation[1]", "mm", "precipitation", true, 1},
	{"precipitation_3h", "hourly.precipitation[3]", "mm", "precipitation", true, 3},
	{"precipitation_6h", "hourly.precipitation[6]", "mm", "precipitation", true, 6},
};

constexpr size_t kOpenMeteoCount = sizeof(kOpenMeteoVariables) / sizeof(kOpenMeteoVariables[0]);

const ExternalPresetVariable* find_open_meteo(const char* name) {
	for (const ExternalPresetVariable& v : kOpenMeteoVariables) {
		if (std::strcmp(v.name, name) == 0) {
			return &v;
		}
	}
	return nullptr;
}

/// Longest text a placeholder becomes ("-180.00").
constexpr size_t kPlaceholderWidth = 7;

bool set_error(const char** error, const char* text) {
	if (error != nullptr) {
		*error = text;
	}
	return false;
}

bool id_valid(const char* id) {
	size_t n = std::strlen(id);
	if (n == 0 || n > ExternalValues::kMaxId) {
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		char c = id[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
			c == '_' || c == '-';
		if (!ok) {
			return false;
		}
	}
	return true;
}

/// Checks a URL template: scheme, a host, no space or control character, only {lat} and {lon}.
/// Sets `placeholders` when it has one.
bool url_valid(const char* url, bool& placeholders, const char** error) {
	placeholders = false;
	const char* rest = nullptr;
	if (std::strncmp(url, "https://", 8) == 0) {
		rest = url + 8;
	} else if (std::strncmp(url, "http://", 7) == 0) {
		rest = url + 7;
	} else {
		return set_error(error, "not an http:// or https:// URL");
	}
	if (*rest == '\0' || *rest == '/' || *rest == ':' || *rest == '?' || *rest == '{') {
		return set_error(error, "no host");
	}
	size_t expanded = 0;
	for (const char* p = url; *p != '\0'; ++p) {
		unsigned char c = static_cast<unsigned char>(*p);
		if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\') {
			return set_error(error, "a space, quote, backslash, control or non-ASCII character");
		}
		if (c == '{') {
			if (std::strncmp(p, "{lat}", 5) != 0 && std::strncmp(p, "{lon}", 5) != 0) {
				return set_error(error, "an unknown placeholder (only {lat} and {lon})");
			}
			placeholders = true;
			expanded += kPlaceholderWidth;
			p += 4;
			continue;
		}
		if (c == '}') {
			return set_error(error, "a '}' without a placeholder");
		}
		++expanded;
	}
	if (expanded > HttpFetcher::kMaxUrl) {
		return set_error(error, "longer than 383 bytes when filled");
	}
	return true;
}

bool feedable(ValueType type) {
	switch (type) {
	case ValueType::real:
	case ValueType::signed_int:
	case ValueType::unsigned_int:
	case ValueType::enumeration:
	case ValueType::boolean:
	case ValueType::string:
		return true;
	default:
		return false;
	}
}

/// Converts the JSON value of a binding to the datapoint's type.
bool to_value(const JsonSpan& span, ValueType type, double scale, double offset, Value& out, const char** error) {
	switch (span.kind) {
	case JsonSpan::Kind::number: {
		double x = 0;
		if (!json_number(span, x)) {
			return set_error(error, "a number out of range");
		}
		double y = x * scale + offset;
		if (!std::isfinite(y)) {
			return set_error(error, "out of range after scaling");
		}
		switch (type) {
		case ValueType::real:
			out = Value::of_real(y);
			return true;
		case ValueType::signed_int: {
			double r = std::round(y);
			if (r < -2147483648.0 || r > 2147483647.0) {
				return set_error(error, "out of the signed datapoint's range");
			}
			out = Value::of_signed(static_cast<int32_t>(r));
			return true;
		}
		case ValueType::unsigned_int:
		case ValueType::enumeration: {
			double r = std::round(y);
			if (r < 0 || r > 4294967295.0) {
				return set_error(error, "out of the unsigned datapoint's range");
			}
			uint32_t n = static_cast<uint32_t>(r);
			out = type == ValueType::enumeration ? Value::of_enum(n) : Value::of_unsigned(n);
			return true;
		}
		default:
			return set_error(error, "a number for a datapoint that is not numeric");
		}
	}
	case JsonSpan::Kind::boolean: {
		bool b = false;
		if (type != ValueType::boolean || !json_bool(span, b)) {
			return set_error(error, "a boolean for a datapoint that is not boolean");
		}
		out = Value::of_bool(b);
		return true;
	}
	case JsonSpan::Kind::string: {
		if (type != ValueType::string) {
			return set_error(error, "a string for a datapoint that is not a string");
		}
		char text[Value::kMaxString + 1];
		size_t length = 0;
		if (!json_string(span, text, sizeof(text), &length)) {
			return set_error(error, "a string longer than 28 bytes");
		}
		std::optional<Value> v = Value::of_string(text, length);
		if (!v) {
			return set_error(error, "a string the datapoint cannot hold");
		}
		out = *v;
		return true;
	}
	case JsonSpan::Kind::null:
		return set_error(error, "null (the service has no value)");
	case JsonSpan::Kind::array:
		return set_error(error, "an array, not a value");
	case JsonSpan::Kind::object:
		return set_error(error, "an object, not a value");
	case JsonSpan::Kind::invalid:
		break;
	}
	return set_error(error, "invalid");
}

bool read_coordinate(const Settings& settings, const char* key, double min, double max, double& out) {
	char text[Settings::kMaxStringLength + 1];
	if (settings.get_string(key, text, sizeof(text)) != SettingsStatus::ok) {
		return false;
	}
	return parse_coordinate(text, min, max, out);
}

} // namespace

const ExternalPresetVariable* open_meteo_variables(size_t& count) {
	count = kOpenMeteoCount;
	return kOpenMeteoVariables;
}

bool external_path_valid(const char* path, const char** error) {
	if (path == nullptr || *path == '\0') {
		return set_error(error, "empty path");
	}
	if (std::strlen(path) > ExternalValues::kMaxPath) {
		return set_error(error, "path longer than 63 bytes");
	}
	const char* p = path;
	for (;;) {
		if (*p == '[') {
			++p;
			size_t digits = 0;
			while (*p >= '0' && *p <= '9') {
				++p;
				++digits;
			}
			if (digits == 0 || digits > 5 || *p != ']') {
				return set_error(error, "an index must be [0]..[99999]");
			}
			++p;
		} else {
			size_t n = 0;
			while (*p != '\0' && *p != '.' && *p != '[') {
				unsigned char c = static_cast<unsigned char>(*p);
				if (c <= 0x20 || c == 0x7f || c == ']' || c == '"' || c == '\\') {
					return set_error(error, "a member name with a space, quote, backslash, ']' or control character");
				}
				++p;
				++n;
			}
			if (n == 0) {
				return set_error(error, "an empty member name");
			}
		}
		if (*p == '\0') {
			return true;
		}
		if (*p == '.') {
			++p;
			if (*p == '\0' || *p == '.' || *p == '[') {
				return set_error(error, "an empty member name");
			}
		} else if (*p != '[') {
			return set_error(error, "']' must be followed by '.', '[' or the end");
		}
	}
}

bool external_extract(const JsonSpan& root, const char* path, JsonSpan& out, const char** error, size_t* failed_at) {
	JsonSpan current = root;
	const char* p = path;
	while (*p != '\0') {
		if (failed_at != nullptr) {
			*failed_at = static_cast<size_t>(p - path);
		}
		if (*p == '[') {
			++p;
			unsigned long index = 0;
			while (*p >= '0' && *p <= '9') {
				index = index * 10 + static_cast<unsigned long>(*p - '0');
				++p;
			}
			++p; // ']'
			if (current.kind != JsonSpan::Kind::array) {
				return set_error(error, "not an array");
			}
			size_t position = 0;
			JsonSpan item;
			bool found = false;
			for (unsigned long i = 0; json_next_item(current, position, item); ++i) {
				if (i == index) {
					found = true;
					break;
				}
			}
			if (!found) {
				return set_error(error, "index out of range");
			}
			current = item;
		} else {
			char name[ExternalValues::kMaxPath + 1];
			size_t n = 0;
			while (*p != '\0' && *p != '.' && *p != '[' && n < sizeof(name) - 1) {
				name[n++] = *p++;
			}
			name[n] = '\0';
			if (current.kind != JsonSpan::Kind::object) {
				return set_error(error, "not an object");
			}
			JsonSpan member;
			if (!json_member(current, name, member)) {
				return set_error(error, "member missing");
			}
			current = member;
		}
		if (*p == '.') {
			++p;
		}
	}
	out = current;
	return true;
}

// ---- Tables ----------------------------------------------------------------------------------

struct ExternalValues::Source {
	char id[kMaxId + 1] = {};
	char url[kMaxUrl + 1] = {};
	const char* preset = ""; ///< Static text.
	uint32_t interval_s = kDefaultIntervalS;
	size_t first = 0; ///< First binding.
	size_t count = 0;
	bool placeholders = false;

	// Runtime state.
	uint64_t anchor = 0; ///< A point of the fetch grid.
	uint64_t due = 0;
	uint64_t last_attempt = 0;
	uint64_t last_ok = 0;
	uint32_t failures = 0;
	uint32_t fetches = 0;
	bool attempted = false;
	bool waiting = false; ///< Missed a due time for a missing condition: fetch when it clears.
	Backoff backoff{kRetryInitialMs, kRetryCapMs};
	char last_error[112] = {};
};

struct ExternalValues::Table {
	struct Binding {
		char datapoint_name[Datapoint::kMaxName + 1] = {};
		char path[kMaxPath + 1] = {};
		char unit[kMaxUnit + 1] = {};
		double scale = 1;
		double offset = 0;
		bool variable = false; ///< `path` holds a preset variable until finish() resolves it.
		uint16_t datapoint = 0;
	};

	Source* sources = nullptr;
	size_t source_capacity = 0;
	size_t source_count = 0;
	Binding* bindings = nullptr;
	size_t binding_capacity = 0;
	size_t binding_count = 0;
	Value* pending = nullptr; ///< The values of one response, before they are reported (one per binding).
	/// The whole block: the table and its three arrays.
	size_t bytes = 0;

	/// One block for exactly `sources` sources and `bindings` bindings (issue #44: no table at the
	/// limits); nullptr without memory, `bytes` receives the size asked for.
	static Table* create(size_t sources, size_t bindings, size_t& bytes);
	static void destroy(Table* table);
};

namespace {

size_t align_up(size_t n, size_t alignment) {
	return (n + alignment - 1) / alignment * alignment;
}

} // namespace

ExternalValues::Table* ExternalValues::Table::create(size_t sources, size_t bindings, size_t& bytes) {
	size_t at_sources = align_up(sizeof(Table), alignof(Source));
	size_t at_bindings = align_up(at_sources + sources * sizeof(Source), alignof(Binding));
	size_t at_pending = align_up(at_bindings + bindings * sizeof(Binding), alignof(Value));
	bytes = at_pending + bindings * sizeof(Value);
	void* memory = ::operator new(bytes, std::nothrow);
	if (memory == nullptr) {
		return nullptr;
	}
	char* base = static_cast<char*>(memory);
	Table* table = new (memory) Table;
	table->bytes = bytes;
	table->sources = reinterpret_cast<Source*>(base + at_sources);
	table->source_capacity = sources;
	for (size_t i = 0; i < sources; ++i) {
		new (&table->sources[i]) Source;
	}
	table->bindings = reinterpret_cast<Binding*>(base + at_bindings);
	table->pending = reinterpret_cast<Value*>(base + at_pending);
	table->binding_capacity = bindings;
	for (size_t i = 0; i < bindings; ++i) {
		new (&table->bindings[i]) Binding;
		new (&table->pending[i]) Value;
	}
	return table;
}

void ExternalValues::Table::destroy(Table* table) {
	if (table == nullptr) {
		return;
	}
	for (size_t i = 0; i < table->binding_capacity; ++i) {
		table->pending[i].~Value();
		table->bindings[i].~Binding();
	}
	for (size_t i = 0; i < table->source_capacity; ++i) {
		table->sources[i].~Source();
	}
	table->~Table();
	::operator delete(table);
}

size_t ExternalValues::table_bytes() const {
	return table_ != nullptr ? table_->bytes : 0;
}

ExternalValues::ExternalValues(EventLoop& loop, const TimeSource& time, const Settings& settings,
	const Connectivity& connectivity, DatapointModel& model, HttpFetcher& fetcher)
	: loop_(loop), time_(time), settings_(settings), connectivity_(connectivity), model_(model), fetcher_(fetcher) {}

ExternalValues::~ExternalValues() {
	loop_.cancel(timer_);
	Table::destroy(table_);
	free_response();
}

bool ExternalValues::reject(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(error_, sizeof(error_), fmt, args);
	va_end(args);
	return false;
}

bool ExternalValues::refuse(const char** error) {
	Log::error(kTag, "configuration refused: %s", error_);
	if (error != nullptr) {
		*error = error_;
	}
	return false;
}

namespace {

bool copy_text(char* out, size_t size, const char* text) {
	size_t n = std::strlen(text);
	if (n >= size) {
		return false;
	}
	std::memcpy(out, text, n + 1);
	return true;
}

} // namespace

bool ExternalValues::fill(Table& table, const ExternalSourceSpec* specs, size_t count) {
	if (count > kMaxSources) {
		return reject("%zu sources, at most %zu", count, kMaxSources);
	}
	if (count > 0 && specs == nullptr) {
		return reject("no source table");
	}
	for (size_t i = 0; i < count; ++i) {
		const ExternalSourceSpec& spec = specs[i];
		if (i >= table.source_capacity) {
			return reject("%zu sources, at most %zu", count, kMaxSources);
		}
		Source& source = table.sources[i];
		if (spec.id == nullptr || !copy_text(source.id, sizeof(source.id), spec.id)) {
			return reject("source %zu: the id is missing or longer than %zu characters", i + 1, kMaxId);
		}
		if (spec.url != nullptr && !copy_text(source.url, sizeof(source.url), spec.url)) {
			return reject("source '%s': the URL is longer than %zu bytes", source.id, kMaxUrl);
		}
		if (spec.preset != nullptr) {
			if (std::strcmp(spec.preset, kOpenMeteo) != 0) {
				return reject("source '%s': unknown preset '%.32s' (known: %s)", source.id, spec.preset, kOpenMeteo);
			}
			source.preset = kOpenMeteo;
		}
		if (spec.url != nullptr && spec.preset != nullptr) {
			return reject("source '%s': both a URL and a preset (one of them)", source.id);
		}
		if (spec.url == nullptr && spec.preset == nullptr) {
			return reject("source '%s': neither a URL nor a preset", source.id);
		}
		source.interval_s = spec.interval_s;
		if (spec.binding_count > 0 && spec.bindings == nullptr) {
			return reject("source '%s': no binding table", source.id);
		}
		source.first = table.binding_count;
		for (size_t b = 0; b < spec.binding_count; ++b) {
			if (table.binding_count >= table.binding_capacity) {
				// The table is sized from the counts, capped at the limit.
				return reject("more than %zu bindings in all sources", kMaxBindings);
			}
			const ExternalBindingSpec& in = spec.bindings[b];
			Table::Binding& out = table.bindings[table.binding_count];
			if (in.datapoint == nullptr || !copy_text(out.datapoint_name, sizeof(out.datapoint_name), in.datapoint)) {
				return reject("source '%s', binding %zu: the datapoint name is missing or too long", source.id, b + 1);
			}
			const char* where = source.preset[0] != '\0' ? in.variable : in.path;
			const char* other = source.preset[0] != '\0' ? in.path : in.variable;
			if (other != nullptr) {
				return reject("source '%s', datapoint '%s': %s", source.id, out.datapoint_name,
					source.preset[0] != '\0' ? "a preset source takes a variable, not a path"
											 : "a source with a URL takes a path, not a variable");
			}
			if (where == nullptr || !copy_text(out.path, sizeof(out.path), where)) {
				return reject("source '%s', datapoint '%s': the %s is missing or longer than %zu bytes", source.id,
					out.datapoint_name, source.preset[0] != '\0' ? "variable" : "path", kMaxPath);
			}
			out.variable = source.preset[0] != '\0';
			out.scale = in.scale;
			out.offset = in.offset;
			if (in.unit != nullptr && !copy_text(out.unit, sizeof(out.unit), in.unit)) {
				return reject("source '%s', datapoint '%s': the unit is longer than %zu bytes", source.id,
					out.datapoint_name, kMaxUnit);
			}
			++table.binding_count;
		}
		source.count = table.binding_count - source.first;
		table.source_count = i + 1;
	}
	return true;
}

namespace {

/// Unescapes a JSON string member into `out`; `what` names it for the error.
enum class Text : uint8_t { ok, absent, invalid };

Text json_text(const JsonSpan& object, const char* name, char* out, size_t size) {
	JsonSpan span;
	if (!json_member(object, name, span)) {
		return Text::absent;
	}
	return json_string(span, out, size) ? Text::ok : Text::invalid;
}

} // namespace

bool ExternalValues::fill_json(Table& table, const char* text, size_t length) {
	JsonSpan root;
	const char* error = nullptr;
	if (!json_parse(text, length, root, &error)) {
		return reject("not JSON: %s", error);
	}
	if (root.kind != JsonSpan::Kind::object) {
		return reject("the external member must be an object with \"sources\"");
	}
	JsonSpan sources;
	{
		size_t position = 0;
		JsonSpan name, value;
		while (json_next_member(root, position, name, value)) {
			if (!json_string_equals(name, "sources")) {
				char key[24];
				if (!json_string(name, key, sizeof(key))) {
					std::snprintf(key, sizeof(key), "?");
				}
				return reject("unknown member \"%s\" (only \"sources\")", key);
			}
		}
	}
	if (!json_member(root, "sources", sources) || sources.kind != JsonSpan::Kind::array) {
		return reject("\"sources\" must be an array");
	}
	size_t position = 0;
	JsonSpan item;
	size_t index = 0;
	while (json_next_item(sources, position, item)) {
		++index;
		if (index > kMaxSources || index > table.source_capacity) {
			return reject("more than %zu sources", kMaxSources);
		}
		if (item.kind != JsonSpan::Kind::object) {
			return reject("source %zu is not an object", index);
		}
		Source& source = table.sources[index - 1];
		Text t = json_text(item, "id", source.id, sizeof(source.id));
		if (t != Text::ok) {
			return reject("source %zu: \"id\" %s", index,
				t == Text::absent ? "is missing" : "must be a string of at most 15 characters");
		}
		size_t member_position = 0;
		JsonSpan name, value;
		while (json_next_member(item, member_position, name, value)) {
			if (!json_string_equals(name, "id") && !json_string_equals(name, "url") &&
				!json_string_equals(name, "preset") && !json_string_equals(name, "interval") &&
				!json_string_equals(name, "bindings")) {
				char key[24];
				if (!json_string(name, key, sizeof(key))) {
					std::snprintf(key, sizeof(key), "?");
				}
				return reject("source '%s': unknown member \"%s\"", source.id, key);
			}
		}
		bool has_url = false;
		t = json_text(item, "url", source.url, sizeof(source.url));
		if (t == Text::invalid) {
			return reject("source '%s': \"url\" must be a string of at most %zu bytes", source.id, kMaxUrl);
		}
		has_url = t == Text::ok;
		char preset[24];
		t = json_text(item, "preset", preset, sizeof(preset));
		if (t == Text::invalid) {
			return reject("source '%s': \"preset\" must be a short string", source.id);
		}
		bool has_preset = t == Text::ok;
		if (has_preset) {
			if (std::strcmp(preset, kOpenMeteo) != 0) {
				return reject("source '%s': unknown preset '%s' (known: %s)", source.id, preset, kOpenMeteo);
			}
			source.preset = kOpenMeteo;
		}
		if (has_url && has_preset) {
			return reject("source '%s': both \"url\" and \"preset\" (one of them)", source.id);
		}
		if (!has_url && !has_preset) {
			return reject("source '%s': neither \"url\" nor \"preset\"", source.id);
		}
		JsonSpan interval;
		if (json_member(item, "interval", interval)) {
			double seconds = 0;
			if (!json_number(interval, seconds) || seconds != std::floor(seconds) || seconds < 0 || seconds > 4e9) {
				return reject("source '%s': \"interval\" must be a whole number of seconds", source.id);
			}
			source.interval_s = static_cast<uint32_t>(seconds);
		}
		JsonSpan bindings;
		if (!json_member(item, "bindings", bindings) || bindings.kind != JsonSpan::Kind::array) {
			return reject("source '%s': \"bindings\" must be an array", source.id);
		}
		source.first = table.binding_count;
		size_t binding_position = 0;
		JsonSpan entry;
		size_t number = 0;
		while (json_next_item(bindings, binding_position, entry)) {
			++number;
			if (table.binding_count >= table.binding_capacity) {
				// The table is sized from the counts, capped at the limit.
				return reject("more than %zu bindings in all sources", kMaxBindings);
			}
			if (entry.kind != JsonSpan::Kind::object) {
				return reject("source '%s': binding %zu is not an object", source.id, number);
			}
			Table::Binding& out = table.bindings[table.binding_count];
			t = json_text(entry, "datapoint", out.datapoint_name, sizeof(out.datapoint_name));
			if (t != Text::ok) {
				return reject("source '%s', binding %zu: \"datapoint\" %s", source.id, number,
					t == Text::absent ? "is missing" : "must be a name of at most 63 bytes");
			}
			size_t p = 0;
			while (json_next_member(entry, p, name, value)) {
				if (!json_string_equals(name, "datapoint") && !json_string_equals(name, "path") &&
					!json_string_equals(name, "variable") && !json_string_equals(name, "scale") &&
					!json_string_equals(name, "offset") && !json_string_equals(name, "unit")) {
					char key[24];
					if (!json_string(name, key, sizeof(key))) {
						std::snprintf(key, sizeof(key), "?");
					}
					return reject("source '%s', datapoint '%s': unknown member \"%s\"", source.id, out.datapoint_name,
						key);
				}
			}
			out.variable = has_preset;
			const char* want = has_preset ? "variable" : "path";
			const char* other = has_preset ? "path" : "variable";
			JsonSpan unused;
			if (json_member(entry, other, unused)) {
				return reject("source '%s', datapoint '%s': %s", source.id, out.datapoint_name,
					has_preset ? "a preset source takes \"variable\", not \"path\""
							   : "a source with a URL takes \"path\", not \"variable\"");
			}
			t = json_text(entry, want, out.path, sizeof(out.path));
			if (t != Text::ok) {
				return reject("source '%s', datapoint '%s': \"%s\" %s", source.id, out.datapoint_name, want,
					t == Text::absent ? "is missing" : "must be a string of at most 63 bytes");
			}
			JsonSpan number_span;
			if (json_member(entry, "scale", number_span) && !json_number(number_span, out.scale)) {
				return reject("source '%s', datapoint '%s': \"scale\" must be a number", source.id, out.datapoint_name);
			}
			if (json_member(entry, "offset", number_span) && !json_number(number_span, out.offset)) {
				return reject("source '%s', datapoint '%s': \"offset\" must be a number", source.id, out.datapoint_name);
			}
			if (json_text(entry, "unit", out.unit, sizeof(out.unit)) == Text::invalid) {
				return reject("source '%s', datapoint '%s': \"unit\" must be a string of at most %zu bytes", source.id,
					out.datapoint_name, kMaxUnit);
			}
			++table.binding_count;
		}
		source.count = table.binding_count - source.first;
		table.source_count = index;
	}
	return true;
}

bool ExternalValues::finish(Table& table) {
	for (size_t i = 0; i < table.source_count; ++i) {
		Source& source = table.sources[i];
		if (!id_valid(source.id)) {
			return reject("source '%s': the id must be 1..15 characters of [A-Za-z0-9._-]", source.id);
		}
		for (size_t j = 0; j < i; ++j) {
			if (std::strcmp(table.sources[j].id, source.id) == 0) {
				return reject("source '%s' is defined twice", source.id);
			}
		}
		if (source.interval_s < kMinIntervalS || source.interval_s > kMaxIntervalS) {
			return reject("source '%s': interval %lu s outside %lu..%lu s (at least 5 minutes: the services' rate "
				"limits)", source.id, static_cast<unsigned long>(source.interval_s),
				static_cast<unsigned long>(kMinIntervalS), static_cast<unsigned long>(kMaxIntervalS));
		}
		if (source.count == 0) {
			return reject("source '%s' has no bindings", source.id);
		}
		if (source.preset[0] != '\0') {
			// Open-Meteo: resolve the variables, then build the URL from the parameters they need.
			char current[192] = {};
			char hourly[64] = {};
			unsigned hours = 0;
			for (size_t b = source.first; b < source.first + source.count; ++b) {
				Table::Binding& binding = table.bindings[b];
				const ExternalPresetVariable* variable = find_open_meteo(binding.path);
				if (variable == nullptr) {
					return reject("source '%s', datapoint '%s': unknown %s variable '%s' (doc/external.md lists them)",
						source.id, binding.datapoint_name, kOpenMeteo, binding.path);
				}
				char* list = variable->hourly ? hourly : current;
				size_t size = variable->hourly ? sizeof(hourly) : sizeof(current);
				// Add the parameter once.
				bool present = false;
				size_t qn = std::strlen(variable->query);
				for (const char* p = list; *p != '\0';) {
					const char* end = std::strchr(p, ',');
					size_t n = end != nullptr ? static_cast<size_t>(end - p) : std::strlen(p);
					if (n == qn && std::strncmp(p, variable->query, n) == 0) {
						present = true;
						break;
					}
					p += n;
					if (*p == ',') {
						++p;
					}
				}
				if (!present) {
					size_t used = std::strlen(list);
					std::snprintf(list + used, size - used, "%s%s", used > 0 ? "," : "", variable->query);
				}
				if (variable->hourly && variable->hour + 1u > hours) {
					hours = variable->hour + 1u;
				}
				std::snprintf(binding.path, sizeof(binding.path), "%s", variable->path);
				binding.variable = false;
				if (binding.unit[0] == '\0') {
					std::snprintf(binding.unit, sizeof(binding.unit), "%s", variable->unit);
				}
			}
			int n = std::snprintf(source.url, sizeof(source.url),
				"https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}%s%s%s%s",
				current[0] != '\0' ? "&current=" : "", current, hourly[0] != '\0' ? "&hourly=" : "", hourly);
			size_t used = n > 0 ? static_cast<size_t>(n) : 0;
			if (hours > 0 && used < sizeof(source.url)) {
				used += static_cast<size_t>(
					std::snprintf(source.url + used, sizeof(source.url) - used, "&forecast_hours=%u", hours));
			}
			if (used < sizeof(source.url)) {
				std::snprintf(source.url + used, sizeof(source.url) - used, "&wind_speed_unit=ms&timezone=UTC");
			}
		}
		const char* error = nullptr;
		if (!url_valid(source.url, source.placeholders, &error)) {
			return reject("source '%s': URL refused: %s", source.id, error);
		}
		for (size_t b = source.first; b < source.first + source.count; ++b) {
			Table::Binding& binding = table.bindings[b];
			if (!external_path_valid(binding.path, &error)) {
				return reject("source '%s', datapoint '%s': path '%s' refused: %s", source.id, binding.datapoint_name,
					binding.path, error);
			}
			if (!std::isfinite(binding.scale) || binding.scale == 0 || !std::isfinite(binding.offset)) {
				return reject("source '%s', datapoint '%s': scale must be a finite number other than 0, offset finite",
					source.id, binding.datapoint_name);
			}
			const Datapoint* dp = model_.find(binding.datapoint_name);
			if (dp == nullptr) {
				return reject("source '%s': no datapoint '%s' in the model", source.id, binding.datapoint_name);
			}
			if (!feedable(dp->type())) {
				return reject("source '%s': datapoint '%s' is of type %s; external values feed real, signed, "
					"unsigned, enumeration, boolean and string datapoints", source.id, binding.datapoint_name,
					value_type_name(dp->type()));
			}
			if (dp->technology() != nullptr && dp->technology() != this) {
				return reject("source '%s': datapoint '%s' is owned by %s", source.id, binding.datapoint_name,
					dp->technology()->name());
			}
			if (dp->writable() && !dp->transmits()) {
				return reject("source '%s': datapoint '%s' is writable without transmit: commands would change it "
					"without asking the source (clear the writable flag)", source.id, binding.datapoint_name);
			}
			binding.datapoint = dp->id();
			for (size_t k = 0; k < b; ++k) {
				if (table.bindings[k].datapoint == binding.datapoint) {
					return reject("datapoint '%s' is bound twice", binding.datapoint_name);
				}
			}
		}
	}
	return true;
}

bool ExternalValues::commit(Table* table, const char** error) {
	for (size_t b = 0; b < table->binding_count; ++b) {
		DatapointStatus status = model_.bind(table->bindings[b].datapoint, *this);
		if (status != DatapointStatus::ok) {
			// Checked before; only a model in a listener call gets here.
			reject("cannot bind datapoint '%s': %s", table->bindings[b].datapoint_name, datapoint_status_name(status));
			Table::destroy(table);
			return refuse(error);
		}
	}
	Table::destroy(table_);
	table_ = table;
	++generation_;
	uint64_t now = time_.monotonic_ms();
	for (size_t i = 0; i < table_->source_count; ++i) {
		Source& source = table_->sources[i];
		source.anchor = now + kStartDelayMs + i * kStaggerMs;
		source.due = source.anchor;
		Log::info(kTag, "source '%s': %zu value(s) every %lu s from %s%s%s", source.id, source.count,
			static_cast<unsigned long>(source.interval_s), source.url, source.preset[0] != '\0' ? " (preset " : "",
			source.preset[0] != '\0' ? "open-meteo)" : "");
	}
	arm();
	return true;
}

bool ExternalValues::configure(const ExternalSourceSpec* specs, size_t count, const char** error) {
	if (model_.notifying()) {
		reject("called from a datapoint listener");
		return refuse(error);
	}
	size_t bindings = 0;
	for (size_t i = 0; specs != nullptr && i < count; ++i) {
		bindings += specs[i].binding_count;
	}
	size_t bytes = 0;
	Table* table = Table::create(count < kMaxSources ? count : kMaxSources,
		bindings < kMaxBindings ? bindings : kMaxBindings, bytes);
	if (table == nullptr) {
		reject("no memory for the source table (%zu bytes)", bytes);
		return refuse(error);
	}
	if (!fill(*table, specs, count) || !finish(*table)) {
		Table::destroy(table);
		return refuse(error);
	}
	return commit(table, error);
}

bool ExternalValues::configure_json(const char* text, size_t length, const char** error) {
	if (model_.notifying()) {
		reject("called from a datapoint listener");
		return refuse(error);
	}
	// Count first, so the table holds exactly what the member declares (capped at the limits; a
	// larger count is refused by fill_json() with its reason).
	size_t sources = 0;
	size_t bindings = 0;
	{
		JsonSpan root;
		JsonSpan list;
		if (json_parse(text, length, root) && root.kind == JsonSpan::Kind::object && json_member(root, "sources", list) &&
			list.kind == JsonSpan::Kind::array) {
			size_t position = 0;
			JsonSpan item;
			while (json_next_item(list, position, item)) {
				++sources;
				JsonSpan entries;
				if (item.kind == JsonSpan::Kind::object && json_member(item, "bindings", entries) &&
					entries.kind == JsonSpan::Kind::array) {
					size_t at = 0;
					JsonSpan entry;
					while (json_next_item(entries, at, entry)) {
						++bindings;
					}
				}
			}
		}
	}
	size_t bytes = 0;
	Table* table = Table::create(sources < kMaxSources ? sources : kMaxSources,
		bindings < kMaxBindings ? bindings : kMaxBindings, bytes);
	if (table == nullptr) {
		reject("no memory for the source table (%zu bytes)", bytes);
		return refuse(error);
	}
	if (!fill_json(*table, text, length) || !finish(*table)) {
		Table::destroy(table);
		return refuse(error);
	}
	return commit(table, error);
}

void ExternalValues::clear() {
	loop_.cancel(timer_);
	timer_ = TimerHandle();
	Table::destroy(table_);
	table_ = nullptr;
	++generation_;
}

size_t ExternalValues::size() const {
	return table_ != nullptr ? table_->source_count : 0;
}

bool ExternalValues::status(size_t index, ExternalSourceStatus& out) const {
	if (table_ == nullptr || index >= table_->source_count) {
		return false;
	}
	const Source& source = table_->sources[index];
	out.id = source.id;
	out.url = source.url;
	out.preset = source.preset;
	out.interval_s = source.interval_s;
	out.bindings = source.count;
	out.fetching = in_flight_ && in_flight_generation_ == generation_ && in_flight_source_ == index;
	out.failures = source.failures;
	out.fetches = source.fetches;
	out.next_ms = source.due;
	out.last_ok_ms = source.last_ok;
	out.last_error = source.last_error;
	return true;
}

bool ExternalValues::binding(size_t index, size_t number, ExternalBindingStatus& out) const {
	if (table_ == nullptr || index >= table_->source_count || number >= table_->sources[index].count) {
		return false;
	}
	const Table::Binding& b = table_->bindings[table_->sources[index].first + number];
	out.datapoint = b.datapoint;
	out.path = b.path;
	out.unit = b.unit;
	out.scale = b.scale;
	out.offset = b.offset;
	return true;
}

bool ExternalValues::command(const Datapoint& datapoint, const Value&) {
	const char* source = "?";
	if (table_ != nullptr) {
		for (size_t i = 0; i < table_->source_count; ++i) {
			const Source& s = table_->sources[i];
			for (size_t b = s.first; b < s.first + s.count; ++b) {
				if (table_->bindings[b].datapoint == datapoint.id()) {
					source = s.id;
				}
			}
		}
	}
	Log::warn(kTag, "refusing command to '%s': an external value (source '%s') is read only", datapoint.name(), source);
	return false;
}

bool ExternalValues::read(const Datapoint& datapoint) {
	Source* source = nullptr;
	if (table_ != nullptr) {
		for (size_t i = 0; i < table_->source_count && source == nullptr; ++i) {
			Source& s = table_->sources[i];
			for (size_t b = s.first; b < s.first + s.count; ++b) {
				if (table_->bindings[b].datapoint == datapoint.id()) {
					source = &s;
				}
			}
		}
	}
	if (source == nullptr) {
		Log::warn(kTag, "refusing read of '%s': no source feeds it", datapoint.name());
		return false;
	}
	uint64_t now = time_.monotonic_ms();
	const char* blocked = source_blocked(*source, now);
	if (blocked != nullptr) {
		Log::warn(kTag, "refusing read of '%s' (source '%s'): %s", datapoint.name(), source->id, blocked);
		return false;
	}
	if (in_flight_ && in_flight_generation_ == generation_ && table_->sources + in_flight_source_ == source) {
		return true; // the answer is on its way
	}
	// The whole source is fetched again at its next opportunity: now, or when the rate limit of
	// fetch_now() allows; reads that come meanwhile join that fetch.
	uint64_t at = now;
	if (source->attempted && now - source->last_attempt < kManualSpacingMs) {
		at = source->last_attempt + kManualSpacingMs;
	}
	if (at < source->due) {
		Log::info(kTag, "source '%s': fetch requested by a read of '%s'", source->id, datapoint.name());
		source->due = at;
		source->waiting = false;
		arm();
	}
	return true;
}

// ---- Scheduling -----------------------------------------------------------------------------

uint64_t ExternalValues::next_grid(const Source& source, uint64_t now) const {
	uint64_t period = static_cast<uint64_t>(source.interval_s) * 1000u;
	if (now < source.anchor) {
		return source.anchor;
	}
	return source.anchor + ((now - source.anchor) / period + 1) * period;
}

const char* ExternalValues::source_blocked(const Source& source, uint64_t) const {
	if (!connectivity_.is_up()) {
		return "not fetched: the network is down";
	}
	if (!time_.synchronized()) {
		return "not fetched: the clock is not synchronized (TLS needs the date)";
	}
	if (source.placeholders) {
		double value = 0;
		if (!read_coordinate(settings_, setting::kLatitude, -90, 90, value) ||
			!read_coordinate(settings_, setting::kLongitude, -180, 180, value)) {
			return "not fetched: no valid location (settings location.lat, location.lon)";
		}
	}
	return nullptr;
}

bool ExternalValues::fill_url(const Source& source, char* out, size_t size, const char** error) const {
	double lat = 0;
	double lon = 0;
	if (source.placeholders && (!read_coordinate(settings_, setting::kLatitude, -90, 90, lat) ||
		!read_coordinate(settings_, setting::kLongitude, -180, 180, lon))) {
		return set_error(error, "no valid location (settings location.lat, location.lon)");
	}
	size_t used = 0;
	for (const char* p = source.url; *p != '\0'; ++p) {
		char piece[16];
		size_t n = 0;
		if (*p == '{') {
			// Two decimals (about 1 km): what a weather grid resolves, and no more of the location.
			n = static_cast<size_t>(std::snprintf(piece, sizeof(piece), "%.2f", p[2] == 'a' ? lat : lon));
			p += 4;
		} else {
			piece[0] = *p;
			n = 1;
		}
		if (used + n >= size) {
			return set_error(error, "the filled URL is too long");
		}
		std::memcpy(out + used, piece, n);
		used += n;
	}
	out[used] = '\0';
	return true;
}

void ExternalValues::arm() {
	loop_.cancel(timer_);
	timer_ = TimerHandle();
	if (table_ == nullptr && !in_flight_) {
		return;
	}
	uint64_t now = time_.monotonic_ms();
	uint64_t next = EventLoop::kNever;
	bool waiting = false;
	if (table_ != nullptr) {
		for (size_t i = 0; i < table_->source_count; ++i) {
			const Source& source = table_->sources[i];
			if (source.due < next) {
				next = source.due;
			}
			waiting = waiting || source.waiting;
		}
	}
	if (waiting && now + kReadyPollMs < next) {
		next = now + kReadyPollMs;
	}
	if (in_flight_) {
		// The sources wait for the result; only the guard against a lost result runs.
		next = in_flight_deadline_;
	}
	if (next != EventLoop::kNever) {
		timer_ = loop_.schedule_at(next, Callback::member<ExternalValues, &ExternalValues::timer_due>(this));
	}
}

void ExternalValues::timer_due() {
	timer_ = TimerHandle();
	service();
}

void ExternalValues::service() {
	uint64_t now = time_.monotonic_ms();
	if (in_flight_) {
		if (now < in_flight_deadline_) {
			arm();
			return;
		}
		in_flight_ = false;
		free_response();
		if (table_ != nullptr && in_flight_generation_ == generation_) {
			char reason[80];
			std::snprintf(reason, sizeof(reason), "no result within %lu s",
				static_cast<unsigned long>(kFetchTimeoutMs / 1000));
			fail(table_->sources[in_flight_source_], reason, true);
		}
	}
	if (table_ == nullptr) {
		arm();
		return;
	}
	// Sources that missed a due time for a missing condition fetch soon after it cleared.
	size_t rank = 0;
	for (size_t i = 0; i < table_->source_count; ++i) {
		Source& source = table_->sources[i];
		if (source.waiting && source_blocked(source, now) == nullptr) {
			source.waiting = false;
			source.anchor = now + kStartDelayMs + rank * kStaggerMs;
			source.due = source.anchor;
			++rank;
		}
	}
	for (;;) {
		Source* next = nullptr;
		for (size_t i = 0; i < table_->source_count; ++i) {
			Source& source = table_->sources[i];
			if (source.due <= now && (next == nullptr || source.due < next->due)) {
				next = &source;
			}
		}
		if (next == nullptr) {
			break;
		}
		const char* blocked = source_blocked(*next, now);
		if (blocked != nullptr) {
			// A missed fetch: counts as a failure, then the grid goes on; the fetch follows when the
			// condition clears.
			fail(*next, blocked, false);
			next->waiting = true;
			continue;
		}
		if (start(*next, now)) {
			break;
		}
	}
	arm();
}

bool ExternalValues::start(Source& source, uint64_t now) {
	source.last_attempt = now;
	source.attempted = true;
	const char* error = nullptr;
	if (response_ == nullptr) {
		response_ = new (std::nothrow) char[kMaxBody];
	}
	if (response_ == nullptr) {
		error = "no memory for the response buffer (8192 bytes)";
	} else {
		body_ = FetchBody(response_, kMaxBody, "the response");
	}
	if (response_ == nullptr || !fill_url(source, url_, sizeof(url_), &error) || !fetcher_.fetch(url_, *this, &error)) {
		free_response();
		char reason[96];
		std::snprintf(reason, sizeof(reason), "fetch not started: %s", error != nullptr ? error : "?");
		fail(source, reason, true);
		return false;
	}
	in_flight_ = true;
	in_flight_generation_ = generation_;
	in_flight_source_ = static_cast<size_t>(&source - table_->sources);
	in_flight_deadline_ = now + kFetchTimeoutMs;
	Log::debug(kTag, "source '%s': fetching %s", source.id, url_);
	return true;
}

void ExternalValues::free_response() {
	delete[] response_;
	response_ = nullptr;
	body_ = FetchBody(nullptr, 0, "the response");
}

bool ExternalValues::wanted() const {
	return in_flight_ && table_ != nullptr && in_flight_generation_ == generation_ && response_ != nullptr;
}

bool ExternalValues::fetch_begin(int http_status, size_t content_length) {
	// Only a 200 is read; fetch_end() names another status, and the body's refusal below.
	return wanted() && http_status == 200 && body_.begin(content_length);
}

bool ExternalValues::fetch_chunk(const char* data, size_t size) {
	return wanted() && body_.append(data, size);
}

void ExternalValues::fetch_end(const FetchResult& result) {
	if (!in_flight_) {
		Log::warn(kTag, "a fetch result arrived after its timeout: ignored");
		return;
	}
	in_flight_ = false;
	if (table_ == nullptr || in_flight_generation_ != generation_) {
		Log::info(kTag, "the result of a fetch for a replaced configuration is dropped");
		free_response();
		arm();
		return;
	}
	Source& source = table_->sources[in_flight_source_];
	if (!result.ok) {
		char reason[112];
		if (result.aborted && body_.refusal()[0] != '\0') {
			std::snprintf(reason, sizeof(reason), "%s", body_.refusal());
		} else if (result.error != nullptr && result.error[0] != '\0') {
			std::snprintf(reason, sizeof(reason), "%s", result.error);
		} else {
			std::snprintf(reason, sizeof(reason), "HTTP status %d", result.http_status);
		}
		fail(source, reason, true);
	} else {
		process(source, body_.data(), body_.length());
	}
	free_response();
	arm();
}

void ExternalValues::process(Source& source, const char* body, size_t length) {
	JsonSpan root;
	const char* error = nullptr;
	char reason[112];
	if (body == nullptr || !json_parse(body, length, root, &error)) {
		std::snprintf(reason, sizeof(reason), "the response is not JSON: %s", error != nullptr ? error : "empty");
		fail(source, reason, true);
		return;
	}
	for (size_t b = source.first; b < source.first + source.count; ++b) {
		const Table::Binding& binding = table_->bindings[b];
		const Datapoint* dp = model_.get(binding.datapoint);
		JsonSpan span;
		size_t at = 0;
		if (!external_extract(root, binding.path, span, &error, &at)) {
			std::snprintf(reason, sizeof(reason), "path '%s': %s at '%s'", binding.path, error, binding.path + at);
			fail(source, reason, true);
			return;
		}
		if (dp == nullptr) {
			std::snprintf(reason, sizeof(reason), "datapoint %u is gone (the model was reloaded: configure again)",
				static_cast<unsigned>(binding.datapoint));
			fail(source, reason, true);
			return;
		}
		if (!to_value(span, dp->type(), binding.scale, binding.offset, table_->pending[b], &error)) {
			std::snprintf(reason, sizeof(reason), "path '%s': %s", binding.path, error);
			fail(source, reason, true);
			return;
		}
	}
	for (size_t b = source.first; b < source.first + source.count; ++b) {
		DatapointStatus status = model_.report(*this, table_->bindings[b].datapoint, table_->pending[b]);
		if (status != DatapointStatus::ok) {
			Log::error(kTag, "source '%s': report of '%s' refused: %s", source.id, table_->bindings[b].datapoint_name,
				datapoint_status_name(status));
		}
	}
	succeed(source, time_.monotonic_ms());
}

void ExternalValues::succeed(Source& source, uint64_t now) {
	if (source.failures > 0) {
		Log::info(kTag, "source '%s': recovered after %lu failure(s) (%s)", source.id,
			static_cast<unsigned long>(source.failures), source.last_error);
	} else if (source.fetches == 0) {
		Log::info(kTag, "source '%s': first fetch, %zu value(s)", source.id, source.count);
	}
	source.failures = 0;
	source.last_error[0] = '\0';
	source.backoff.reset();
	source.last_ok = now;
	++source.fetches;
	source.due = next_grid(source, now);
	Log::debug(kTag, "source '%s': %zu value(s) reported; next in %llu s", source.id, source.count,
		static_cast<unsigned long long>((source.due - now) / 1000));
}

void ExternalValues::fail(Source& source, const char* reason, bool retry) {
	uint64_t now = time_.monotonic_ms();
	++source.failures;
	if (std::strncmp(source.last_error, reason, sizeof(source.last_error) - 1) != 0) {
		// Logged once per change of the reason.
		Log::write(retry ? LogLevel::warn : LogLevel::info, kTag, "source '%s': %s", source.id, reason);
		std::snprintf(source.last_error, sizeof(source.last_error), "%s", reason);
	}
	if (source.failures == kStaleAfterFailures) {
		size_t stale = 0;
		for (size_t b = source.first; b < source.first + source.count; ++b) {
			const Datapoint* dp = model_.get(table_->bindings[b].datapoint);
			if (dp != nullptr && dp->quality() == Quality::good) {
				++stale;
			}
			model_.report_stale(*this, table_->bindings[b].datapoint);
		}
		Log::warn(kTag, "source '%s': %lu failures in a row: its %zu value(s) are stale", source.id,
			static_cast<unsigned long>(source.failures), stale);
	}
	if (retry) {
		source.due = now + source.backoff.next();
	} else {
		source.due = next_grid(source, now);
	}
}

bool ExternalValues::fetch_now(const char* id, const char** error) {
	if (table_ == nullptr) {
		return set_error(error, "no external sources configured");
	}
	Source* source = nullptr;
	for (size_t i = 0; i < table_->source_count; ++i) {
		if (std::strcmp(table_->sources[i].id, id) == 0) {
			source = &table_->sources[i];
		}
	}
	if (source == nullptr) {
		return set_error(error, "no source with this id");
	}
	uint64_t now = time_.monotonic_ms();
	const char* blocked = source_blocked(*source, now);
	if (blocked != nullptr) {
		return set_error(error, blocked);
	}
	if (in_flight_ && in_flight_generation_ == generation_ && table_->sources + in_flight_source_ == source) {
		return set_error(error, "its fetch is in flight");
	}
	if (source->attempted && now - source->last_attempt < kManualSpacingMs) {
		return set_error(error, "its last attempt was less than a minute ago (rate limit)");
	}
	Log::info(kTag, "source '%s': fetch requested", source->id);
	source->due = now;
	source->waiting = false;
	service();
	return true;
}

} // namespace hc
