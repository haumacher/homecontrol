/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/image/loader.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "hc/json_document.h"
#include "hc/mqtt.h"
#include "hc/network_settings.h"
#include "hc/rules/rule_set.h"
#include "hc/tz.h"
#include "hc/zigbee/adapter.h"

namespace hc {
namespace image {

// ---- Common ----------------------------------------------------------------------------------

const char* source_name(Source source) {
	switch (source) {
		case Source::boot: return "boot";
		case Source::api: return "api";
		case Source::console: return "console";
		case Source::file: return "file";
		case Source::restore: return "restore";
	}
	return "?";
}

const char* report_kind_name(Report::Kind kind) {
	switch (kind) {
		case Report::Kind::none: return "none";
		case Report::Kind::syntax: return "syntax";
		case Report::Kind::version: return "version";
		case Report::Kind::invalid: return "invalid";
		case Report::Kind::too_large: return "too large";
		case Report::Kind::no_memory: return "no memory";
		case Report::Kind::apply: return "apply";
	}
	return "?";
}

void Report::add(Kind failure, const char* path, const char* format, ...) {
	if (kind == Kind::none) {
		kind = failure;
	}
	if (count < kMaxRefusals) {
		Refusal& r = refusals[count++];
		std::snprintf(r.path, sizeof(r.path), "%s", path != nullptr ? path : "");
		va_list args;
		va_start(args, format);
		std::vsnprintf(r.reason, sizeof(r.reason), format, args);
		va_end(args);
	}
	++total;
}

// ---- Arena -----------------------------------------------------------------------------------

void* Arena::allocate(size_t size) {
	size = (size + 7) & ~static_cast<size_t>(7);
	if (size == 0) {
		size = 8;
	}
	if (head_ == nullptr || head_->size - head_->used < size) {
		size_t header = (sizeof(Block) + 7) & ~static_cast<size_t>(7);
		size_t capacity = size > kBlockSize ? size : kBlockSize;
		void* memory = ::operator new(header + capacity, std::nothrow);
		if (memory == nullptr) {
			return nullptr;
		}
		Block* block = static_cast<Block*>(memory);
		block->next = head_;
		block->size = capacity;
		block->used = 0;
		head_ = block;
		bytes_ += header + capacity;
	}
	size_t header = (sizeof(Block) + 7) & ~static_cast<size_t>(7);
	char* base = reinterpret_cast<char*>(head_) + header + head_->used;
	head_->used += size;
	std::memset(base, 0, size);
	return base;
}

char* Arena::copy(const char* text, size_t length) {
	char* out = static_cast<char*>(allocate(length + 1));
	if (out != nullptr) {
		std::memcpy(out, text, length);
		out[length] = '\0';
	}
	return out;
}

void Arena::release() {
	while (head_ != nullptr) {
		Block* next = head_->next;
		::operator delete(static_cast<void*>(head_));
		head_ = next;
	}
	bytes_ = 0;
}

bool Plan::has_schedule(const char* id) const {
	for (size_t i = 0; i < schedule_count; ++i) {
		if (schedules[i].id != nullptr && std::strcmp(schedules[i].id, id) == 0) {
			return true;
		}
	}
	return false;
}

bool Plan::schedule_owns_output(const char* datapoint) const {
	for (size_t i = 0; i < schedule_count; ++i) {
		if (schedules[i].output != nullptr && std::strcmp(schedules[i].output, datapoint) == 0) {
			return true;
		}
	}
	return false;
}

// ---- Value types -------------------------------------------------------------------------------

namespace {

struct TypeName {
	const char* name;
	ValueType type;
};

constexpr TypeName kTypes[] = {
	{"boolean", ValueType::boolean},
	{"unsigned", ValueType::unsigned_int},
	{"signed", ValueType::signed_int},
	{"real", ValueType::real},
	{"string", ValueType::string},
	{"timeOfDay", ValueType::time_of_day},
	{"date", ValueType::date},
	{"dateTime", ValueType::date_time},
	{"enumeration", ValueType::enumeration},
	{"raw", ValueType::raw},
	{"controlled", ValueType::controlled},
	{"color", ValueType::color},
};

} // namespace

ValueType value_type_of(const char* name) {
	for (const TypeName& t : kTypes) {
		if (std::strcmp(t.name, name) == 0) {
			return t.type;
		}
	}
	return ValueType::none;
}

const char* value_type_label(ValueType type) {
	for (const TypeName& t : kTypes) {
		if (t.type == type) {
			return t.name;
		}
	}
	return "none";
}

// ---- The parser -------------------------------------------------------------------------------

namespace {

constexpr const char* kTag = "hc.image";

/// The datapoints the time master implies (doc/knx.md, "Time master").
constexpr const char* kTimeDatapoint = "knx.time";
constexpr const char* kDateDatapoint = "knx.date";
constexpr const char* kDateTimeDatapoint = "knx.datetime";

constexpr size_t kMaxText = Settings::kMaxStringLength; ///< Longest string member read (127 bytes).

bool numeric(ValueType type) {
	return type == ValueType::unsigned_int || type == ValueType::signed_int || type == ValueType::real ||
		type == ValueType::enumeration;
}

bool printable(const char* text, size_t length) {
	for (size_t i = 0; i < length; ++i) {
		unsigned char c = static_cast<unsigned char>(text[i]);
		if (c < 0x20 || c == 0x7F) {
			return false;
		}
	}
	return true;
}

/// 1..max characters of A-Z a-z 0-9 _ - (and '.', when `dot`).
bool id_valid(const char* text, size_t max, bool dot) {
	size_t length = std::strlen(text);
	if (length == 0 || length > max) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		char c = text[i];
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
			(dot && c == '.');
		if (!ok) {
			return false;
		}
	}
	return true;
}

struct Ctx;

/// Appends a segment to the current path for its lifetime.
struct Path {
	Path(Ctx& ctx, const char* format, ...) HC_PRINTF_FORMAT(3, 4);
	~Path();
	Ctx& ctx;
	size_t saved;
};

struct Ctx {
	Plan& plan;
	Report& report;
	char path[Report::kMaxPath + 1] = {};
	size_t path_length = 0;

	Ctx(Plan& p, Report& r) : plan(p), report(r) {}

	void refuse(const char* format, ...) HC_PRINTF_FORMAT(2, 3) {
		char reason[Report::kMaxReason + 1];
		va_list args;
		va_start(args, format);
		std::vsnprintf(reason, sizeof(reason), format, args);
		va_end(args);
		report.add(Report::Kind::invalid, path, "%s", reason);
	}

	void no_memory(const char* what) {
		report.add(Report::Kind::no_memory, path, "no memory for %s", what);
	}

	bool stopped() const { return report.full() || report.kind == Report::Kind::no_memory; }

	bool member(const JsonSpan& object, const char* name, JsonSpan& out) const {
		return json_member(object, name, out);
	}

	/// The member must be an object; refuses otherwise.
	bool object(const JsonSpan& span, const char* what) {
		if (span.kind != JsonSpan::Kind::object) {
			refuse("%s must be an object", what);
			return false;
		}
		return true;
	}

	bool array(const JsonSpan& span, const char* what) {
		if (span.kind != JsonSpan::Kind::array) {
			refuse("%s must be an array", what);
			return false;
		}
		return true;
	}

	/// A string of min..max bytes (printable if `plain`) into `out` (size > max).
	bool string(const JsonSpan& span, char* out, size_t size, size_t min, size_t max, bool plain, const char* what) {
		size_t length = 0;
		if (span.kind != JsonSpan::Kind::string) {
			refuse("%s must be a string", what);
			return false;
		}
		if (!json_string(span, out, size, &length)) {
			refuse("%s is longer than %u bytes or not a valid string", what, static_cast<unsigned>(max));
			return false;
		}
		if (length < min || length > max) {
			refuse("%s must have %u..%u bytes", what, static_cast<unsigned>(min), static_cast<unsigned>(max));
			return false;
		}
		if (plain && !printable(out, length)) {
			refuse("%s contains a control character", what);
			return false;
		}
		return true;
	}

	/// The same into the arena; nullptr after a refusal.
	char* arena_string(const JsonSpan& span, size_t min, size_t max, const char* what) {
		char buffer[kMaxText + 1];
		if (!string(span, buffer, sizeof(buffer), min, max, true, what)) {
			return nullptr;
		}
		char* copy = plan.arena.copy(buffer, std::strlen(buffer));
		if (copy == nullptr) {
			no_memory("a string");
		}
		return copy;
	}

	bool integer(const JsonSpan& span, double min, double max, int64_t& out, const char* what) {
		double value = 0;
		if (span.kind != JsonSpan::Kind::number || !json_number(span, value) || std::floor(value) != value) {
			refuse("%s must be an integer", what);
			return false;
		}
		if (value < min || value > max) {
			refuse("%s must be %.0f..%.0f", what, min, max);
			return false;
		}
		out = static_cast<int64_t>(value);
		return true;
	}

	bool number(const JsonSpan& span, double min, double max, double& out, const char* what) {
		if (span.kind != JsonSpan::Kind::number || !json_number(span, out)) {
			refuse("%s must be a number", what);
			return false;
		}
		if (out < min || out > max) {
			refuse("%s must be %g..%g", what, min, max);
			return false;
		}
		return true;
	}

	bool boolean(const JsonSpan& span, bool& out, const char* what) {
		if (span.kind != JsonSpan::Kind::boolean || !json_bool(span, out)) {
			refuse("%s must be true or false", what);
			return false;
		}
		return true;
	}

	/// An optional boolean member.
	void flag(const JsonSpan& object, const char* name, bool& out) {
		JsonSpan value;
		if (member(object, name, value)) {
			Path p(*this, ".%s", name);
			boolean(value, out, name);
		}
	}

	/// A label: a string of 1..63 printable bytes, or an object from language tags to such strings
	/// (decision ticket #28: the image carries the installation's texts, the UI picks a language).
	bool label(const JsonSpan& span) {
		char buffer[kMaxName + 1];
		if (span.kind == JsonSpan::Kind::string) {
			return string(span, buffer, sizeof(buffer), 1, kMaxName, true, "the label");
		}
		if (span.kind != JsonSpan::Kind::object) {
			refuse("the label must be a string or an object of language tags to strings");
			return false;
		}
		size_t position = 0;
		JsonSpan key;
		JsonSpan value;
		size_t entries = 0;
		bool ok = true;
		while (json_next_member(span, position, key, value)) {
			char tag[16];
			size_t length = 0;
			if (!json_string(key, tag, sizeof(tag), &length) || length < 2 || length > 8 ||
				!id_valid(tag, 8, false)) {
				refuse("a label's language tag must have 2..8 characters of A-Z a-z 0-9 -");
				ok = false;
				continue;
			}
			Path p(*this, ".%s", tag);
			ok = string(value, buffer, sizeof(buffer), 1, kMaxName, true, "the label") && ok;
			++entries;
		}
		if (entries == 0 && ok) {
			refuse("the label names no language");
			return false;
		}
		return ok;
	}

	bool group_address(const JsonSpan& span, knx::GroupAddress& out, const char* what) {
		char text[24];
		if (!string(span, text, sizeof(text), 1, 15, true, what)) {
			return false;
		}
		const char* error = nullptr;
		std::optional<knx::GroupAddress> address = knx::GroupAddress::parse(text, &error);
		if (!address) {
			refuse("%s \"%s\" is not a group address (%s)", what, text, error != nullptr ? error : "invalid");
			return false;
		}
		out = *address;
		return true;
	}

	/// Members of a strict object (schedules): those listed, plus `description` and `comment`.
	bool only(const JsonSpan& object, const char* const* allowed) {
		size_t position = 0;
		JsonSpan key;
		JsonSpan value;
		bool ok = true;
		while (json_next_member(object, position, key, value)) {
			bool known = json_string_equals(key, "description") || json_string_equals(key, "comment");
			for (const char* const* a = allowed; *a != nullptr && !known; ++a) {
				known = json_string_equals(key, *a);
			}
			if (!known) {
				char name[40];
				if (!json_string(key, name, sizeof(name))) {
					std::snprintf(name, sizeof(name), "?");
				}
				refuse("malformed: unknown member '%s'", name);
				ok = false;
			}
		}
		return ok;
	}
};

Path::Path(Ctx& c, const char* format, ...) : ctx(c), saved(c.path_length) {
	va_list args;
	va_start(args, format);
	size_t room = sizeof(c.path) - c.path_length;
	int n = std::vsnprintf(c.path + c.path_length, room, format, args);
	va_end(args);
	if (n > 0) {
		c.path_length += static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room - 1;
	}
}

Path::~Path() {
	ctx.path_length = saved;
	ctx.path[saved] = '\0';
}

// ---- Image members ----------------------------------------------------------------------------

/// "1.0" -> 1, 0; false if malformed.
bool parse_version(const char* text, uint32_t& major, uint32_t& minor) {
	const char* p = text;
	auto number = [&p](uint32_t& out) {
		if (*p < '0' || *p > '9') {
			return false;
		}
		uint32_t value = 0;
		while (*p >= '0' && *p <= '9') {
			value = value * 10 + static_cast<uint32_t>(*p - '0');
			if (value > 9999) {
				return false;
			}
			++p;
		}
		out = value;
		return true;
	};
	if (!number(major) || *p != '.') {
		return false;
	}
	++p;
	return number(minor) && *p == '\0';
}

/// The `image` member. False when nothing else should be checked (a newer major version).
bool parse_image(Ctx& c, const JsonSpan& root) {
	Path p(c, "image");
	JsonSpan image;
	if (!c.member(root, "image", image)) {
		c.refuse("the member 'image' (schemaVersion, projectId, revision) is missing");
		return false;
	}
	if (!c.object(image, "'image'")) {
		return false;
	}
	ImageInfo& info = c.plan.info;
	JsonSpan value;
	{
		Path q(c, ".schemaVersion");
		if (!c.member(image, "schemaVersion", value)) {
			c.refuse("schemaVersion is missing");
			return false;
		}
		if (!c.string(value, info.schema_version, sizeof(info.schema_version), 3, kMaxVersionText, true,
				"schemaVersion")) {
			return false;
		}
		uint32_t major = 0;
		uint32_t minor = 0;
		if (!parse_version(info.schema_version, major, minor) || major == 0) {
			c.refuse("schemaVersion \"%s\" is not <major>.<minor> with a major version from 1", info.schema_version);
			return false;
		}
		if (major > kSchemaMajor) {
			c.report.add(Report::Kind::version, c.path,
				"schemaVersion %s is newer than this firmware reads (%u.x, up to %u.%u): update the firmware",
				info.schema_version, static_cast<unsigned>(kSchemaMajor), static_cast<unsigned>(kSchemaMajor),
				static_cast<unsigned>(kSchemaMinor));
			return false;
		}
	}
	{
		Path q(c, ".projectId");
		if (!c.member(image, "projectId", value)) {
			c.refuse("projectId is missing");
		} else {
			c.string(value, info.project_id, sizeof(info.project_id), 1, kMaxProjectId, true, "projectId");
		}
	}
	{
		Path q(c, ".revision");
		int64_t revision = 0;
		if (!c.member(image, "revision", value)) {
			c.refuse("revision is missing");
		} else if (c.integer(value, 0, 4294967295.0, revision, "revision")) {
			info.revision = static_cast<uint32_t>(revision);
		}
	}
	if (c.member(image, "generatedAt", value)) {
		Path q(c, ".generatedAt");
		c.string(value, info.generated_at, sizeof(info.generated_at), 1, kMaxGeneratedAt, true, "generatedAt");
	}
	if (c.member(image, "generator", value)) {
		Path q(c, ".generator");
		c.string(value, info.generator, sizeof(info.generator), 1, kMaxGenerator, true, "generator");
	}
	if (c.member(image, "name", value)) {
		Path q(c, ".name");
		c.string(value, info.name, sizeof(info.name), 1, kMaxName, true, "name");
	}
	return true;
}

void parse_settings(Ctx& c, const JsonSpan& root) {
	JsonSpan settings;
	if (!c.member(root, "settings", settings)) {
		return;
	}
	Path p(c, "settings");
	if (!c.object(settings, "'settings'")) {
		return;
	}
	Plan& plan = c.plan;
	JsonSpan value;
	if (c.member(settings, "tz", value)) {
		Path q(c, ".tz");
		if (c.string(value, plan.tz, sizeof(plan.tz), 1, Settings::kMaxStringLength, true, "tz")) {
			const char* error = nullptr;
			if (!posix_tz_valid(plan.tz, &error)) {
				c.refuse("tz \"%s\" is not a POSIX TZ string: %s", plan.tz, error != nullptr ? error : "invalid");
			} else {
				plan.has_tz = true;
			}
		}
	}
	if (c.member(settings, "location", value)) {
		Path q(c, ".location");
		if (c.object(value, "location")) {
			JsonSpan lat;
			JsonSpan lon;
			JsonSpan elev;
			bool ok = true;
			if (!c.member(value, "lat", lat) || !c.member(value, "lon", lon)) {
				c.refuse("location needs lat and lon");
				ok = false;
			} else {
				Path a(c, ".lat");
				ok = c.number(lat, -90, 90, plan.location.latitude, "lat") && ok;
			}
			if (ok) {
				Path a(c, ".lon");
				ok = c.number(lon, -180, 180, plan.location.longitude, "lon") && ok;
			}
			plan.location.elevation_m = 0;
			if (c.member(value, "elev", elev)) {
				Path a(c, ".elev");
				int64_t metres = 0;
				if (c.integer(elev, -500, 9000, metres, "elev")) {
					plan.location.elevation_m = static_cast<double>(metres);
				} else {
					ok = false;
				}
			}
			plan.has_location = ok;
		}
	}
	if (c.member(settings, "ntp", value)) {
		Path q(c, ".ntp");
		JsonSpan server;
		if (c.object(value, "ntp")) {
			if (!c.member(value, "server", server)) {
				c.refuse("ntp needs server");
			} else {
				Path a(c, ".server");
				if (c.string(server, plan.ntp, sizeof(plan.ntp), 1, Settings::kMaxStringLength, true, "server")) {
					const char* error = nullptr;
					if (!host_name_valid(plan.ntp, &error)) {
						c.refuse("server \"%s\" is not a host name: %s", plan.ntp, error != nullptr ? error : "invalid");
					} else {
						plan.has_ntp = true;
					}
				}
			}
		}
	}
}

/// The `knx` member: the time master. Its datapoints are added after the image's.
void parse_knx(Ctx& c, const JsonSpan& root) {
	JsonSpan knx_member;
	if (!c.member(root, "knx", knx_member)) {
		return;
	}
	Path p(c, "knx");
	if (!c.object(knx_member, "'knx'")) {
		return;
	}
	JsonSpan tm;
	if (!c.member(knx_member, "timeMaster", tm)) {
		return;
	}
	Path q(c, ".timeMaster");
	if (!c.object(tm, "timeMaster")) {
		return;
	}
	knx::TimeMasterConfig& config = c.plan.time_master;
	struct Output {
		const char* member;
		knx::TimeMasterConfig::Output& output;
		const char* datapoint;
	};
	Output outputs[] = {
		{"time", config.time, kTimeDatapoint},
		{"date", config.date, kDateDatapoint},
		{"dateTime", config.date_time, kDateTimeDatapoint},
	};
	size_t count = 0;
	size_t named = 0;
	JsonSpan value;
	for (Output& o : outputs) {
		if (!c.member(tm, o.member, value)) {
			continue;
		}
		++named;
		Path a(c, ".%s", o.member);
		if (c.group_address(value, o.output.address, o.member)) {
			o.output.enabled = true;
			o.output.datapoint = o.datapoint;
			++count;
		}
	}
	if (c.member(tm, "interval", value)) {
		Path a(c, ".interval");
		int64_t seconds = 0;
		if (c.integer(value, knx::TimeMasterConfig::kMinIntervalS, knx::TimeMasterConfig::kMaxIntervalS, seconds,
				"interval (seconds)")) {
			config.interval_s = static_cast<uint32_t>(seconds);
		}
	}
	c.flag(tm, "atMidnight", config.at_midnight);
	c.flag(tm, "onSynchronized", config.on_synchronized);
	if (named == 0) {
		c.refuse("timeMaster names no address (time, date, dateTime)");
		return;
	}
	if (count < named) {
		return; // each refused address was named already
	}
	for (size_t i = 0; i < 3; ++i) {
		for (size_t j = i + 1; j < 3; ++j) {
			if (outputs[i].output.enabled && outputs[j].output.enabled &&
				outputs[i].output.address == outputs[j].output.address) {
				c.refuse("%s and %s use the same group address", outputs[i].member, outputs[j].member);
			}
		}
	}
	c.plan.has_time_master = true;
}

/// Per datapoint, what the KNX member said (before the mapping table is built).
struct KnxDraft {
	bool present = false;
	Dpt dpt;
	bool has_send = false;
	knx::GroupAddress send;
	knx::GroupAddress listen[knx::KnxMapping::kMaxListen];
	size_t listen_count = 0;
};

/// Per datapoint, what the `zigbee` member said (spec/zigbee.md "Bindings").
struct ZigbeeDraft {
	bool present = false; ///< Valid: `binding` holds it.
	zigbee::ZigbeeOverride binding;
	zigbee::Function function = zigbee::Function::on_off;
};

/// The `zigbee` member of a datapoint: the device function it stands for. Checks the address, the
/// endpoint, the cluster/attribute against the function table, the command cluster, the function's
/// value type against the datapoint's and that a transmitting datapoint can be commanded.
void parse_zigbee(Ctx& c, const JsonSpan& value, const DatapointSpec& spec, ZigbeeDraft& draft) {
	if (!c.object(value, "zigbee")) {
		return;
	}
	zigbee::ZigbeeOverride binding;
	bool ok = true;
	JsonSpan member;
	{
		Path q(c, ".ieee");
		char text[32];
		if (!c.member(value, "ieee", member)) {
			c.refuse("ieee is missing");
			ok = false;
		} else if (!c.string(member, text, sizeof(text), 1, 23, true, "ieee")) {
			ok = false;
		} else if (!zigbee::parse_ieee(text, binding.ieee)) {
			c.refuse("\"%s\" is not an IEEE address (16 hex digits, colons allowed; not 0 and not all ones)", text);
			ok = false;
		}
	}
	int64_t number = 0;
	{
		Path q(c, ".endpoint");
		if (!c.member(value, "endpoint", member)) {
			c.refuse("endpoint is missing");
			ok = false;
		} else if (c.integer(member, 1, 240, number, "endpoint")) {
			binding.endpoint = static_cast<uint8_t>(number);
		} else {
			ok = false;
		}
	}
	if (c.member(value, "client", member)) {
		Path q(c, ".client");
		ok = c.boolean(member, binding.client, "client") && ok;
	}
	{
		Path q(c, ".cluster");
		if (!c.member(value, "cluster", member)) {
			c.refuse("cluster is missing");
			ok = false;
		} else if (c.integer(member, 0, 65535, number, "cluster")) {
			binding.cluster = static_cast<uint16_t>(number);
		} else {
			ok = false;
		}
	}
	if (c.member(value, "attribute", member)) {
		Path q(c, ".attribute");
		if (c.integer(member, 0, 65535, number, "attribute")) {
			binding.attribute = static_cast<uint16_t>(number);
		} else {
			ok = false;
		}
	} else if (!binding.client) {
		c.refuse("attribute is missing (only a client binding, a button's commands, has none)");
		ok = false;
	}
	if (c.member(value, "command", member)) {
		Path q(c, ".command");
		if (c.integer(member, 0, 65534, number, "command (the cluster commands go to)")) {
			binding.command_cluster = static_cast<uint16_t>(number);
		} else {
			ok = false;
		}
	}
	if (!ok) {
		return;
	}
	binding.name = spec.name != nullptr ? spec.name : "?";
	const char* why = nullptr;
	std::optional<zigbee::ZigbeePoint> point = zigbee::override_point(binding, &why);
	if (!point) {
		if (binding.client) {
			c.refuse("client cluster %u: %s", static_cast<unsigned>(binding.cluster), why);
		} else {
			c.refuse("cluster %u attribute %u: %s", static_cast<unsigned>(binding.cluster),
				static_cast<unsigned>(binding.attribute), why);
		}
		return;
	}
	const zigbee::FunctionInfo& info = zigbee::function_info(point->function);
	if (spec.type != ValueType::none && info.value_type != spec.type &&
		!(numeric(info.value_type) && numeric(spec.type))) {
		c.refuse("the function %s carries %s values, the datapoint is %s", info.key, value_type_label(info.value_type),
			value_type_label(spec.type));
		return;
	}
	if ((spec.flags & DatapointFlags::kTransmit) != 0 && !point->commandable()) {
		c.refuse("a transmitting Zigbee datapoint needs the cluster its commands go to (command); %s",
			binding.client ? "a client binding (button commands) cannot be commanded" : "without it the binding is read-only");
		return;
	}
	draft.present = spec.name != nullptr;
	draft.binding = binding;
	draft.function = point->function;
}

/// Sorted datapoint ids for lookups by name.
struct Index {
	const Plan& plan;
	uint16_t* order = nullptr;
	size_t count = 0;

	/// The datapoint named `name`, -1 if none.
	int find(const char* name) const {
		size_t lo = 0;
		size_t hi = count;
		while (lo < hi) {
			size_t mid = (lo + hi) / 2;
			int cmp = std::strcmp(plan.datapoints[order[mid]].name, name);
			if (cmp == 0) {
				return order[mid];
			}
			if (cmp < 0) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		return -1;
	}
};

void parse_datapoint(Ctx& c, const JsonSpan& dp, size_t i, KnxDraft& knx_draft, ZigbeeDraft& zigbee_draft,
	HistorySpec* history, size_t& history_count) {
	Plan& plan = c.plan;
	DatapointSpec& spec = plan.datapoints[i];
	if (!c.object(dp, "a datapoint")) {
		return;
	}
	JsonSpan value;
	{
		Path p(c, ".id");
		if (!c.member(dp, "id", value)) {
			c.refuse("id is missing");
		} else {
			spec.name = c.arena_string(value, 1, Datapoint::kMaxName, "id");
			if (spec.name != nullptr &&
				(std::strcmp(spec.name, kTimeDatapoint) == 0 || std::strcmp(spec.name, kDateDatapoint) == 0 ||
					std::strcmp(spec.name, kDateTimeDatapoint) == 0) &&
				plan.has_time_master) {
				c.refuse("id '%s' is reserved for the datapoints of knx.timeMaster", spec.name);
				spec.name = nullptr; // refused once, not again as a duplicate of the time master's
			}
		}
	}
	{
		Path p(c, ".type");
		char type[16];
		if (!c.member(dp, "type", value)) {
			c.refuse("type is missing");
		} else if (c.string(value, type, sizeof(type), 1, 15, true, "type")) {
			spec.type = value_type_of(type);
			if (spec.type == ValueType::none) {
				c.refuse("unknown type \"%s\" (boolean, unsigned, signed, real, string, timeOfDay, date, dateTime, "
					"enumeration, raw, controlled, color)", type);
			}
		}
	}
	if (c.member(dp, "label", value)) {
		Path p(c, ".label");
		c.label(value);
	}
	if (c.member(dp, "unit", value)) {
		Path p(c, ".unit");
		char unit[16];
		c.string(value, unit, sizeof(unit), 1, 15, true, "unit");
	}
	bool readable = false;
	bool writable = false;
	bool transmit = false;
	bool read_on_start = false;
	bool persist = false;
	c.flag(dp, "readable", readable);
	c.flag(dp, "writable", writable);
	c.flag(dp, "transmit", transmit);
	c.flag(dp, "readOnStart", read_on_start);
	c.flag(dp, "persist", persist);
	spec.flags = static_cast<uint8_t>((readable ? DatapointFlags::kReadable : 0) |
		(writable ? DatapointFlags::kWritable : 0) | (transmit ? DatapointFlags::kTransmit : 0) |
		(read_on_start ? DatapointFlags::kReadOnStart : 0) | (persist ? DatapointFlags::kPersist : 0));

	if (c.member(dp, "history", value)) {
		Path p(c, ".history");
		if (c.object(value, "history")) {
			JsonSpan retention;
			JsonSpan spacing;
			int64_t retention_s = 0;
			int64_t spacing_s = 0;
			bool ok = true;
			if (!c.member(value, "retention", retention) || !c.member(value, "spacing", spacing)) {
				c.refuse("history needs retention and spacing (seconds)");
				ok = false;
			} else {
				{
					Path q(c, ".spacing");
					ok = c.integer(spacing, History::kMinSpacingS, History::kMaxSpacingS, spacing_s, "spacing (seconds)");
				}
				if (ok) {
					Path q(c, ".retention");
					ok = c.integer(retention, static_cast<double>(spacing_s), History::kMaxRetentionS, retention_s,
						"retention (seconds, at least the spacing)");
				}
			}
			if (ok && History::ring_samples(static_cast<uint32_t>(retention_s), static_cast<uint32_t>(spacing_s)) >
					History::kMaxSamples) {
				c.refuse("retention / spacing gives more than %u samples", static_cast<unsigned>(History::kMaxSamples));
				ok = false;
			}
			if (spec.type != ValueType::none && spec.type != ValueType::boolean && spec.type != ValueType::unsigned_int &&
				spec.type != ValueType::signed_int && spec.type != ValueType::real) {
				c.refuse("history needs a boolean, unsigned, signed or real datapoint");
				ok = false;
			}
			if (ok) {
				HistorySpec& h = history[history_count++];
				h.datapoint = static_cast<uint16_t>(i);
				h.retention_s = static_cast<uint32_t>(retention_s);
				h.spacing_s = static_cast<uint32_t>(spacing_s);
				spec.flags = static_cast<uint8_t>(spec.flags | DatapointFlags::kHistory);
			}
		}
	}

	JsonSpan virtual_member;
	bool has_virtual = c.member(dp, "virtual", virtual_member);
	if (has_virtual) {
		Path p(c, ".virtual");
		c.object(virtual_member, "virtual");
	}
	JsonSpan zigbee_member;
	if (c.member(dp, "zigbee", zigbee_member)) {
		Path p(c, ".zigbee");
		if (has_virtual) {
			c.refuse("a datapoint is either zigbee or virtual");
		} else if (c.member(dp, "knx", value)) {
			c.refuse("a datapoint is either knx or zigbee");
		} else {
			parse_zigbee(c, zigbee_member, spec, zigbee_draft);
		}
		return;
	}
	if (!c.member(dp, "knx", value)) {
		return;
	}
	Path p(c, ".knx");
	if (has_virtual) {
		c.refuse("a datapoint is either knx or virtual");
		return;
	}
	if (!c.object(value, "knx")) {
		return;
	}
	knx_draft.present = true;
	JsonSpan member;
	{
		Path q(c, ".dpt");
		char text[Dpt::kMaxText + 4];
		if (!c.member(value, "dpt", member)) {
			c.refuse("dpt is missing");
		} else if (c.string(member, text, sizeof(text), 1, Dpt::kMaxText, true, "dpt")) {
			std::optional<Dpt> dpt = Dpt::parse(text);
			if (!dpt) {
				c.refuse("\"%s\" is not a datapoint type (expected e.g. 1.001, 9.001)", text);
			} else if (!dpt->supported()) {
				c.refuse("DPT %s: %s", text, dpt->unsupported_reason());
			} else {
				knx_draft.dpt = *dpt;
				ValueType type = dpt->value_type();
				if (spec.type != ValueType::none && type != spec.type && !(numeric(type) && numeric(spec.type))) {
					c.refuse("DPT %s carries %s values, the datapoint is %s", text, value_type_label(type),
						value_type_label(spec.type));
				}
			}
		}
	}
	bool address_refused = false;
	if (c.member(value, "send", member)) {
		Path q(c, ".send");
		knx_draft.has_send = c.group_address(member, knx_draft.send, "send");
		address_refused = !knx_draft.has_send;
	}
	if (c.member(value, "listen", member)) {
		Path q(c, ".listen");
		if (c.array(member, "listen")) {
			size_t position = 0;
			JsonSpan item;
			size_t n = 0;
			while (json_next_item(member, position, item)) {
				Path a(c, "[%u]", static_cast<unsigned>(n));
				if (n >= knx::KnxMapping::kMaxListen) {
					c.refuse("more than %u listening addresses", static_cast<unsigned>(knx::KnxMapping::kMaxListen));
					break;
				}
				if (c.group_address(item, knx_draft.listen[knx_draft.listen_count], "a listening address")) {
					++knx_draft.listen_count;
				} else {
					address_refused = true;
				}
				++n;
			}
		}
	}
	if (address_refused) {
		return; // the address's own refusal says enough
	}
	if (!knx_draft.has_send && knx_draft.listen_count == 0) {
		c.refuse("a KNX datapoint needs a sending or a listening group address");
	} else if (!knx_draft.has_send && (transmit || readable)) {
		c.refuse("a transmitting or readable KNX datapoint needs a sending address (send)");
	}
}

void parse_datapoints(Ctx& c, const JsonSpan& root) {
	Plan& plan = c.plan;
	JsonSpan list;
	size_t declared = 0;
	bool has_list = c.member(root, "datapoints", list);
	if (has_list) {
		Path p(c, "datapoints");
		if (!c.array(list, "'datapoints'")) {
			has_list = false;
		} else {
			size_t position = 0;
			JsonSpan item;
			while (json_next_item(list, position, item)) {
				++declared;
			}
			if (declared > kMaxDatapoints) {
				c.refuse("%u datapoints exceed the limit of %u", static_cast<unsigned>(declared),
					static_cast<unsigned>(kMaxDatapoints));
				return;
			}
		}
	}
	size_t implied = 0;
	const knx::TimeMasterConfig& tm = plan.time_master;
	if (plan.has_time_master) {
		implied = (tm.time.enabled ? 1 : 0) + (tm.date.enabled ? 1 : 0) + (tm.date_time.enabled ? 1 : 0);
	}
	size_t total = declared + implied;
	plan.datapoints = plan.arena.array<DatapointSpec>(total);
	plan.bindings = plan.arena.array<Binding>(total);
	KnxDraft* drafts = plan.arena.array<KnxDraft>(declared);
	ZigbeeDraft* zigbee_drafts = plan.arena.array<ZigbeeDraft>(declared);
	HistorySpec* history = plan.arena.array<HistorySpec>(declared);
	if (total > 0 && (plan.datapoints == nullptr || plan.bindings == nullptr || (declared > 0 && (drafts == nullptr ||
		zigbee_drafts == nullptr || history == nullptr)))) {
		c.no_memory("the datapoint table");
		return;
	}
	plan.datapoint_count = total;
	size_t history_count = 0;
	if (has_list) {
		Path p(c, "datapoints");
		size_t position = 0;
		JsonSpan item;
		size_t i = 0;
		while (json_next_item(list, position, item) && !c.stopped()) {
			Path q(c, "[%u]", static_cast<unsigned>(i));
			parse_datapoint(c, item, i, drafts[i], zigbee_drafts[i], history, history_count);
			++i;
		}
	}
	// The time master's datapoints (doc/knx.md: T flag, R to answer reads).
	size_t at = declared;
	struct Implied {
		const knx::TimeMasterConfig::Output& output;
		ValueType type;
	};
	Implied implied_list[] = {
		{tm.time, ValueType::time_of_day},
		{tm.date, ValueType::date},
		{tm.date_time, ValueType::date_time},
	};
	if (plan.has_time_master) {
		for (const Implied& o : implied_list) {
			if (o.output.enabled) {
				plan.datapoints[at].name = o.output.datapoint;
				plan.datapoints[at].type = o.type;
				plan.datapoints[at].flags = DatapointFlags::kReadable | DatapointFlags::kTransmit;
				plan.bindings[at] = Binding::time_master;
				++at;
			}
		}
	}
	if (c.stopped()) {
		return;
	}
	// Unique ids.
	uint16_t* order = plan.arena.array<uint16_t>(total);
	if (total > 0 && order == nullptr) {
		c.no_memory("the datapoint index");
		return;
	}
	size_t named = 0;
	for (size_t i = 0; i < total; ++i) {
		if (plan.datapoints[i].name != nullptr) {
			order[named++] = static_cast<uint16_t>(i);
		}
	}
	std::sort(order, order + named, [&plan](uint16_t a, uint16_t b) {
		int cmp = std::strcmp(plan.datapoints[a].name, plan.datapoints[b].name);
		return cmp < 0 || (cmp == 0 && a < b);
	});
	for (size_t k = 1; k < named; ++k) {
		if (std::strcmp(plan.datapoints[order[k - 1]].name, plan.datapoints[order[k]].name) == 0) {
			Path p(c, "datapoints[%u].id", static_cast<unsigned>(order[k]));
			c.refuse("duplicate id '%s' (also datapoints[%u])", plan.datapoints[order[k]].name,
				static_cast<unsigned>(order[k - 1]));
		}
	}
	// KNX mappings.
	size_t knx_count = 0;
	size_t listens = 0;
	for (size_t i = 0; i < declared; ++i) {
		if (drafts[i].present) {
			++knx_count;
			listens += drafts[i].listen_count;
		}
	}
	plan.mappings = plan.arena.array<knx::KnxMapping>(knx_count);
	knx::GroupAddress* addresses = plan.arena.array<knx::GroupAddress>(listens);
	if ((knx_count > 0 && plan.mappings == nullptr) || (listens > 0 && addresses == nullptr)) {
		c.no_memory("the KNX mapping");
		return;
	}
	size_t m = 0;
	for (size_t i = 0; i < declared; ++i) {
		const KnxDraft& d = drafts[i];
		if (!d.present) {
			continue;
		}
		knx::KnxMapping& mapping = plan.mappings[m++];
		mapping.datapoint = static_cast<uint16_t>(i);
		mapping.dpt = d.dpt;
		mapping.has_send = d.has_send;
		mapping.send = d.send;
		mapping.listen = d.listen_count > 0 ? addresses : nullptr;
		mapping.listen_count = d.listen_count;
		for (size_t k = 0; k < d.listen_count; ++k) {
			*addresses++ = d.listen[k];
		}
		plan.bindings[i] = Binding::knx;
	}
	plan.mapping_count = knx_count;
	plan.info.knx_datapoints = knx_count;
	// Zigbee bindings: at most what the adapter takes, no function of an endpoint twice.
	size_t zigbee_count = 0;
	for (size_t i = 0; i < declared; ++i) {
		zigbee_count += zigbee_drafts[i].present ? 1 : 0;
	}
	if (zigbee_count > zigbee::ZigbeeAdapter::kMaxOverrides) {
		Path p(c, "datapoints");
		c.refuse("%u Zigbee bindings exceed the limit of %u", static_cast<unsigned>(zigbee_count),
			static_cast<unsigned>(zigbee::ZigbeeAdapter::kMaxOverrides));
		return;
	}
	plan.zigbee = plan.arena.array<zigbee::ZigbeeOverride>(zigbee_count);
	if (zigbee_count > 0 && plan.zigbee == nullptr) {
		c.no_memory("the Zigbee bindings");
		return;
	}
	for (size_t i = 0; i < declared; ++i) {
		const ZigbeeDraft& d = zigbee_drafts[i];
		if (!d.present) {
			continue;
		}
		for (size_t k = 0; k < i; ++k) {
			const ZigbeeDraft& e = zigbee_drafts[k];
			if (e.present && e.binding.ieee == d.binding.ieee && e.binding.endpoint == d.binding.endpoint &&
				e.function == d.function) {
				char ieee[zigbee::kIeeeText];
				zigbee::format_ieee(d.binding.ieee, ieee);
				Path p(c, "datapoints[%u].zigbee", static_cast<unsigned>(i));
				c.refuse("device %s endpoint %u: the function %s is bound twice (also datapoints[%u] '%s')", ieee,
					static_cast<unsigned>(d.binding.endpoint), zigbee::function_info(d.function).key,
					static_cast<unsigned>(k), e.binding.name);
				break;
			}
		}
		plan.zigbee[plan.zigbee_count++] = d.binding;
		plan.bindings[i] = Binding::zigbee;
	}
	plan.info.zigbee_datapoints = plan.zigbee_count;
	// The time master's addresses must not be sent on by a datapoint of the image.
	if (plan.has_time_master) {
		const char* names[] = {"time", "date", "dateTime"};
		const knx::TimeMasterConfig::Output* outs[] = {&tm.time, &tm.date, &tm.date_time};
		for (size_t o = 0; o < 3; ++o) {
			if (!outs[o]->enabled) {
				continue;
			}
			for (size_t k = 0; k < knx_count; ++k) {
				if (plan.mappings[k].has_send && plan.mappings[k].send == outs[o]->address) {
					Path p(c, "knx.timeMaster.%s", names[o]);
					c.refuse("the address is the sending address of datapoints[%u] '%s'",
						static_cast<unsigned>(plan.mappings[k].datapoint),
						plan.datapoints[plan.mappings[k].datapoint].name != nullptr
							? plan.datapoints[plan.mappings[k].datapoint].name : "?");
				}
			}
		}
	}
	plan.history = history;
	plan.history_count = history_count;
	plan.info.history_rings = history_count;
	plan.info.time_master = plan.has_time_master;
	plan.info.datapoints = total;
}

// ---- Locations and functions ------------------------------------------------------------------

struct Ids {
	char** ids = nullptr;
	int* parents = nullptr;
	size_t count = 0;

	int find(const char* id) const {
		for (size_t i = 0; i < count; ++i) {
			if (ids[i] != nullptr && std::strcmp(ids[i], id) == 0) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}
};

/// Reads an id member (1..32 of A-Z a-z 0-9 _ . -) into the arena; nullptr after a refusal.
char* read_id(Ctx& c, const JsonSpan& object, const char* what) {
	JsonSpan value;
	Path p(c, ".id");
	if (!c.member(object, "id", value)) {
		c.refuse("id is missing");
		return nullptr;
	}
	char* id = c.arena_string(value, 1, 32, what);
	if (id != nullptr && !id_valid(id, 32, true)) {
		c.refuse("%s '%s' must have 1..32 characters of A-Z a-z 0-9 _ . -", what, id);
		return nullptr;
	}
	return id;
}

void parse_locations(Ctx& c, const JsonSpan& root, Ids& locations) {
	JsonSpan list;
	if (!c.member(root, "locations", list)) {
		return;
	}
	Path p(c, "locations");
	if (!c.array(list, "'locations'")) {
		return;
	}
	size_t count = 0;
	size_t position = 0;
	JsonSpan item;
	while (json_next_item(list, position, item)) {
		++count;
	}
	if (count > kMaxLocations) {
		c.refuse("%u locations exceed the limit of %u", static_cast<unsigned>(count),
			static_cast<unsigned>(kMaxLocations));
		return;
	}
	locations.ids = c.plan.arena.array<char*>(count);
	locations.parents = c.plan.arena.array<int>(count);
	char** parent_names = c.plan.arena.array<char*>(count);
	if (count > 0 && (locations.ids == nullptr || locations.parents == nullptr || parent_names == nullptr)) {
		c.no_memory("the locations");
		return;
	}
	locations.count = count;
	position = 0;
	size_t i = 0;
	static const char* const kKinds[] = {"building", "floor", "room", "area", "outside", nullptr};
	while (json_next_item(list, position, item) && !c.stopped()) {
		Path q(c, "[%u]", static_cast<unsigned>(i));
		locations.parents[i] = -1;
		if (c.object(item, "a location")) {
			char* id = read_id(c, item, "location id");
			if (id != nullptr) {
				if (locations.find(id) >= 0) {
					Path a(c, ".id");
					c.refuse("duplicate location id '%s'", id);
				}
				locations.ids[i] = id;
			}
			JsonSpan value;
			if (!c.member(item, "label", value)) {
				c.refuse("label is missing");
			} else {
				Path a(c, ".label");
				c.label(value);
			}
			if (c.member(item, "kind", value)) {
				Path a(c, ".kind");
				char kind[16];
				if (c.string(value, kind, sizeof(kind), 1, 15, true, "kind")) {
					bool known = false;
					for (const char* const* k = kKinds; *k != nullptr && !known; ++k) {
						known = std::strcmp(*k, kind) == 0;
					}
					if (!known) {
						c.refuse("unknown location kind \"%s\" (building, floor, room, area, outside)", kind);
					}
				}
			}
			if (c.member(item, "parent", value)) {
				Path a(c, ".parent");
				parent_names[i] = c.arena_string(value, 1, 32, "parent");
			}
		}
		++i;
	}
	for (size_t k = 0; k < count; ++k) {
		if (parent_names[k] == nullptr) {
			continue;
		}
		int parent = locations.find(parent_names[k]);
		if (parent < 0) {
			Path q(c, "[%u].parent", static_cast<unsigned>(k));
			c.refuse("unknown location '%s'", parent_names[k]);
		} else {
			locations.parents[k] = parent;
		}
	}
	// No cycles: walking up from any location ends within count steps.
	for (size_t k = 0; k < count; ++k) {
		int at = locations.parents[k];
		size_t steps = 0;
		while (at >= 0 && steps <= count) {
			at = locations.parents[at];
			++steps;
		}
		if (steps > count) {
			Path q(c, "[%u].parent", static_cast<unsigned>(k));
			c.refuse("the parents form a cycle");
			break;
		}
	}
	c.plan.info.locations = count;
}

/// The datapoint roles of a function kind.
struct Role {
	const char* name;
	uint8_t types; ///< kRoleBool | kRoleNumber | kRoleEnum | kRoleControlled, or kRoleAny.
	bool writable;
	bool required;
};

constexpr uint8_t kRoleBool = 0x01;
constexpr uint8_t kRoleNumber = 0x02; ///< unsigned, signed, real
constexpr uint8_t kRoleEnum = 0x04;
constexpr uint8_t kRoleControlled = 0x08;
constexpr uint8_t kRoleAny = 0xFF;

struct FunctionKind {
	const char* name;
	const Role* roles;
	size_t role_count;
};

constexpr Role kLight[] = {{"command", kRoleBool, true, true}, {"state", kRoleBool, false, false}};
constexpr Role kDimmer[] = {{"levelCommand", kRoleNumber, true, true}, {"level", kRoleNumber, false, false},
	{"command", kRoleBool, true, false}, {"state", kRoleBool, false, false},
	{"step", kRoleControlled, true, false}}; // relative dimming, KNX DPT 3.007 (#30)
constexpr Role kBlind[] = {{"move", kRoleBool, true, true}, {"stop", kRoleBool, true, false},
	{"position", kRoleNumber, false, false}, {"positionCommand", kRoleNumber, true, false},
	{"slat", kRoleNumber, false, false}, {"slatCommand", kRoleNumber, true, false}};
constexpr Role kSwitch[] = {{"command", kRoleBool, true, true}, {"state", kRoleBool, false, false}};
constexpr Role kSensor[] = {{"value", kRoleAny, false, true}};
constexpr Role kClimate[] = {{"temperature", kRoleNumber, false, true}, {"setpoint", kRoleNumber, false, false},
	{"setpointCommand", kRoleNumber, true, false}, {"mode", kRoleEnum | kRoleNumber, false, false},
	{"modeCommand", kRoleEnum | kRoleNumber, true, false}, {"heating", kRoleBool, false, false}};
constexpr Role kScene[] = {{"command", kRoleNumber | kRoleEnum | kRoleControlled, true, true}};
constexpr Role kButton[] = {{"press", kRoleBool | kRoleControlled, false, true}, {"state", kRoleBool, false, false}};

constexpr FunctionKind kFunctionKinds[] = {
	{"light", kLight, sizeof(kLight) / sizeof(Role)},
	{"dimmer", kDimmer, sizeof(kDimmer) / sizeof(Role)},
	{"blind", kBlind, sizeof(kBlind) / sizeof(Role)},
	{"switch", kSwitch, sizeof(kSwitch) / sizeof(Role)},
	{"sensor", kSensor, sizeof(kSensor) / sizeof(Role)},
	{"climate", kClimate, sizeof(kClimate) / sizeof(Role)},
	{"scene", kScene, sizeof(kScene) / sizeof(Role)},
	{"button", kButton, sizeof(kButton) / sizeof(Role)},
};

/// The value types a role accepts, for refusals: "boolean", "a number", "boolean or controlled", ...
const char* role_types_text(uint8_t types) {
	switch (types) {
		case kRoleBool: return "boolean";
		case kRoleNumber: return "a number (unsigned, signed or real)";
		case kRoleEnum: return "enumeration";
		case kRoleControlled: return "controlled";
		case kRoleEnum | kRoleNumber: return "enumeration or a number";
		case kRoleBool | kRoleControlled: return "boolean or controlled";
		case kRoleNumber | kRoleEnum | kRoleControlled: return "a number, enumeration or controlled";
		default: return "any type";
	}
}

bool role_type_fits(uint8_t types, ValueType type) {
	if (types == kRoleAny) {
		return true;
	}
	switch (type) {
		case ValueType::boolean: return (types & kRoleBool) != 0;
		case ValueType::unsigned_int:
		case ValueType::signed_int:
		case ValueType::real: return (types & kRoleNumber) != 0;
		case ValueType::enumeration: return (types & (kRoleEnum | kRoleNumber)) != 0;
		case ValueType::controlled: return (types & kRoleControlled) != 0;
		default: return false;
	}
}

void parse_functions(Ctx& c, const JsonSpan& root, const Ids& locations, const Index& index) {
	JsonSpan list;
	if (!c.member(root, "functions", list)) {
		return;
	}
	Path p(c, "functions");
	if (!c.array(list, "'functions'")) {
		return;
	}
	size_t count = 0;
	size_t position = 0;
	JsonSpan item;
	while (json_next_item(list, position, item)) {
		++count;
	}
	if (count > kMaxFunctions) {
		c.refuse("%u functions exceed the limit of %u", static_cast<unsigned>(count),
			static_cast<unsigned>(kMaxFunctions));
		return;
	}
	Ids ids;
	ids.ids = c.plan.arena.array<char*>(count);
	if (count > 0 && ids.ids == nullptr) {
		c.no_memory("the functions");
		return;
	}
	position = 0;
	size_t i = 0;
	while (json_next_item(list, position, item) && !c.stopped()) {
		Path q(c, "[%u]", static_cast<unsigned>(i++));
		if (!c.object(item, "a function")) {
			continue;
		}
		char* id = read_id(c, item, "function id");
		if (id != nullptr) {
			if (ids.find(id) >= 0) {
				Path a(c, ".id");
				c.refuse("duplicate function id '%s'", id);
			}
			ids.ids[ids.count++] = id;
		}
		JsonSpan value;
		if (!c.member(item, "label", value)) {
			c.refuse("label is missing");
		} else {
			Path a(c, ".label");
			c.label(value);
		}
		if (c.member(item, "room", value)) {
			Path a(c, ".room");
			char room[40];
			if (c.string(value, room, sizeof(room), 1, 32, true, "room") && locations.find(room) < 0) {
				c.refuse("unknown location '%s'", room);
			}
		}
		if (c.member(item, "unit", value)) {
			Path a(c, ".unit");
			char unit[16];
			c.string(value, unit, sizeof(unit), 1, 15, true, "unit");
		}
		const FunctionKind* kind = nullptr;
		{
			Path a(c, ".kind");
			char name[16];
			if (!c.member(item, "kind", value)) {
				c.refuse("kind is missing");
			} else if (c.string(value, name, sizeof(name), 1, 15, true, "kind")) {
				for (const FunctionKind& k : kFunctionKinds) {
					if (std::strcmp(k.name, name) == 0) {
						kind = &k;
					}
				}
				if (kind == nullptr) {
					c.refuse("unknown function kind \"%s\" (light, dimmer, blind, switch, sensor, climate, scene, button)",
						name);
				}
			}
		}
		if (kind != nullptr && std::strcmp(kind->name, "scene") == 0) {
			Path a(c, ".scene");
			int64_t number = 0;
			if (!c.member(item, "scene", value)) {
				c.refuse("a scene function needs scene (1..64)");
			} else {
				c.integer(value, 1, 64, number, "scene");
			}
		}
		if (kind == nullptr) {
			continue;
		}
		JsonSpan roles;
		Path a(c, ".datapoints");
		if (!c.member(item, "datapoints", roles)) {
			c.refuse("datapoints (the roles of a %s) is missing", kind->name);
			continue;
		}
		if (!c.object(roles, "datapoints")) {
			continue;
		}
		for (size_t r = 0; r < kind->role_count; ++r) {
			const Role& role = kind->roles[r];
			JsonSpan ref;
			if (!c.member(roles, role.name, ref)) {
				if (role.required) {
					c.refuse("a %s needs the role %s", kind->name, role.name);
				}
				continue;
			}
			Path b(c, ".%s", role.name);
			char name[Datapoint::kMaxName + 1];
			if (!c.string(ref, name, sizeof(name), 1, Datapoint::kMaxName, true, "the role")) {
				continue;
			}
			int dp = index.find(name);
			if (dp < 0) {
				c.refuse("unknown datapoint '%s'", name);
				continue;
			}
			const DatapointSpec& spec = c.plan.datapoints[dp];
			if (!role_type_fits(role.types, spec.type)) {
				c.refuse("datapoint '%s' is %s, but the role %s of a %s takes %s", name,
					value_type_label(spec.type), role.name, kind->name, role_types_text(role.types));
			}
			if (role.writable && (spec.flags & DatapointFlags::kWritable) == 0) {
				c.refuse("the role %s needs a writable datapoint; '%s' is not", role.name, name);
			}
		}
	}
	c.plan.info.functions = count;
}

// ---- Schedules (spec/schedules.md) -------------------------------------------------------------

bool read_clock(Ctx& c, const JsonSpan& span, LocalClockTime& out, const char* what) {
	static const char* const kMembers[] = {"hour", "minute", "second", nullptr};
	if (span.kind != JsonSpan::Kind::object) {
		c.refuse("malformed: %s must be {\"hour\", \"minute\", \"second\"}", what);
		return false;
	}
	if (!c.only(span, kMembers)) {
		return false;
	}
	JsonSpan h;
	JsonSpan m;
	JsonSpan s;
	if (!c.member(span, "hour", h) || !c.member(span, "minute", m) || !c.member(span, "second", s)) {
		c.refuse("malformed: %s needs hour, minute and second", what);
		return false;
	}
	int64_t hour = 0;
	int64_t minute = 0;
	int64_t second = 0;
	if (!c.integer(h, 0, 23, hour, "malformed: hour") || !c.integer(m, 0, 59, minute, "malformed: minute") ||
		!c.integer(s, 0, 59, second, "malformed: second")) {
		return false;
	}
	out.hour = static_cast<uint8_t>(hour);
	out.minute = static_cast<uint8_t>(minute);
	out.second = static_cast<uint8_t>(second);
	return true;
}

bool date_valid(int64_t year, int64_t month, int64_t day) {
	return year >= 1970 && year <= 2155 && month >= 1 && month <= 12 && day >= 1 &&
		day <= static_cast<int64_t>(days_in_month(static_cast<int32_t>(year), static_cast<unsigned>(month)));
}

bool read_date(Ctx& c, const JsonSpan& span, Date& out, LocalClockTime* time, const char* what) {
	static const char* const kDate[] = {"year", "month", "day", nullptr};
	static const char* const kDateTime[] = {"year", "month", "day", "hour", "minute", "second", nullptr};
	if (span.kind != JsonSpan::Kind::object) {
		c.refuse("malformed: %s must be an object", what);
		return false;
	}
	if (!c.only(span, time != nullptr ? kDateTime : kDate)) {
		return false;
	}
	JsonSpan y;
	JsonSpan mo;
	JsonSpan d;
	if (!c.member(span, "year", y) || !c.member(span, "month", mo) || !c.member(span, "day", d)) {
		c.refuse("malformed: %s needs year, month and day", what);
		return false;
	}
	int64_t year = 0;
	int64_t month = 0;
	int64_t day = 0;
	if (!c.integer(y, 1970, 2155, year, "malformed: year") || !c.integer(mo, 1, 12, month, "malformed: month") ||
		!c.integer(d, 1, 31, day, "malformed: day")) {
		return false;
	}
	if (!date_valid(year, month, day)) {
		c.refuse("malformed: %04d-%02d-%02d is not a date", static_cast<int>(year), static_cast<int>(month),
			static_cast<int>(day));
		return false;
	}
	out.year = static_cast<uint16_t>(year);
	out.month = static_cast<uint8_t>(month);
	out.day = static_cast<uint8_t>(day);
	if (time != nullptr) {
		JsonSpan h;
		JsonSpan mi;
		JsonSpan s;
		if (!c.member(span, "hour", h) || !c.member(span, "minute", mi) || !c.member(span, "second", s)) {
			c.refuse("malformed: %s needs hour, minute and second", what);
			return false;
		}
		int64_t hour = 0;
		int64_t minute = 0;
		int64_t second = 0;
		if (!c.integer(h, 0, 23, hour, "malformed: hour") || !c.integer(mi, 0, 59, minute, "malformed: minute") ||
			!c.integer(s, 0, 59, second, "malformed: second")) {
			return false;
		}
		time->hour = static_cast<uint8_t>(hour);
		time->minute = static_cast<uint8_t>(minute);
		time->second = static_cast<uint8_t>(second);
	}
	return true;
}

bool read_days(Ctx& c, const JsonSpan& span, uint8_t& out) {
	if (span.kind != JsonSpan::Kind::array) {
		c.refuse("malformed: days must be an array of 1 (Monday) .. 7 (Sunday)");
		return false;
	}
	uint8_t days = 0;
	size_t position = 0;
	JsonSpan item;
	while (json_next_item(span, position, item)) {
		int64_t day = 0;
		if (!c.integer(item, 1, 7, day, "malformed: a day")) {
			return false;
		}
		uint8_t bit = Weekdays::bit(static_cast<unsigned>(day));
		if ((days & bit) != 0) {
			c.refuse("malformed: day %d is listed twice", static_cast<int>(day));
			return false;
		}
		days = static_cast<uint8_t>(days | bit);
	}
	if (days == 0) {
		c.refuse("malformed: days must name at least one day");
		return false;
	}
	out = days;
	return true;
}

uint32_t seconds_of(const LocalClockTime& t) {
	return static_cast<uint32_t>(t.hour) * 3600u + t.minute * 60u + t.second;
}

void parse_schedule(Ctx& c, const JsonSpan& item, ScheduleSpec& spec, const Index& index) {
	static const char* const kWeekly[] = {"id", "label", "kind", "enabled", "enabledAdjustable", "missed", "output",
		"time", "days", nullptr};
	static const char* const kYearly[] = {"id", "label", "kind", "enabled", "enabledAdjustable", "missed", "output",
		"month", "day", "time", nullptr};
	static const char* const kDates[] = {"id", "label", "kind", "enabled", "enabledAdjustable", "missed", "output",
		"dates", "time", nullptr};
	static const char* const kAstro[] = {"id", "label", "kind", "enabled", "enabledAdjustable", "missed", "output",
		"sun", "offset", "notBefore", "notAfter", "days", nullptr};
	static const char* const kInterval[] = {"id", "label", "kind", "enabled", "enabledAdjustable", "missed",
		"output", "every", "from", "until", "days", nullptr};
	static const char* const kOnce[] = {"id", "label", "kind", "enabled", "enabledAdjustable", "missed", "output",
		"at", nullptr};
	if (item.kind != JsonSpan::Kind::object) {
		c.refuse("malformed: a schedule entry must be an object");
		return;
	}
	JsonSpan value;
	{
		Path p(c, ".id");
		if (!c.member(item, "id", value)) {
			c.refuse("malformed: id is missing");
		} else {
			spec.id = c.arena_string(value, 1, Scheduler::kMaxId, "malformed: id");
			if (spec.id != nullptr && !id_valid(spec.id, Scheduler::kMaxId, false)) {
				c.refuse("malformed: id '%s' must have 1..32 characters of A-Z a-z 0-9 _ -", spec.id);
				spec.id = nullptr;
			}
		}
	}
	{
		Path p(c, ".label");
		if (!c.member(item, "label", value)) {
			c.refuse("malformed: label is missing");
		} else {
			spec.label = c.arena_string(value, 1, Scheduler::kMaxLabel, "malformed: label");
		}
	}
	char kind[16] = {};
	{
		Path p(c, ".kind");
		if (!c.member(item, "kind", value)) {
			c.refuse("malformed: kind is missing");
			return;
		}
		if (!c.string(value, kind, sizeof(kind), 1, 15, true, "malformed: kind")) {
			return;
		}
	}
	const char* const* members = nullptr;
	if (std::strcmp(kind, "weekly") == 0) {
		spec.kind = ScheduleKind::weekly;
		members = kWeekly;
	} else if (std::strcmp(kind, "yearly") == 0) {
		spec.kind = ScheduleKind::yearly;
		members = kYearly;
	} else if (std::strcmp(kind, "dates") == 0) {
		spec.kind = ScheduleKind::dates;
		members = kDates;
	} else if (std::strcmp(kind, "astro") == 0) {
		spec.kind = ScheduleKind::astro;
		members = kAstro;
	} else if (std::strcmp(kind, "interval") == 0) {
		spec.kind = ScheduleKind::interval;
		members = kInterval;
	} else if (std::strcmp(kind, "once") == 0) {
		spec.kind = ScheduleKind::once;
		members = kOnce;
	} else {
		Path p(c, ".kind");
		c.refuse("malformed: unknown kind \"%s\" (weekly, yearly, dates, astro, interval, once)", kind);
		return;
	}
	c.only(item, members);
	c.flag(item, "enabled", spec.enabled);
	c.flag(item, "enabledAdjustable", spec.enabled_adjustable);
	if (c.member(item, "missed", value)) {
		Path p(c, ".missed");
		char missed[16];
		if (c.string(value, missed, sizeof(missed), 1, 15, true, "malformed: missed")) {
			if (std::strcmp(missed, "skip") == 0) {
				spec.missed = MissedPolicy::skip;
			} else if (std::strcmp(missed, "catchUp") == 0) {
				spec.missed = MissedPolicy::catch_up;
			} else {
				c.refuse("malformed: missed must be skip or catchUp");
			}
		}
	}
	if (c.member(item, "output", value)) {
		Path p(c, ".output");
		spec.output = c.arena_string(value, 1, Datapoint::kMaxName, "malformed: output");
		if (spec.output != nullptr) {
			int dp = index.find(spec.output);
			if (dp < 0) {
				c.refuse("unknownReference: output '%s' is no datapoint of the image", spec.output);
			} else if (c.plan.datapoints[dp].type != ValueType::boolean) {
				c.refuse("typeError: output '%s' is %s, not boolean", spec.output,
					value_type_label(c.plan.datapoints[dp].type));
			} else if (c.plan.bindings[dp] != Binding::none) {
				c.refuse("outputConflict: output '%s' is owned by %s", spec.output,
					c.plan.bindings[dp] == Binding::knx ? "the KNX adapter"
					: c.plan.bindings[dp] == Binding::schedule ? "another schedule entry"
					: c.plan.bindings[dp] == Binding::zigbee ? "the Zigbee adapter"
					: "the time master");
			} else {
				c.plan.bindings[dp] = Binding::schedule;
			}
		}
	}
	bool uses_days = spec.kind == ScheduleKind::weekly || spec.kind == ScheduleKind::astro ||
		spec.kind == ScheduleKind::interval;
	if (uses_days && c.member(item, "days", value)) {
		Path p(c, ".days");
		read_days(c, value, spec.days);
	}
	auto required = [&](const char* name, JsonSpan& out) {
		if (!c.member(item, name, out)) {
			c.refuse("malformed: a %s entry needs %s", kind, name);
			return false;
		}
		return true;
	};
	switch (spec.kind) {
		case ScheduleKind::weekly:
			if (required("time", value)) {
				Path p(c, ".time");
				read_clock(c, value, spec.time, "time");
			}
			break;
		case ScheduleKind::yearly: {
			if (required("time", value)) {
				Path p(c, ".time");
				read_clock(c, value, spec.time, "time");
			}
			JsonSpan month;
			JsonSpan day;
			int64_t mo = 0;
			int64_t d = 0;
			if (required("month", month) && required("day", day)) {
				Path p(c, ".day");
				if (c.integer(month, 1, 12, mo, "malformed: month") && c.integer(day, 1, 31, d, "malformed: day")) {
					if (d > static_cast<int64_t>(days_in_month(2024, static_cast<unsigned>(mo)))) {
						c.refuse("malformed: month %d has no day %d", static_cast<int>(mo), static_cast<int>(d));
					} else {
						spec.month = static_cast<uint8_t>(mo);
						spec.day = static_cast<uint8_t>(d);
					}
				}
			}
			break;
		}
		case ScheduleKind::dates: {
			if (required("time", value)) {
				Path p(c, ".time");
				read_clock(c, value, spec.time, "time");
			}
			if (!required("dates", value)) {
				break;
			}
			Path p(c, ".dates");
			if (value.kind != JsonSpan::Kind::array) {
				c.refuse("malformed: dates must be an array of dates");
				break;
			}
			size_t count = 0;
			size_t position = 0;
			JsonSpan d;
			while (json_next_item(value, position, d)) {
				++count;
			}
			if (count == 0) {
				c.refuse("malformed: dates must list at least one date");
				break;
			}
			if (count > Scheduler::kMaxDates) {
				c.refuse("limitExceeded: %u dates exceed the limit of %u", static_cast<unsigned>(count),
					static_cast<unsigned>(Scheduler::kMaxDates));
				break;
			}
			Date* dates = c.plan.arena.array<Date>(count);
			if (dates == nullptr) {
				c.no_memory("the dates");
				break;
			}
			position = 0;
			size_t n = 0;
			bool ok = true;
			while (json_next_item(value, position, d)) {
				Path q(c, "[%u]", static_cast<unsigned>(n));
				if (!read_date(c, d, dates[n], nullptr, "a date")) {
					ok = false;
				} else {
					for (size_t k = 0; k < n; ++k) {
						if (dates[k] == dates[n]) {
							c.refuse("duplicateId: the date is listed twice");
							ok = false;
						}
					}
				}
				++n;
			}
			if (ok) {
				spec.dates = dates;
				spec.date_count = count;
			}
			break;
		}
		case ScheduleKind::astro: {
			if (required("sun", value)) {
				Path p(c, ".sun");
				char sun[16];
				if (c.string(value, sun, sizeof(sun), 1, 15, true, "malformed: sun")) {
					if (std::strcmp(sun, "dawn") == 0) {
						spec.sun = SunEvent::dawn;
					} else if (std::strcmp(sun, "sunrise") == 0) {
						spec.sun = SunEvent::sunrise;
					} else if (std::strcmp(sun, "sunset") == 0) {
						spec.sun = SunEvent::sunset;
					} else if (std::strcmp(sun, "dusk") == 0) {
						spec.sun = SunEvent::dusk;
					} else {
						c.refuse("malformed: sun must be dawn, sunrise, sunset or dusk");
					}
				}
			}
			if (c.member(item, "offset", value)) {
				Path p(c, ".offset");
				int64_t ms = 0;
				if (c.integer(value, -43200000.0, 43200000.0, ms, "malformed: offset (milliseconds, at most 12 h)")) {
					if (ms % 1000 != 0) {
						c.refuse("malformed: offset must be a whole number of seconds");
					} else {
						spec.offset_s = static_cast<int32_t>(ms / 1000);
					}
				}
			}
			if (c.member(item, "notBefore", value)) {
				Path p(c, ".notBefore");
				spec.has_not_before = read_clock(c, value, spec.not_before, "notBefore");
			}
			if (c.member(item, "notAfter", value)) {
				Path p(c, ".notAfter");
				spec.has_not_after = read_clock(c, value, spec.not_after, "notAfter");
			}
			if (spec.has_not_before && spec.has_not_after && seconds_of(spec.not_before) > seconds_of(spec.not_after)) {
				c.refuse("malformed: notBefore is later than notAfter");
			}
			break;
		}
		case ScheduleKind::interval: {
			if (required("every", value)) {
				Path p(c, ".every");
				int64_t ms = 0;
				if (c.integer(value, 60000, 43200000.0, ms, "malformed: every (milliseconds, 1 min .. 12 h)")) {
					if (ms % 60000 != 0) {
						c.refuse("malformed: every must be a whole number of minutes");
					} else {
						spec.every_minutes = static_cast<uint16_t>(ms / 60000);
					}
				}
			}
			bool from_ok = false;
			bool until_ok = false;
			if (required("from", value)) {
				Path p(c, ".from");
				from_ok = read_clock(c, value, spec.from, "from");
			}
			if (required("until", value)) {
				Path p(c, ".until");
				until_ok = read_clock(c, value, spec.until, "until");
			}
			if (from_ok && until_ok && seconds_of(spec.until) < seconds_of(spec.from)) {
				c.refuse("malformed: until is earlier than from (a window across midnight needs two entries)");
			}
			break;
		}
		case ScheduleKind::once:
			if (required("at", value)) {
				Path p(c, ".at");
				read_date(c, value, spec.at_date, &spec.at_time, "at");
			}
			break;
	}
}

void parse_schedules(Ctx& c, const JsonSpan& root, const Index& index) {
	JsonSpan list;
	if (!c.member(root, "schedules", list)) {
		return;
	}
	Path p(c, "schedules");
	if (list.kind != JsonSpan::Kind::array) {
		c.refuse("malformed: 'schedules' must be an array");
		return;
	}
	size_t count = 0;
	size_t position = 0;
	JsonSpan item;
	while (json_next_item(list, position, item)) {
		++count;
	}
	if (count > Scheduler::kMaxEntries) {
		c.refuse("limitExceeded: %u entries exceed the limit of %u", static_cast<unsigned>(count),
			static_cast<unsigned>(Scheduler::kMaxEntries));
		return;
	}
	Plan& plan = c.plan;
	plan.schedules = plan.arena.array<ScheduleSpec>(count);
	if (count > 0 && plan.schedules == nullptr) {
		c.no_memory("the schedule entries");
		return;
	}
	for (size_t i = 0; i < count; ++i) {
		new (&plan.schedules[i]) ScheduleSpec();
	}
	position = 0;
	size_t i = 0;
	while (json_next_item(list, position, item) && !c.stopped()) {
		Path q(c, "[%u]", static_cast<unsigned>(i));
		parse_schedule(c, item, plan.schedules[i], index);
		if (plan.schedules[i].id != nullptr) {
			for (size_t k = 0; k < i; ++k) {
				if (plan.schedules[k].id != nullptr && std::strcmp(plan.schedules[k].id, plan.schedules[i].id) == 0) {
					Path a(c, ".id");
					c.refuse("duplicateId: schedules[%u] has the id '%s' too", static_cast<unsigned>(k),
						plan.schedules[i].id);
				}
			}
		}
		++i;
	}
	plan.schedule_count = count;
	plan.info.schedules = count;
}

// ---- History budget -----------------------------------------------------------------------------

void parse_history(Ctx& c, const JsonSpan& root, size_t cap) {
	Plan& plan = c.plan;
	size_t needed = 0;
	for (size_t i = 0; i < plan.history_count; ++i) {
		needed += History::ring_bytes(plan.history[i].retention_s, plan.history[i].spacing_s);
	}
	JsonSpan history;
	if (!c.member(root, "history", history)) {
		plan.history_budget = needed;
		if (needed > cap) {
			Path p(c, "history");
			c.refuse("the rings need %u bytes, the device allows at most %u", static_cast<unsigned>(needed),
				static_cast<unsigned>(cap));
		}
		return;
	}
	Path p(c, "history");
	if (!c.object(history, "'history'")) {
		return;
	}
	JsonSpan budget;
	if (!c.member(history, "budget", budget)) {
		plan.history_budget = needed;
	} else {
		Path q(c, ".budget");
		int64_t bytes = 0;
		if (!c.integer(budget, 0, 16777216.0, bytes, "budget (bytes)")) {
			return;
		}
		if (static_cast<size_t>(bytes) > cap) {
			c.refuse("the image asks for %u bytes of history, the device allows at most %u",
				static_cast<unsigned>(bytes), static_cast<unsigned>(cap));
			return;
		}
		plan.history_budget = static_cast<size_t>(bytes);
	}
	if (needed > plan.history_budget) {
		c.refuse("the rings need %u bytes, the image's budget is %u", static_cast<unsigned>(needed),
			static_cast<unsigned>(plan.history_budget));
	}
}

// ---- Rules ------------------------------------------------------------------------------------

struct PlanSchedules : rules::ScheduleDirectory {
	explicit PlanSchedules(const Plan& p) : plan(p) {}
	bool has_schedule(const char* id) const override { return plan.has_schedule(id); }
	bool owns_output(const char* datapoint) const override { return plan.schedule_owns_output(datapoint); }
	const Plan& plan;
};

void parse_rules(Ctx& c, const JsonSpan& root, const Clock& clock) {
	Plan& plan = c.plan;
	JsonSpan rules_member;
	if (!c.member(root, "rules", rules_member)) {
		return;
	}
	plan.has_rules = true;
	plan.rules = rules_member;
	if (rules_member.kind == JsonSpan::Kind::array) {
		size_t position = 0;
		JsonSpan item;
		while (json_next_item(rules_member, position, item)) {
			++plan.info.rules;
		}
	}
	if (!c.report.ok()) {
		// The rules are checked against the datapoints; with refusals there the check would only
		// repeat them.
		return;
	}
	DatapointModel scratch(clock);
	DatapointStatus status = scratch.load(plan.datapoints, plan.datapoint_count);
	if (status != DatapointStatus::ok) {
		Path p(c, "datapoints");
		if (status == DatapointStatus::out_of_memory) {
			c.no_memory("checking the rules (a scratch datapoint model)");
		} else {
			c.refuse("the datapoint model refuses the datapoints: %s (the log names it)", datapoint_status_name(status));
		}
		return;
	}
	JsonSpanDocument document;
	PlanSchedules schedules(plan);
	rules::RuleSet set;
	rules::LoadResult result = set.load(document.value(rules_member), scratch, &schedules);
	if (!result.ok()) {
		Report::Kind kind =
			result.error == rules::LoadError::out_of_memory ? Report::Kind::no_memory : Report::Kind::invalid;
		c.report.add(kind, result.path, "rule '%s': %s: %s", result.rule, rules::load_error_name(result.error),
			result.text);
	}
}

} // namespace

bool Loader::parse(const char* text, size_t length, Plan& plan, Report& report) const {
	report.clear();
	plan.info.bytes = length;
	if (length > kMaxImageBytes) {
		report.add(Report::Kind::too_large, "", "the image has %u bytes, the limit is %u", static_cast<unsigned>(length),
			static_cast<unsigned>(kMaxImageBytes));
		return false;
	}
	JsonSpan root;
	const char* error = nullptr;
	size_t offset = 0;
	if (!json_parse(text, length, root, &error, kMaxNesting, &offset)) {
		report.add(Report::Kind::syntax, "", "not JSON: %s at byte %u", error != nullptr ? error : "invalid",
			static_cast<unsigned>(offset));
		return false;
	}
	Ctx c(plan, report);
	if (root.kind != JsonSpan::Kind::object) {
		c.refuse("the image must be a JSON object");
		return false;
	}
	if (!parse_image(c, root)) {
		return false;
	}
	parse_settings(c, root);
	parse_knx(c, root);
	parse_datapoints(c, root);
	if (c.stopped()) {
		return false;
	}
	Index index{plan};
	index.order = plan.arena.array<uint16_t>(plan.datapoint_count);
	if (plan.datapoint_count > 0 && index.order == nullptr) {
		c.no_memory("the datapoint index");
		return false;
	}
	for (size_t i = 0; i < plan.datapoint_count; ++i) {
		if (plan.datapoints[i].name != nullptr) {
			index.order[index.count++] = static_cast<uint16_t>(i);
		}
	}
	std::sort(index.order, index.order + index.count, [&plan](uint16_t a, uint16_t b) {
		return std::strcmp(plan.datapoints[a].name, plan.datapoints[b].name) < 0;
	});
	Ids locations;
	parse_locations(c, root, locations);
	parse_functions(c, root, locations, index);
	parse_schedules(c, root, index);
	parse_history(c, root, history_cap_);
	JsonSpan ui;
	if (c.member(root, "ui", ui)) {
		Path p(c, "ui");
		c.object(ui, "'ui'");
	}
	JsonSpan external;
	if (c.member(root, "external", external)) {
		Path p(c, "external");
		if (c.object(external, "'external'")) {
			plan.has_external = true;
			plan.external = external;
		}
	}
	JsonSpan mqtt;
	if (c.member(root, "mqtt", mqtt)) {
		Path p(c, "mqtt");
		if (c.object(mqtt, "'mqtt'")) {
			const char* reason = nullptr;
			if (Mqtt::check_json(mqtt.data, mqtt.length, &reason)) {
				plan.has_mqtt = true;
				plan.mqtt = mqtt;
			} else {
				c.refuse("%s", reason != nullptr ? reason : "invalid");
			}
		}
	}
	if (!c.stopped()) {
		parse_rules(c, root, clock_);
	}
	if (!report.ok()) {
		Log::warn(kTag, "image refused: %u problem%s, first at %s: %s", static_cast<unsigned>(report.total),
			report.total == 1 ? "" : "s", report.count > 0 ? report.refusals[0].path : "",
			report.count > 0 ? report.refusals[0].reason : "");
		return false;
	}
	return true;
}

} // namespace image
} // namespace hc
