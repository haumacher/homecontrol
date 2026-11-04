/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/mqtt.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "hc/image/loader.h"
#include "hc/mqtt_codec.h"
#include "hc/value_json.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.mqtt";
constexpr const char* kDefaultPrefix = "homeassistant";
constexpr const char* kNodePrefix = "homecontrol-";
constexpr uint16_t kNone = 0xFFFF;
constexpr size_t kMaxRoles = 6;
constexpr size_t kMaxLabel = 63;
constexpr size_t kMaxUnit = 15;
constexpr size_t kMaxFunctionId = 32;

/// The function kinds of spec/runtime-image.md 3.5, their roles in the loader's order.
enum Kind : uint8_t { kLight, kDimmer, kBlind, kSwitch, kSensor, kClimate, kScene, kButton, kKindCount };

struct KindInfo {
	const char* name;
	const char* roles[kMaxRoles];
};

constexpr KindInfo kKinds[kKindCount] = {
	{"light", {"command", "state"}},
	{"dimmer", {"levelCommand", "level", "command", "state"}},
	{"blind", {"move", "stop", "position", "positionCommand", "slat", "slatCommand"}},
	{"switch", {"command", "state"}},
	{"sensor", {"value"}},
	{"climate", {"temperature", "setpoint", "setpointCommand", "mode", "modeCommand", "heating"}},
	{"scene", {"command"}},
	{"button", {"press", "state"}},
};

// Role indexes per kind.
enum : uint8_t { kLightCommand = 0, kLightState = 1 };
enum : uint8_t { kDimLevelCommand = 0, kDimLevel = 1, kDimCommand = 2, kDimState = 3 };
enum : uint8_t { kBlindMove = 0, kBlindStop = 1, kBlindPosition = 2, kBlindPositionCommand = 3, kBlindSlat = 4,
	kBlindSlatCommand = 5 };
enum : uint8_t { kSensorValue = 0 };
enum : uint8_t { kClimateTemperature = 0, kClimateSetpoint = 1, kClimateSetpointCommand = 2, kClimateHeating = 5 };
enum : uint8_t { kButtonPress = 0, kButtonState = 1 };

// Home Assistant templates over the state payload {"value": ..., "quality": ..., "time": ...}.
constexpr const char* kValueTemplate = "{{ value_json.value }}";
constexpr const char* kBoolTemplate = "{{ 'true' if value_json.value else 'false' }}";
constexpr const char* kOnOffTemplate = "{{ 'ON' if value_json.value else 'OFF' }}";
constexpr const char* kControlledOnOffTemplate = "{{ 'ON' if value_json.value.value else 'OFF' }}";
constexpr const char* kLevelOnTemplate = "{{ 'true' if value_json.value > 0 else 'false' }}";
constexpr const char* kHeatingTemplate = "{{ 'heating' if value_json.value else 'idle' }}";
constexpr const char* kTimeTemplate =
	"{{ '%02d:%02d:%02d' | format(value_json.value.hour, value_json.value.minute, value_json.value.second) }}";
constexpr const char* kDateTemplate =
	"{{ '%04d-%02d-%02d' | format(value_json.value.year, value_json.value.month, value_json.value.day) }}";
constexpr const char* kDateTimeTemplate =
	"{{ '%04d-%02d-%02d %02d:%02d:%02d' | format(value_json.value.year, value_json.value.month, "
	"value_json.value.day, value_json.value.hour, value_json.value.minute, value_json.value.second) }}";
constexpr const char* kJsonTemplate = "{{ value_json.value | tojson }}";

bool set_error(const char** error, const char* text) {
	if (error != nullptr) {
		*error = text;
	}
	return false;
}

bool numeric(ValueType type) {
	return type == ValueType::unsigned_int || type == ValueType::signed_int || type == ValueType::real;
}

/// A base topic or discovery prefix: 1..max bytes of printable ASCII without wildcards, no
/// leading or trailing '/', no empty level.
bool topic_root_valid(const char* text, size_t max) {
	size_t length = std::strlen(text);
	if (length == 0 || length > max || text[0] == '/' || text[length - 1] == '/') {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		char c = text[i];
		if (c <= 0x20 || c >= 0x7F || c == '+' || c == '#' || (c == '/' && text[i + 1] == '/')) {
			return false;
		}
	}
	return true;
}

/// Appends `name` percent-encoded ('/', '+', '#', '%' and control bytes) to `out` at `length`.
bool append_encoded(char* out, size_t size, size_t& length, const char* name) {
	static const char kHex[] = "0123456789ABCDEF";
	for (const char* p = name; *p != '\0'; ++p) {
		unsigned char c = static_cast<unsigned char>(*p);
		if (c == '/' || c == '+' || c == '#' || c == '%' || c < 0x20 || c == 0x7F) {
			if (length + 3 >= size) {
				return false;
			}
			out[length++] = '%';
			out[length++] = kHex[c >> 4];
			out[length++] = kHex[c & 0x0F];
		} else {
			if (length + 1 >= size) {
				return false;
			}
			out[length++] = static_cast<char>(c);
		}
	}
	out[length] = '\0';
	return true;
}

int hex_digit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

/// Decodes `length` bytes of an encoded topic level into `out`; false if malformed or too long.
bool decode_level(const char* text, size_t length, char* out, size_t size) {
	size_t n = 0;
	for (size_t i = 0; i < length; ++i) {
		char c = text[i];
		if (c == '%') {
			if (i + 2 >= length) {
				return false;
			}
			int hi = hex_digit(text[i + 1]);
			int lo = hex_digit(text[i + 2]);
			if (hi < 0 || lo < 0) {
				return false;
			}
			c = static_cast<char>((hi << 4) | lo);
			i += 2;
			if (c == '\0') {
				return false;
			}
		}
		if (n + 1 >= size) {
			return false;
		}
		out[n++] = c;
	}
	out[n] = '\0';
	return n > 0;
}

/// The text of a label (a string, or an object of languages: "en", else the first).
bool label_text(const JsonSpan& span, char* out, size_t size) {
	if (span.kind == JsonSpan::Kind::string) {
		return json_string(span, out, size);
	}
	if (span.kind == JsonSpan::Kind::object) {
		JsonSpan value;
		if (json_member(span, "en", value) && json_string(value, out, size)) {
			return true;
		}
		size_t position = 0;
		JsonSpan name;
		if (json_next_member(span, position, name, value)) {
			return json_string(value, out, size);
		}
	}
	return false;
}

/// The device class and state class Home Assistant gets for a sensor with `unit`.
void classes_of(const char* unit, const char*& device_class, const char*& state_class) {
	device_class = nullptr;
	state_class = "measurement";
	if (unit == nullptr) {
		return;
	}
	struct Entry {
		const char* unit;
		const char* device_class;
	};
	static const Entry kEntries[] = {
		{"\xc2\xb0" "C", "temperature"}, {"\xc2\xb0" "F", "temperature"}, {"K", "temperature"},
		{"%", "humidity"}, {"lx", "illuminance"}, {"W", "power"}, {"kW", "power"}, {"Wh", "energy"},
		{"kWh", "energy"}, {"V", "voltage"}, {"A", "current"}, {"hPa", "pressure"}, {"mbar", "pressure"},
		{"m/s", "wind_speed"}, {"km/h", "wind_speed"},
	};
	for (const Entry& e : kEntries) {
		if (std::strcmp(unit, e.unit) == 0) {
			device_class = e.device_class;
			if (std::strcmp(e.device_class, "energy") == 0) {
				state_class = "total_increasing";
			}
			return;
		}
	}
}

bool read_bool(const JsonSpan& object, const char* name, bool& out, bool fallback, const char*& error,
	const char* message) {
	JsonSpan value;
	out = fallback;
	if (!json_member(object, name, value)) {
		return true;
	}
	if (!json_bool(value, out)) {
		error = message;
		return false;
	}
	return true;
}

/// The checked settings of the `mqtt` member.
struct MemberConfig {
	bool enabled = true;
	char broker[Mqtt::kMaxBroker + 1] = {};
	char username[Mqtt::kMaxUser + 1] = {};
	bool has_base = false;
	char base[Mqtt::kMaxBaseTopic + 1] = {};
	bool retain = true;
	bool discovery = true;
	char prefix[Mqtt::kMaxPrefix + 1] = {};
	bool history = true;
	uint32_t interval_min = Mqtt::kDefaultHistoryIntervalMin;
};

/// Parses and checks the `mqtt` member; false with `error` (static text naming the member).
bool parse_member(const JsonSpan& member, MemberConfig& out, const char*& error) {
	if (member.kind != JsonSpan::Kind::object) {
		error = "mqtt: must be an object";
		return false;
	}
	JsonSpan value;
	if (json_member(member, "password", value)) {
		error = "mqtt.password: the broker password is the device setting mqtt.password, never part of the image";
		return false;
	}
	if (!read_bool(member, "enabled", out.enabled, true, error, "mqtt.enabled: must be true or false")) {
		return false;
	}
	if (!json_member(member, "broker", value)) {
		error = "mqtt.broker: is missing (mqtt://host[:port] or mqtts://host[:port])";
		return false;
	}
	if (!json_string(value, out.broker, sizeof(out.broker))) {
		error = "mqtt.broker: must be a string of at most 127 bytes";
		return false;
	}
	mqtt::Broker broker;
	const char* reason = nullptr;
	if (!mqtt::parse_broker(out.broker, broker, &reason)) {
		error = reason;
		return false;
	}
	if (json_member(member, "username", value)) {
		if (!json_string(value, out.username, sizeof(out.username)) || out.username[0] == '\0') {
			error = "mqtt.username: must be a string of 1..63 bytes";
			return false;
		}
	}
	if (json_member(member, "baseTopic", value)) {
		if (!json_string(value, out.base, sizeof(out.base)) || !topic_root_valid(out.base, Mqtt::kMaxBaseTopic)) {
			error = "mqtt.baseTopic: must be 1..64 bytes of printable ASCII without '+', '#', spaces, empty levels "
					"or a leading or trailing '/'";
			return false;
		}
		out.has_base = true;
	}
	if (!read_bool(member, "retain", out.retain, true, error, "mqtt.retain: must be true or false")) {
		return false;
	}
	std::strcpy(out.prefix, kDefaultPrefix);
	if (json_member(member, "discovery", value)) {
		if (value.kind != JsonSpan::Kind::object) {
			error = "mqtt.discovery: must be an object";
			return false;
		}
		if (!read_bool(value, "enabled", out.discovery, true, error, "mqtt.discovery.enabled: must be true or false")) {
			return false;
		}
		JsonSpan prefix;
		if (json_member(value, "prefix", prefix)) {
			if (!json_string(prefix, out.prefix, sizeof(out.prefix)) ||
				!topic_root_valid(out.prefix, Mqtt::kMaxPrefix)) {
				error = "mqtt.discovery.prefix: must be 1..32 bytes of printable ASCII without '+', '#', spaces, "
						"empty levels or a leading or trailing '/'";
				return false;
			}
		}
	}
	if (json_member(member, "history", value)) {
		if (value.kind != JsonSpan::Kind::object) {
			error = "mqtt.history: must be an object";
			return false;
		}
		if (!read_bool(value, "enabled", out.history, true, error, "mqtt.history.enabled: must be true or false")) {
			return false;
		}
		JsonSpan interval;
		if (json_member(value, "interval", interval)) {
			double minutes = 0;
			if (!json_number(interval, minutes) || minutes < 1 || minutes > Mqtt::kMaxHistoryIntervalMin ||
				minutes != std::floor(minutes)) {
				error = "mqtt.history.interval: must be an integer of 1..1440 minutes";
				return false;
			}
			out.interval_min = static_cast<uint32_t>(minutes);
		}
	}
	return true;
}

} // namespace

const char* mqtt_state_name(MqttState state) {
	switch (state) {
		case MqttState::disabled: return "disabled";
		case MqttState::waiting: return "waiting for the network";
		case MqttState::backoff: return "backoff";
		case MqttState::connecting: return "connecting";
		case MqttState::connected: return "connected";
		case MqttState::retiring: return "retiring";
	}
	return "?";
}

struct Mqtt::Function {
	const char* id;
	const char* label;
	const char* unit; ///< The function's unit, else the value datapoint's (sensor); nullptr: none.
	uint8_t kind;
	uint8_t scene;
	uint16_t roles[kMaxRoles];

	bool has(uint8_t role) const { return roles[role] != kNone; }
};

struct Mqtt::Point {
	const char* name;     ///< The datapoint's name, kept for a history ring only (its cursor follows it).
	const char* unit;     ///< From the image; nullptr: none.
	uint32_t retention_s; ///< 0: no history ring.
	uint32_t spacing_s;
	int64_t cursor;       ///< Time of the last exported sample; 0: none yet.
	bool dirty;
};

struct Mqtt::Buffers {
	char payload[kMaxPayload];
	HistoryPoint points[kHistoryBatch];
};

struct Mqtt::Table {
	image::Arena arena;
	MemberConfig config;
	const char* device_name = nullptr;
	Function* functions = nullptr;
	size_t function_count = 0;
	Point* points = nullptr;
	size_t point_count = 0;
};

Mqtt::Mqtt(EventLoop& loop, const Clock& clock, const Settings& settings, const Connectivity& connectivity,
	DatapointModel& model, MqttClient& client, const MqttIdentity& identity, const History* history)
	: loop_(loop),
	  clock_(clock),
	  settings_(settings),
	  connectivity_(connectivity),
	  model_(model),
	  client_(client),
	  history_(history),
	  backoff_(kRetryInitialMs, kRetryCapMs) {
	const char* node = identity.node_id != nullptr ? identity.node_id : "";
	size_t length = std::strlen(node);
	bool ok = length > 0 && length <= kMaxNodeId;
	for (size_t i = 0; ok && i < length; ++i) {
		char c = node[i];
		ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
	}
	if (!ok) {
		Log::error(kTag, "node id \"%s\" is not 1..48 characters of [A-Za-z0-9_-]; using \"homecontrol\"", node);
		node = "homecontrol";
	}
	std::snprintf(node_id_, sizeof(node_id_), "%s", node);
	std::snprintf(sw_version_, sizeof(sw_version_), "%s", identity.sw_version != nullptr ? identity.sw_version : "");
}

Mqtt::~Mqtt() {
	loop_.cancel(timer_);
	if (listening_) {
		model_.remove_listener(*this);
	}
	// No disconnect(): when the platform's client goes away without DISCONNECT, the broker publishes
	// the last will (offline).
	delete table_;
	delete next_;
	delete buffers_;
}

bool Mqtt::ensure_buffers(const char** error) {
	if (buffers_ == nullptr) {
		buffers_ = new (std::nothrow) Buffers;
		if (buffers_ == nullptr) {
			return refuse(error, "no memory for the message buffers (%u bytes)", static_cast<unsigned>(sizeof(Buffers)));
		}
	}
	return true;
}

void Mqtt::release_buffers_if_idle() {
	if (table_ == nullptr && next_ == nullptr) {
		delete buffers_;
		buffers_ = nullptr;
	}
}

bool Mqtt::refuse(const char** error, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(error_, sizeof(error_), fmt, args);
	va_end(args);
	Log::error(kTag, "configuration refused: %s", error_);
	return set_error(error, error_);
}

bool Mqtt::check_json(const char* text, size_t length, const char** error) {
	JsonSpan member;
	if (!json_parse(text, length, member, error)) {
		return false;
	}
	MemberConfig config;
	const char* reason = nullptr;
	if (!parse_member(member, config, reason)) {
		return set_error(error, reason);
	}
	return true;
}

bool Mqtt::configure_json(const char* text, size_t length, const char** error) {
	MqttImage image;
	const char* reason = nullptr;
	if (!json_parse(text, length, image.mqtt, &reason)) {
		return refuse(error, "mqtt: not JSON: %s", reason);
	}
	return configure(image, error);
}

bool Mqtt::configure_image(const char* text, size_t length, const char** error) {
	JsonSpan root;
	const char* reason = nullptr;
	if (!json_parse(text, length, root, &reason, kJsonMaxDepth) || root.kind != JsonSpan::Kind::object) {
		return refuse(error, "the image is not a JSON object: %s", reason != nullptr ? reason : "not an object");
	}
	MqttImage image;
	if (!json_member(root, "mqtt", image.mqtt)) {
		clear();
		return true;
	}
	json_member(root, "datapoints", image.datapoints);
	json_member(root, "functions", image.functions);
	JsonSpan info;
	if (json_member(root, "image", info)) {
		json_member(info, "name", image.name);
	}
	return configure(image, error);
}

bool Mqtt::build(Table& table, const MqttImage& image, const char** error) {
	const char* reason = nullptr;
	if (!parse_member(image.mqtt, table.config, reason)) {
		return refuse(error, "%s", reason);
	}
	MemberConfig& config = table.config;
	if (!config.has_base) {
		const char* suffix = node_id_;
		size_t prefix_length = std::strlen(kNodePrefix);
		if (std::strncmp(node_id_, kNodePrefix, prefix_length) == 0 && node_id_[prefix_length] != '\0') {
			suffix = node_id_ + prefix_length;
		}
		std::snprintf(config.base, sizeof(config.base), "homecontrol/%s", suffix);
	}
	if (image.name.kind == JsonSpan::Kind::string) {
		char name[kMaxLabel + 1];
		if (json_string(image.name, name, sizeof(name)) && name[0] != '\0') {
			table.device_name = table.arena.copy(name, std::strlen(name));
		}
	}

	// The per-datapoint table, indexed by the model's ids.
	table.point_count = model_.size();
	table.points = table.arena.array<Point>(table.point_count);
	if (table.point_count > 0 && table.points == nullptr) {
		return refuse(error, "no memory for the datapoint table (%u datapoints)", static_cast<unsigned>(table.point_count));
	}
	if (image.datapoints.kind == JsonSpan::Kind::array) {
		size_t position = 0;
		JsonSpan item;
		while (json_next_item(image.datapoints, position, item)) {
			JsonSpan value;
			char name[Datapoint::kMaxName + 1];
			if (!json_member(item, "id", value) || !json_string(value, name, sizeof(name))) {
				continue;
			}
			const Datapoint* dp = model_.find(name);
			if (dp == nullptr) {
				continue;
			}
			Point& point = table.points[dp->id()];
			char unit[kMaxUnit + 1];
			if (json_member(item, "unit", value) && json_string(value, unit, sizeof(unit)) && unit[0] != '\0') {
				point.unit = table.arena.copy(unit, std::strlen(unit));
			}
			JsonSpan history;
			double retention = 0;
			double spacing = 0;
			if (json_member(item, "history", history) && json_member(history, "retention", value) &&
				json_number(value, retention) && json_member(history, "spacing", value) &&
				json_number(value, spacing) && spacing >= 1 && retention >= spacing && retention <= 4e9) {
				point.retention_s = static_cast<uint32_t>(retention);
				point.spacing_s = static_cast<uint32_t>(spacing);
				point.name = table.arena.copy(name, std::strlen(name));
				if (point.name == nullptr) {
					return refuse(error, "no memory for the history table");
				}
			}
		}
	}

	// The functions.
	if (image.functions.kind == JsonSpan::Kind::array) {
		size_t count = 0;
		size_t position = 0;
		JsonSpan item;
		while (json_next_item(image.functions, position, item)) {
			++count;
		}
		table.functions = table.arena.array<Function>(count);
		if (count > 0 && table.functions == nullptr) {
			return refuse(error, "no memory for %u functions", static_cast<unsigned>(count));
		}
		position = 0;
		size_t index = 0;
		while (json_next_item(image.functions, position, item)) {
			size_t at = index++;
			JsonSpan value;
			char id[kMaxFunctionId + 1];
			if (!json_member(item, "id", value) || !json_string(value, id, sizeof(id)) || id[0] == '\0') {
				return refuse(error, "functions[%u].id: missing or longer than 32 bytes", static_cast<unsigned>(at));
			}
			char kind_name[16];
			uint8_t kind = kKindCount;
			if (json_member(item, "kind", value) && json_string(value, kind_name, sizeof(kind_name))) {
				for (uint8_t k = 0; k < kKindCount; ++k) {
					if (std::strcmp(kKinds[k].name, kind_name) == 0) {
						kind = k;
					}
				}
			}
			if (kind == kKindCount) {
				Log::warn(kTag, "function '%s': kind unknown to this firmware, not announced", id);
				continue;
			}
			Function& function = table.functions[table.function_count];
			function.kind = kind;
			function.id = table.arena.copy(id, std::strlen(id));
			char label[kMaxLabel + 1];
			if (!json_member(item, "label", value) || !label_text(value, label, sizeof(label))) {
				std::snprintf(label, sizeof(label), "%s", id);
			}
			function.label = table.arena.copy(label, std::strlen(label));
			char unit[kMaxUnit + 1];
			if (json_member(item, "unit", value) && json_string(value, unit, sizeof(unit)) && unit[0] != '\0') {
				function.unit = table.arena.copy(unit, std::strlen(unit));
			}
			double scene = 0;
			if (json_member(item, "scene", value) && json_number(value, scene) && scene >= 1 && scene <= 64) {
				function.scene = static_cast<uint8_t>(scene);
			}
			JsonSpan roles;
			json_member(item, "datapoints", roles);
			for (size_t r = 0; r < kMaxRoles; ++r) {
				function.roles[r] = kNone;
				const char* role = kKinds[kind].roles[r];
				char name[Datapoint::kMaxName + 1];
				if (role == nullptr || !json_member(roles, role, value) || !json_string(value, name, sizeof(name))) {
					continue;
				}
				const Datapoint* dp = model_.find(name);
				if (dp == nullptr) {
					return refuse(error, "functions[%u].datapoints.%s: unknown datapoint '%s'", static_cast<unsigned>(at),
						role, name);
				}
				function.roles[r] = dp->id();
			}
			if (function.roles[0] == kNone) {
				return refuse(error, "functions[%u].datapoints.%s: a %s needs it", static_cast<unsigned>(at),
					kKinds[kind].roles[0], kKinds[kind].name);
			}
			if (function.unit == nullptr && kind == kSensor && function.roles[kSensorValue] < table.point_count) {
				function.unit = table.points[function.roles[kSensorValue]].unit;
			}
			if (function.id == nullptr || function.label == nullptr) {
				return refuse(error, "no memory for the functions");
			}
			++table.function_count;
		}
	}
	return true;
}

bool Mqtt::configure(const MqttImage& image, const char** error) {
	if (image.mqtt.kind == JsonSpan::Kind::invalid) {
		clear();
		return true;
	}
	if (!ensure_buffers(error)) {
		return false;
	}
	Table* table = new (std::nothrow) Table();
	if (table == nullptr) {
		release_buffers_if_idle();
		return refuse(error, "no memory for the configuration");
	}
	if (!build(*table, image, error)) {
		delete table;
		release_buffers_if_idle();
		return false;
	}
	bool ok = commit(table, error);
	release_buffers_if_idle(); // a refused first configuration
	return ok;
}

bool Mqtt::model_reloaded(const char** error) {
	const Table* current = next_ != nullptr ? next_ : table_;
	if (current == nullptr) {
		return true;
	}
	Table* table = new (std::nothrow) Table;
	if (table == nullptr) {
		return refuse(error, "no memory for the datapoint table");
	}
	table->config = current->config;
	if (current->device_name != nullptr) {
		table->device_name = table->arena.copy(current->device_name, std::strlen(current->device_name));
	}
	// The image's datapoints come first in the composed model and keep their ids; the providers'
	// follow and may be more or fewer than before.
	table->point_count = model_.size();
	table->points = table->arena.array<Point>(table->point_count);
	if (table->point_count > 0 && table->points == nullptr) {
		delete table;
		return refuse(error, "no memory for the datapoint table (%u datapoints)", static_cast<unsigned>(model_.size()));
	}
	size_t carried = current->point_count < table->point_count ? current->point_count : table->point_count;
	for (size_t i = 0; i < carried; ++i) {
		const Point& old = current->points[i];
		Point& point = table->points[i];
		point.retention_s = old.retention_s;
		point.spacing_s = old.spacing_s;
		if (old.unit != nullptr) {
			point.unit = table->arena.copy(old.unit, std::strlen(old.unit));
		}
		if (old.name != nullptr) {
			point.name = table->arena.copy(old.name, std::strlen(old.name));
			if (point.name == nullptr) {
				delete table;
				return refuse(error, "no memory for the history table");
			}
		}
	}
	table->function_count = current->function_count;
	if (table->function_count > 0) {
		table->functions = table->arena.array<Function>(table->function_count);
		if (table->functions == nullptr) {
			delete table;
			return refuse(error, "no memory for %u functions", static_cast<unsigned>(current->function_count));
		}
		for (size_t i = 0; i < table->function_count; ++i) {
			const Function& old = current->functions[i];
			Function& function = table->functions[i];
			function = old;
			function.id = table->arena.copy(old.id, std::strlen(old.id));
			function.label = table->arena.copy(old.label, std::strlen(old.label));
			function.unit = old.unit != nullptr ? table->arena.copy(old.unit, std::strlen(old.unit)) : nullptr;
			if (function.id == nullptr || function.label == nullptr) {
				delete table;
				return refuse(error, "no memory for the functions");
			}
		}
	}
	Log::info(kTag, "model reloaded: %u datapoint(s) now, states published again", static_cast<unsigned>(table->point_count));
	return commit(table, error);
}

bool Mqtt::commit(Table* table, const char** error) {
	if (!listening_) {
		DatapointStatus status = model_.add_listener(*this);
		if (status != DatapointStatus::ok) {
			delete table;
			return refuse(error, "cannot observe the datapoints: %s", datapoint_status_name(status));
		}
		listening_ = true;
	}
	// History cursors follow the datapoint's name (the previous table kept the names of its rings).
	const Table* previous = next_ != nullptr ? next_ : table_;
	if (previous != nullptr) {
		for (size_t i = 0; i < table->point_count; ++i) {
			Point& point = table->points[i];
			if (point.name == nullptr) {
				continue;
			}
			for (size_t j = 0; j < previous->point_count; ++j) {
				const Point& old = previous->points[j];
				if (old.name != nullptr && std::strcmp(old.name, point.name) == 0) {
					point.cursor = old.cursor;
					break;
				}
			}
		}
	}
	const MemberConfig& c = table->config;
	char history[24] = "off";
	if (c.history) {
		std::snprintf(history, sizeof(history), "every %u min", static_cast<unsigned>(c.interval_min));
	}
	Log::info(kTag, "configured: broker %s, base topic %s, discovery %s, %u function(s), history export %s",
		c.broker, c.base, c.discovery ? c.prefix : "off", static_cast<unsigned>(table->function_count), history);
	if (!c.enabled) {
		Log::info(kTag, "disabled by the image (mqtt.enabled false)");
	}

	switch (state_) {
		case MqttState::retiring:
			delete next_;
			next_ = table;
			return true;
		case MqttState::connected: {
			const MemberConfig& o = table_->config;
			bool same = c.enabled && std::strcmp(o.broker, c.broker) == 0 && std::strcmp(o.username, c.username) == 0 &&
				std::strcmp(o.base, c.base) == 0;
			if (same) {
				delete table_;
				table_ = table;
				start_session_work();
				return true;
			}
			bool remove = !c.enabled || !c.discovery || std::strcmp(o.broker, c.broker) != 0 ||
				std::strcmp(o.prefix, c.prefix) != 0;
			remove_configs_ = remove && o.discovery;
			begin_retire(table);
			return true;
		}
		case MqttState::connecting:
			client_.disconnect();
			break;
		default:
			break;
	}
	delete table_;
	table_ = table;
	backoff_.reset();
	last_error_[0] = '\0';
	attempt_at_ = clock_.monotonic_ms();
	if (c.enabled) {
		attempt();
	} else {
		set_state(MqttState::disabled);
		arm();
	}
	return true;
}

void Mqtt::stop() {
	if (state_ == MqttState::connected) {
		Log::info(kTag, "stopping: offline, the discovery configs stay for the next run");
		remove_configs_ = false;
		begin_retire(nullptr);
		return;
	}
	clear();
}

void Mqtt::clear() {
	if (table_ == nullptr && next_ == nullptr) {
		return;
	}
	Log::info(kTag, "configuration cleared (the image has no mqtt member)");
	if (state_ == MqttState::connected) {
		remove_configs_ = table_->config.discovery;
		begin_retire(nullptr);
		return;
	}
	if (state_ == MqttState::retiring) {
		delete next_;
		next_ = nullptr;
		return;
	}
	if (state_ == MqttState::connecting) {
		client_.disconnect();
	}
	delete table_;
	table_ = nullptr;
	release_buffers_if_idle();
	if (listening_) {
		model_.remove_listener(*this);
		listening_ = false;
	}
	set_state(MqttState::disabled);
	arm();
}

void Mqtt::reconnect(const char* why) {
	if (table_ == nullptr || !table_->config.enabled || state_ == MqttState::retiring) {
		return;
	}
	Log::info(kTag, "reconnecting: %s", why);
	if (state_ == MqttState::connected || state_ == MqttState::connecting) {
		client_.disconnect();
	}
	sweeping_ = false;
	backoff_.reset();
	attempt();
}

bool Mqtt::publish_discovery(const char** error) {
	if (state_ != MqttState::connected) {
		return set_error(error, "not connected to the broker");
	}
	if (!table_->config.discovery) {
		return set_error(error, "discovery is off in the image (mqtt.discovery.enabled false)");
	}
	discovery_next_ = 0;
	if (sweeping_) {
		sweep_again_ = true;
	} else {
		start_sweep();
	}
	request_pump();
	return true;
}

const char* Mqtt::base_topic() const { return table_ != nullptr ? table_->config.base : ""; }

void Mqtt::status(MqttStatus& out) const {
	out = MqttStatus();
	out.state = state_;
	out.last_error = last_error_;
	out.published = published_;
	out.commands = commands_;
	out.refusals = refusals_;
	out.connects = connects_;
	out.failures = backoff_.failures();
	out.next_attempt_ms = (state_ == MqttState::waiting || state_ == MqttState::backoff) ? attempt_at_ : 0;
	out.dirty = dirty_;
	out.pending = client_.pending();
	out.history_messages = history_messages_;
	if (table_ != nullptr) {
		const MemberConfig& c = table_->config;
		out.broker = c.broker;
		out.username = c.username;
		out.base_topic = c.base;
		out.discovery = c.discovery;
		out.discovery_prefix = c.prefix;
		out.functions = table_->function_count;
		out.history = c.history && history_ != nullptr;
		out.history_interval_min = c.interval_min;
	}
	char password[Settings::kMaxStringLength + 1];
	out.password_set = settings_.get_string(setting::kMqttPassword, password, sizeof(password)) == SettingsStatus::ok &&
		password[0] != '\0';
}

// ---- Connection --------------------------------------------------------------------------------

void Mqtt::set_state(MqttState state) {
	state_ = state;
}

void Mqtt::attempt() {
	if (table_ == nullptr || !table_->config.enabled) {
		set_state(MqttState::disabled);
		arm();
		return;
	}
	uint64_t now = clock_.monotonic_ms();
	if (!connectivity_.is_up()) {
		if (state_ != MqttState::waiting) {
			Log::info(kTag, "waiting for the network");
		}
		set_state(MqttState::waiting);
		attempt_at_ = now + kReadyPollMs;
		arm();
		return;
	}
	const MemberConfig& c = table_->config;
	char user[Settings::kMaxStringLength + 1] = {};
	if (c.username[0] != '\0') {
		std::snprintf(user, sizeof(user), "%s", c.username);
	} else if (settings_.get_string(setting::kMqttUser, user, sizeof(user)) != SettingsStatus::ok) {
		user[0] = '\0';
	}
	char password[Settings::kMaxStringLength + 1] = {};
	SettingsStatus status = settings_.get_string(setting::kMqttPassword, password, sizeof(password));
	if (status != SettingsStatus::ok && status != SettingsStatus::not_found) {
		Log::warn(kTag, "setting %s unusable (%s): connecting without a password", setting::kMqttPassword,
			settings_status_name(status));
		password[0] = '\0';
	}
	if (password[0] != '\0' && user[0] == '\0') {
		Log::warn(kTag, "a password is set but no user name (mqtt.username in the image or the setting %s): "
			"MQTT sends no password without a user name", setting::kMqttUser);
	}
	char will[kMaxBaseTopic + 8];
	std::snprintf(will, sizeof(will), "%s/status", c.base);
	MqttConnectOptions options;
	options.broker = c.broker;
	options.client_id = node_id_;
	options.username = user;
	options.password = password;
	options.will_topic = will;
	options.will_payload = "offline";
	options.will_retain = true;
	options.will_qos = 1;
	options.keepalive_s = kKeepaliveS;
	const char* error = nullptr;
	bool started = client_.connect(options, *this, &error);
	std::memset(password, 0, sizeof(password));
	if (!started) {
		fail(error != nullptr ? error : "the client refused to connect");
		return;
	}
	Log::info(kTag, "connecting to %s as %s%s%s", c.broker, node_id_, user[0] != '\0' ? ", user " : "", user);
	set_state(MqttState::connecting);
	connect_deadline_ = now + kConnectTimeoutMs;
	arm();
}

void Mqtt::fail(const char* reason) {
	uint32_t delay = backoff_.next();
	bool repeated = std::strcmp(last_error_, reason) == 0;
	std::snprintf(last_error_, sizeof(last_error_), "%s", reason);
	if (repeated) {
		Log::info(kTag, "%s; retry in %u s (failure %u)", reason, static_cast<unsigned>(delay / 1000),
			static_cast<unsigned>(backoff_.failures()));
	} else {
		Log::warn(kTag, "%s; retry in %u s (failure %u)", reason, static_cast<unsigned>(delay / 1000),
			static_cast<unsigned>(backoff_.failures()));
	}
	sweeping_ = false;
	set_state(MqttState::backoff);
	attempt_at_ = clock_.monotonic_ms() + delay;
	arm();
}

void Mqtt::mqtt_connected() {
	if (state_ != MqttState::connecting) {
		return;
	}
	set_state(MqttState::connected);
	backoff_.reset();
	last_error_[0] = '\0';
	++connects_;
	Log::info(kTag, "connected to %s; base topic %s", table_->config.broker, table_->config.base);
	start_session_work();
	if (table_->config.history && history_ != nullptr) {
		history_due_ = clock_.monotonic_ms() + static_cast<uint64_t>(table_->config.interval_min) * 60000;
	}
	arm();
}

void Mqtt::mqtt_disconnected(const char* reason) {
	char text[96];
	switch (state_) {
		case MqttState::connecting:
			std::snprintf(text, sizeof(text), "cannot connect: %s", reason);
			fail(text);
			break;
		case MqttState::connected:
			std::snprintf(text, sizeof(text), "connection lost: %s", reason);
			fail(text);
			break;
		case MqttState::retiring:
			finish_retire();
			break;
		default:
			break;
	}
}

void Mqtt::start_session_work() {
	online_sent_ = false;
	subscribed_ = false;
	discovery_next_ = 0;
	dirty_ = 0;
	dirty_scan_ = 0;
	for (size_t i = 0; i < table_->point_count; ++i) {
		const Datapoint* dp = model_.get(static_cast<uint16_t>(i));
		bool dirty = dp != nullptr && !dp->value().empty();
		table_->points[i].dirty = dirty;
		if (dirty) {
			++dirty_;
		}
	}
	export_active_ = false;
	if (table_->config.history && history_ != nullptr) {
		export_active_ = true;
		export_index_ = 0;
		export_to_ = 0; // taken when the export starts
	}
	if (sweeping_) {
		sweep_again_ = true;
	}
	request_pump();
}

void Mqtt::start_sweep() {
	const MemberConfig& c = table_->config;
	char filter[MqttClient::kMaxTopic + 1];
	const char* error = nullptr;
	std::snprintf(filter, sizeof(filter), "%s/+/%s/+/config", c.prefix, node_id_);
	bool ok = client_.subscribe(filter, 1, &error);
	std::snprintf(filter, sizeof(filter), "%s/dp/+/state", c.base);
	ok = ok && client_.subscribe(filter, 1, &error);
	if (!ok) {
		Log::warn(kTag, "cannot start the sweep of retained topics: %s; trying again", error != nullptr ? error : "?");
		sweep_again_ = true;
		sweeping_ = false;
		sweep_end_ = clock_.monotonic_ms() + kSweepMs;
		arm();
		return;
	}
	sweeping_ = true;
	sweep_again_ = false;
	sweep_end_ = clock_.monotonic_ms() + kSweepMs;
	arm();
}

void Mqtt::end_sweep() {
	sweep_end_ = 0;
	if (sweeping_) {
		const MemberConfig& c = table_->config;
		char filter[MqttClient::kMaxTopic + 1];
		const char* error = nullptr;
		std::snprintf(filter, sizeof(filter), "%s/+/%s/+/config", c.prefix, node_id_);
		client_.unsubscribe(filter, &error);
		std::snprintf(filter, sizeof(filter), "%s/dp/+/state", c.base);
		client_.unsubscribe(filter, &error);
		sweeping_ = false;
	}
	if (sweep_again_) {
		start_sweep();
	}
}

void Mqtt::begin_retire(Table* next) {
	delete next_;
	next_ = next;
	set_state(MqttState::retiring);
	sweeping_ = false;
	retire_index_ = 0;
	offline_sent_ = false;
	retire_deadline_ = 0;
	retire_poll_at_ = 0;
	Log::info(kTag, "retiring the connection to %s%s", table_->config.broker,
		remove_configs_ ? " (clearing the discovery configs)" : "");
	request_pump();
}

void Mqtt::pump_retire() {
	uint64_t now = clock_.monotonic_ms();
	size_t budget = kPublishesPerTurn;
	if (remove_configs_) {
		while (retire_index_ < table_->function_count) {
			if (budget == 0) {
				request_pump();
				return;
			}
			const Function& function = table_->functions[retire_index_];
			if (config_topic(topic_, sizeof(topic_), component_of(function), function.id) == 0 ||
				send(topic_, "", 0, true, 1)) {
				++retire_index_;
				--budget;
				continue;
			}
			retry_publish_at_ = now + kRetryPublishMs;
			arm();
			return;
		}
	}
	if (!offline_sent_) {
		std::snprintf(topic_, sizeof(topic_), "%s/status", table_->config.base);
		if (!send(topic_, "offline", 7, true, 1)) {
			retry_publish_at_ = now + kRetryPublishMs;
			arm();
			return;
		}
		offline_sent_ = true;
		retire_deadline_ = now + kRetireMs;
	}
	if (client_.pending() == 0 || now >= retire_deadline_) {
		finish_retire();
		return;
	}
	retire_poll_at_ = now + kRetirePollMs;
	arm();
}

void Mqtt::finish_retire() {
	client_.disconnect();
	Log::info(kTag, "disconnected from %s", table_->config.broker);
	delete table_;
	table_ = next_;
	next_ = nullptr;
	remove_configs_ = false;
	retry_publish_at_ = 0;
	retire_poll_at_ = 0;
	retire_deadline_ = 0;
	history_due_ = 0;
	sweep_end_ = 0;
	set_state(MqttState::disabled);
	if (table_ == nullptr) {
		release_buffers_if_idle();
		if (listening_) {
			model_.remove_listener(*this);
			listening_ = false;
		}
		arm();
		return;
	}
	backoff_.reset();
	attempt();
}

// ---- Timer -------------------------------------------------------------------------------------

void Mqtt::arm() {
	uint64_t at = EventLoop::kNever;
	auto consider = [&at](uint64_t t) {
		if (t != 0 && t < at) {
			at = t;
		}
	};
	switch (state_) {
		case MqttState::waiting:
		case MqttState::backoff:
			consider(attempt_at_);
			break;
		case MqttState::connecting:
			consider(connect_deadline_);
			break;
		case MqttState::connected:
			consider(sweep_end_);
			consider(history_due_);
			consider(retry_publish_at_);
			break;
		case MqttState::retiring:
			consider(retry_publish_at_);
			consider(retire_poll_at_);
			break;
		case MqttState::disabled:
			break;
	}
	loop_.cancel(timer_);
	timer_ = TimerHandle();
	if (at != EventLoop::kNever) {
		timer_ = loop_.schedule_at(at, Callback::member<Mqtt, &Mqtt::timer_due>(this));
	}
}

void Mqtt::timer_due() {
	timer_ = TimerHandle();
	uint64_t now = clock_.monotonic_ms();
	switch (state_) {
		case MqttState::waiting:
		case MqttState::backoff:
			if (attempt_at_ <= now) {
				attempt();
				return;
			}
			break;
		case MqttState::connecting:
			if (connect_deadline_ <= now) {
				client_.disconnect();
				fail("no answer from the broker within 30 s");
				return;
			}
			break;
		case MqttState::connected:
			if (sweep_end_ != 0 && sweep_end_ <= now) {
				end_sweep();
			}
			if (history_due_ != 0 && history_due_ <= now) {
				history_due_ = now + static_cast<uint64_t>(table_->config.interval_min) * 60000;
				if (!export_active_) {
					export_active_ = true;
					export_index_ = 0;
					export_to_ = 0;
				}
				request_pump();
			}
			if (retry_publish_at_ != 0 && retry_publish_at_ <= now) {
				retry_publish_at_ = 0;
				request_pump();
			}
			break;
		case MqttState::retiring:
			if ((retry_publish_at_ != 0 && retry_publish_at_ <= now) || (retire_poll_at_ != 0 && retire_poll_at_ <= now)) {
				retry_publish_at_ = 0;
				retire_poll_at_ = 0;
				request_pump();
			}
			break;
		case MqttState::disabled:
			break;
	}
	arm();
}

// ---- Publication -------------------------------------------------------------------------------

void Mqtt::request_pump() {
	if (pump_posted_) {
		return;
	}
	pump_posted_ = loop_.post(Callback::member<Mqtt, &Mqtt::pump_due>(this));
	if (!pump_posted_) {
		// The loop's queue is full: try with the timer.
		retry_publish_at_ = clock_.monotonic_ms() + kRetryPublishMs;
		arm();
	}
}

void Mqtt::pump_due() {
	pump_posted_ = false;
	pump();
}

bool Mqtt::send(const char* topic, const char* payload, size_t length, bool retain, uint8_t qos) {
	const char* error = nullptr;
	if (!client_.publish(topic, payload, length, retain, qos, &error)) {
		Log::debug(kTag, "publish %s deferred: %s", topic, error != nullptr ? error : "?");
		return false;
	}
	++published_;
	return true;
}

void Mqtt::pump() {
	if (state_ == MqttState::retiring) {
		pump_retire();
		return;
	}
	if (state_ != MqttState::connected) {
		return;
	}
	uint64_t now = clock_.monotonic_ms();
	const MemberConfig& c = table_->config;
	if (!online_sent_) {
		std::snprintf(topic_, sizeof(topic_), "%s/status", c.base);
		if (!send(topic_, "online", 6, true, 1)) {
			retry_publish_at_ = now + kRetryPublishMs;
			arm();
			return;
		}
		online_sent_ = true;
	}
	if (!subscribed_) {
		const char* error = nullptr;
		std::snprintf(topic_, sizeof(topic_), "%s/dp/+/set", c.base);
		bool ok = client_.subscribe(topic_, 1, &error);
		std::snprintf(topic_, sizeof(topic_), "%s/fn/+/set", c.base);
		ok = ok && client_.subscribe(topic_, 1, &error);
		if (!ok) {
			Log::warn(kTag, "cannot subscribe the command topics: %s; trying again", error != nullptr ? error : "?");
			retry_publish_at_ = now + kRetryPublishMs;
			arm();
			return;
		}
		subscribed_ = true;
		if (sweeping_) {
			sweep_again_ = true;
		} else {
			start_sweep();
		}
	}
	size_t budget = kPublishesPerTurn;
	Step step = pump_discovery(budget);
	if (step == Step::done) {
		step = pump_states(budget);
	}
	if (step == Step::done) {
		step = pump_history(budget);
	}
	if (step == Step::budget) {
		request_pump();
	} else if (step == Step::blocked) {
		retry_publish_at_ = now + kRetryPublishMs;
		arm();
	}
}

Mqtt::Step Mqtt::pump_discovery(size_t& budget) {
	if (!table_->config.discovery) {
		return Step::done;
	}
	while (discovery_next_ < table_->function_count) {
		if (budget == 0) {
			return Step::budget;
		}
		const Function& function = table_->functions[discovery_next_];
		if (!write_config(function)) {
			Log::error(kTag, "function '%s': the discovery config does not fit %u bytes, not announced", function.id,
				static_cast<unsigned>(kMaxPayload));
			++discovery_next_;
			continue;
		}
		if (!send(topic_, buffers_->payload, std::strlen(buffers_->payload), true, 1)) {
			return Step::blocked;
		}
		++discovery_next_;
		--budget;
	}
	return Step::done;
}

Mqtt::Step Mqtt::pump_states(size_t& budget) {
	size_t count = table_->point_count;
	size_t visited = 0;
	while (dirty_ > 0 && visited < count) {
		if (dirty_scan_ >= count) {
			dirty_scan_ = 0;
		}
		Point& point = table_->points[dirty_scan_];
		if (!point.dirty) {
			++dirty_scan_;
			++visited;
			continue;
		}
		const Datapoint* dp = model_.get(static_cast<uint16_t>(dirty_scan_));
		if (dp == nullptr || dp->value().empty()) {
			point.dirty = false;
			--dirty_;
			continue;
		}
		if (budget == 0) {
			return Step::budget;
		}
		if (!write_state(*dp)) {
			Log::error(kTag, "datapoint '%s': its state topic is longer than %u bytes, not published", dp->name(),
				static_cast<unsigned>(MqttClient::kMaxTopic));
			point.dirty = false;
			--dirty_;
			continue;
		}
		if (!send(topic_, buffers_->payload, std::strlen(buffers_->payload), table_->config.retain, 0)) {
			return Step::blocked;
		}
		point.dirty = false;
		--dirty_;
		--budget;
		++dirty_scan_;
		++visited;
	}
	return Step::done;
}

Mqtt::Step Mqtt::pump_history(size_t& budget) {
	if (!export_active_) {
		return Step::done;
	}
	if (history_ == nullptr || !table_->config.history) {
		export_active_ = false;
		return Step::done;
	}
	if (export_to_ == 0) {
		WallTime wall = clock_.wall_time();
		if (!wall.synchronized) {
			if (!clock_warned_) {
				Log::info(kTag, "history export waits for the wall clock (next attempt at the next interval)");
				clock_warned_ = true;
			}
			export_active_ = false;
			return Step::done;
		}
		clock_warned_ = false;
		export_to_ = wall.seconds; // exclusive: every sample before this second is in the rings
	}
	size_t queries = 0;
	while (export_index_ < table_->point_count) {
		Point& point = table_->points[export_index_];
		const Datapoint* dp = model_.get(static_cast<uint16_t>(export_index_));
		if (point.spacing_s == 0 || dp == nullptr || !dp->has_history()) {
			++export_index_;
			continue;
		}
		int64_t from = export_to_ - static_cast<int64_t>(point.retention_s);
		if (point.cursor + 1 > from) {
			from = point.cursor + 1;
		}
		if (from >= export_to_) {
			++export_index_;
			continue;
		}
		if (budget == 0 || queries >= kHistoryQueriesPerTurn) {
			return Step::budget;
		}
		// At most kHistoryBatch samples fit a window of kHistoryBatch spacings (the samples of a ring
		// are at least spacing_s apart): range() then returns every sample undecimated.
		int64_t to = from + static_cast<int64_t>(point.spacing_s) * static_cast<int64_t>(kHistoryBatch);
		if (to > export_to_) {
			to = export_to_;
		}
		++queries;
		size_t count = 0;
		HistoryStatus status = history_->range(dp->id(), from, to, buffers_->points, kHistoryBatch, count);
		if (status != HistoryStatus::ok) {
			Log::warn(kTag, "history of '%s' not exported: %s", dp->name(), history_status_name(status));
			++export_index_;
			continue;
		}
		if (count == 0) {
			point.cursor = to - 1;
			continue;
		}
		JsonWriter json(buffers_->payload, kMaxPayload);
		json.begin_array();
		for (size_t i = 0; i < count; ++i) {
			const HistoryPoint& p = buffers_->points[i];
			json.begin_array().number(p.last_t);
			switch (dp->type()) {
				case ValueType::boolean: json.boolean(p.last != 0); break;
				case ValueType::real: {
					char text[24];
					int n = std::snprintf(text, sizeof(text), "%.7g", p.last);
					json.raw(text, static_cast<size_t>(n));
					break;
				}
				default: json.number(static_cast<int64_t>(std::llround(p.last))); break;
			}
			json.end_array();
		}
		json.end_array();
		size_t length = 0;
		std::snprintf(topic_, sizeof(topic_), "%s/history/", table_->config.base);
		length = std::strlen(topic_);
		if (json.overflowed() || !append_encoded(topic_, sizeof(topic_), length, dp->name())) {
			Log::error(kTag, "history of '%s' not exported: topic or payload too long", dp->name());
			++export_index_;
			continue;
		}
		buffers_->payload[json.length() < kMaxPayload ? json.length() : kMaxPayload - 1] = '\0';
		if (!send(topic_, buffers_->payload, json.length(), false, 0)) {
			return Step::blocked;
		}
		point.cursor = buffers_->points[count - 1].last_t;
		++history_messages_;
		--budget;
	}
	export_active_ = false;
	return Step::done;
}

// ---- Payloads ----------------------------------------------------------------------------------

size_t Mqtt::point_topic(char* out, size_t size, const char* name, const char* leaf) const {
	int n = std::snprintf(out, size, "%s/dp/", table_->config.base);
	if (n < 0 || static_cast<size_t>(n) >= size) {
		return 0;
	}
	size_t length = static_cast<size_t>(n);
	if (!append_encoded(out, size, length, name)) {
		return 0;
	}
	n = std::snprintf(out + length, size - length, "/%s", leaf);
	if (n < 0 || static_cast<size_t>(n) >= size - length) {
		return 0;
	}
	return length + static_cast<size_t>(n);
}

size_t Mqtt::config_topic(char* out, size_t size, const char* component, const char* id) const {
	int n = std::snprintf(out, size, "%s/%s/%s/%s/config", table_->config.prefix, component, node_id_, id);
	return n < 0 || static_cast<size_t>(n) >= size ? 0 : static_cast<size_t>(n);
}

bool Mqtt::write_state(const Datapoint& datapoint) {
	if (point_topic(topic_, sizeof(topic_), datapoint.name(), "state") == 0) {
		return false;
	}
	JsonWriter json(buffers_->payload, kMaxPayload - 1);
	json.begin_object().key("value");
	write_value_json(json, datapoint.value());
	json.member("quality", quality_name(datapoint.quality()));
	WallTime wall = clock_.wall_time();
	uint64_t now = clock_.monotonic_ms();
	if (wall.synchronized && datapoint.updated_ms() != 0 && datapoint.updated_ms() <= now) {
		int64_t t = wall.seconds - static_cast<int64_t>((now - datapoint.updated_ms()) / 1000);
		json.member("time", t);
	}
	json.end_object();
	if (json.overflowed()) {
		return false;
	}
	buffers_->payload[json.length()] = '\0';
	return true;
}

const char* Mqtt::component_of(const Function& function) const {
	switch (function.kind) {
		case kLight:
		case kDimmer: return "light";
		case kBlind: return "cover";
		case kSwitch: return "switch";
		case kSensor: {
			const Datapoint* dp = model_.get(function.roles[kSensorValue]);
			return dp != nullptr && dp->type() == ValueType::boolean ? "binary_sensor" : "sensor";
		}
		case kClimate: return "climate";
		case kScene: return "scene";
		case kButton: {
			const Datapoint* dp = model_.get(function.roles[kButtonPress]);
			if (dp != nullptr && dp->writable()) {
				return "button";
			}
			// A remote's action (an enumeration press, 1.4) without a state: the last press's element.
			return dp != nullptr && dp->type() == ValueType::enumeration && !function.has(kButtonState) ? "sensor"
				: "binary_sensor";
		}
		default: return "sensor";
	}
}

namespace {

/// Writes the members of a config whose topics are relative to "~" (the base topic).
class ConfigWriter {
public:
	ConfigWriter(JsonWriter& json, const DatapointModel& model) : json_(json), model_(model) {}

	/// `key`: "~/dp/<name>/<leaf>" of the datapoint `id` (nothing if `id` is none).
	void topic(const char* key, uint16_t id, const char* leaf) {
		const Datapoint* dp = id == kNone ? nullptr : model_.get(id);
		if (dp == nullptr) {
			return;
		}
		char text[MqttClient::kMaxTopic + 1] = "~/dp/";
		size_t length = std::strlen(text);
		if (!append_encoded(text, sizeof(text), length, dp->name()) || length + std::strlen(leaf) + 2 > sizeof(text)) {
			failed_ = true;
			return;
		}
		text[length++] = '/';
		std::strcpy(text + length, leaf);
		json_.member(key, text);
	}

	bool failed() const { return failed_; }

private:
	JsonWriter& json_;
	const DatapointModel& model_;
	bool failed_ = false;
};

/// The template that makes a sensor's state payload into Home Assistant's state.
const char* sensor_template(ValueType type) {
	switch (type) {
		case ValueType::time_of_day: return kTimeTemplate;
		case ValueType::date: return kDateTemplate;
		case ValueType::date_time: return kDateTimeTemplate;
		case ValueType::controlled:
		case ValueType::color:
		case ValueType::raw: return kJsonTemplate;
		default: return kValueTemplate;
	}
}

} // namespace

bool Mqtt::write_config(const Function& function) {
	const char* component = component_of(function);
	if (config_topic(topic_, sizeof(topic_), component, function.id) == 0) {
		return false;
	}
	const MemberConfig& c = table_->config;
	JsonWriter json(buffers_->payload, kMaxPayload - 1);
	ConfigWriter w(json, model_);
	char unique[kMaxNodeId + kMaxFunctionId + 2];
	std::snprintf(unique, sizeof(unique), "%s_%s", node_id_, function.id);
	json.begin_object();
	json.member("~", c.base);
	json.member("name", function.label);
	json.member("unique_id", unique);
	json.member("availability_topic", "~/status");
	const uint16_t* r = function.roles;
	uint16_t primary = kNone; // the datapoint whose state payload carries the attributes
	switch (function.kind) {
		case kLight:
		case kSwitch:
			w.topic("command_topic", r[kLightCommand], "set");
			json.member("payload_on", "true").member("payload_off", "false");
			if (function.has(kLightState)) {
				w.topic("state_topic", r[kLightState], "state");
				json.member(function.kind == kLight ? "state_value_template" : "value_template", kBoolTemplate);
				primary = r[kLightState];
			}
			break;
		case kDimmer: {
			w.topic("brightness_command_topic", r[kDimLevelCommand], "set");
			json.member("brightness_scale", uint32_t(100));
			if (function.has(kDimLevel)) {
				w.topic("brightness_state_topic", r[kDimLevel], "state");
				json.member("brightness_value_template", kValueTemplate);
			}
			if (function.has(kDimCommand)) {
				w.topic("command_topic", r[kDimCommand], "set");
			} else {
				w.topic("command_topic", r[kDimLevelCommand], "set");
				json.member("on_command_type", "brightness");
			}
			json.member("payload_on", "true").member("payload_off", function.has(kDimCommand) ? "false" : "0");
			if (function.has(kDimState)) {
				w.topic("state_topic", r[kDimState], "state");
				json.member("state_value_template", kBoolTemplate);
				primary = r[kDimState];
			} else if (function.has(kDimLevel)) {
				w.topic("state_topic", r[kDimLevel], "state");
				json.member("state_value_template", kLevelOnTemplate);
				primary = r[kDimLevel];
			}
			break;
		}
		case kBlind: {
			char text[MqttClient::kMaxTopic + 1];
			std::snprintf(text, sizeof(text), "~/fn/%s/set", function.id);
			json.member("command_topic", text);
			json.member("payload_open", "OPEN").member("payload_close", "CLOSE");
			if (function.has(kBlindStop)) {
				json.member("payload_stop", "STOP");
			} else {
				json.key("payload_stop").null();
			}
			if (function.has(kBlindPosition)) {
				w.topic("position_topic", r[kBlindPosition], "state");
				json.member("position_template", kValueTemplate);
				primary = r[kBlindPosition];
			}
			if (function.has(kBlindPositionCommand)) {
				w.topic("set_position_topic", r[kBlindPositionCommand], "set");
			}
			if (function.has(kBlindPosition) || function.has(kBlindPositionCommand)) {
				json.member("position_open", uint32_t(0)).member("position_closed", uint32_t(100));
			}
			if (function.has(kBlindSlat)) {
				w.topic("tilt_status_topic", r[kBlindSlat], "state");
				json.member("tilt_status_template", kValueTemplate);
			}
			if (function.has(kBlindSlatCommand)) {
				w.topic("tilt_command_topic", r[kBlindSlatCommand], "set");
			}
			if (function.has(kBlindSlat) || function.has(kBlindSlatCommand)) {
				json.member("tilt_min", uint32_t(0)).member("tilt_max", uint32_t(100));
			}
			json.member("device_class", "blind");
			break;
		}
		case kSensor: {
			const Datapoint* dp = model_.get(r[kSensorValue]);
			ValueType type = dp != nullptr ? dp->type() : ValueType::none;
			w.topic("state_topic", r[kSensorValue], "state");
			primary = r[kSensorValue];
			if (type == ValueType::boolean) {
				json.member("value_template", kOnOffTemplate);
				break;
			}
			json.member("value_template", sensor_template(type));
			if (numeric(type)) {
				const char* device_class = nullptr;
				const char* state_class = nullptr;
				classes_of(function.unit, device_class, state_class);
				if (function.unit != nullptr) {
					json.member("unit_of_measurement", function.unit);
				}
				if (device_class != nullptr) {
					json.member("device_class", device_class);
				}
				json.member("state_class", state_class);
			}
			break;
		}
		case kClimate:
			w.topic("current_temperature_topic", r[kClimateTemperature], "state");
			json.member("current_temperature_template", kValueTemplate);
			primary = r[kClimateTemperature];
			if (function.has(kClimateSetpoint)) {
				w.topic("temperature_state_topic", r[kClimateSetpoint], "state");
				json.member("temperature_state_template", kValueTemplate);
			}
			if (function.has(kClimateSetpointCommand)) {
				w.topic("temperature_command_topic", r[kClimateSetpointCommand], "set");
			}
			if (function.has(kClimateHeating)) {
				w.topic("action_topic", r[kClimateHeating], "state");
				json.member("action_template", kHeatingTemplate);
			}
			json.key("modes").begin_array().string("heat").end_array();
			json.member("temperature_unit", "C").member("temp_step", 0.5);
			break;
		case kScene: {
			const Datapoint* dp = model_.get(r[0]);
			w.topic("command_topic", r[0], "set");
			char text[48];
			uint32_t number = function.scene > 0 ? function.scene - 1u : 0u;
			if (dp != nullptr && dp->type() == ValueType::controlled) {
				std::snprintf(text, sizeof(text), "{\"control\":false,\"value\":%u}", static_cast<unsigned>(number));
			} else {
				std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(number));
			}
			json.member("payload_on", text);
			break;
		}
		case kButton: {
			const Datapoint* press = model_.get(r[kButtonPress]);
			bool controlled = press != nullptr && press->type() == ValueType::controlled;
			if (press != nullptr && press->writable()) {
				w.topic("command_topic", r[kButtonPress], "set");
				json.member("payload_press", controlled ? "{\"control\":true,\"value\":1}" : "true");
				break;
			}
			uint16_t state = function.has(kButtonState) ? r[kButtonState] : r[kButtonPress];
			const Datapoint* dp = model_.get(state);
			w.topic("state_topic", state, "state");
			json.member("value_template", dp != nullptr && dp->type() == ValueType::enumeration ? kValueTemplate
				: dp != nullptr && dp->type() == ValueType::controlled ? kControlledOnOffTemplate : kOnOffTemplate);
			primary = state;
			break;
		}
		default: break;
	}
	if (primary != kNone) {
		w.topic("json_attributes_topic", primary, "state");
	}
	json.key("device").begin_object();
	json.key("identifiers").begin_array().string(node_id_).end_array();
	json.member("name", table_->device_name != nullptr ? table_->device_name : node_id_);
	json.member("manufacturer", "HomeControl").member("model", "HomeControl");
	if (sw_version_[0] != '\0') {
		json.member("sw_version", sw_version_);
	}
	json.end_object();
	json.end_object();
	if (json.overflowed() || w.failed()) {
		return false;
	}
	buffers_->payload[json.length()] = '\0';
	return true;
}

// ---- Inbound -----------------------------------------------------------------------------------

void Mqtt::datapoint_updated(const DatapointUpdate& update) {
	if (table_ == nullptr || !update.changed()) {
		return;
	}
	uint16_t id = update.datapoint.id();
	if (id >= table_->point_count) {
		return;
	}
	Point& point = table_->points[id];
	if (!point.dirty) {
		point.dirty = true;
		++dirty_;
	}
	if (state_ == MqttState::connected) {
		request_pump();
	}
}

const Mqtt::Function* Mqtt::find_function(const char* id) const {
	for (size_t i = 0; i < table_->function_count; ++i) {
		if (std::strcmp(table_->functions[i].id, id) == 0) {
			return &table_->functions[i];
		}
	}
	return nullptr;
}

void Mqtt::mqtt_message(const MqttMessage& message) {
	if (state_ != MqttState::connected || table_ == nullptr || message.topic == nullptr) {
		return;
	}
	const char* base = table_->config.base;
	size_t base_length = std::strlen(base);
	const char* topic = message.topic;
	size_t length = std::strlen(topic);
	if (std::strncmp(topic, base, base_length) == 0 && topic[base_length] == '/') {
		const char* rest = topic + base_length + 1;
		size_t rest_length = length - base_length - 1;
		bool dp = std::strncmp(rest, "dp/", 3) == 0;
		bool fn = std::strncmp(rest, "fn/", 3) == 0;
		if ((dp || fn) && rest_length > 7) {
			const char* level = rest + 3;
			const char* slash = std::strchr(level, '/');
			if (slash != nullptr && std::strchr(slash + 1, '/') == nullptr) {
				const char* leaf = slash + 1;
				char name[Datapoint::kMaxName + 1];
				bool named = decode_level(level, static_cast<size_t>(slash - level), name, sizeof(name));
				if (std::strcmp(leaf, "set") == 0) {
					if (!named) {
						Log::warn(kTag, "command on %s refused: the name is malformed or too long", topic);
						++refusals_;
						return;
					}
					if (dp) {
						handle_command(name, message);
					} else {
						handle_function_command(name, message);
					}
					return;
				}
				if (dp && std::strcmp(leaf, "state") == 0) {
					handle_sweep(message);
					return;
				}
			}
		}
	}
	const char* prefix = table_->config.prefix;
	size_t prefix_length = std::strlen(prefix);
	if (std::strncmp(topic, prefix, prefix_length) == 0 && topic[prefix_length] == '/') {
		handle_sweep(message);
	}
}

void Mqtt::publish_error(const char* level, const char* name, const char* reason, const MqttMessage& message) {
	++refusals_;
	char shown[49];
	size_t n = message.length < sizeof(shown) - 1 ? message.length : sizeof(shown) - 1;
	std::memcpy(shown, message.payload, n);
	shown[n] = '\0';
	Log::warn(kTag, "command on %s refused: %s", message.topic, reason);
	int written = std::snprintf(topic_, sizeof(topic_), "%s/%s/", table_->config.base, level);
	size_t length = written > 0 ? static_cast<size_t>(written) : 0;
	if (!append_encoded(topic_, sizeof(topic_), length, name) || length + 7 > sizeof(topic_)) {
		return;
	}
	std::strcpy(topic_ + length, "/error");
	JsonWriter json(buffers_->payload, kMaxPayload - 1);
	json.begin_object().member("error", reason).key("payload").string(shown, n).end_object();
	if (json.overflowed()) {
		return;
	}
	send(topic_, buffers_->payload, json.length(), false, 0);
}

void Mqtt::handle_command(const char* name, const MqttMessage& message) {
	if (message.retained) {
		publish_error("dp", name, "retained commands are ignored (they would repeat at every connect)", message);
		return;
	}
	const Datapoint* dp = model_.find(name);
	if (dp == nullptr) {
		publish_error("dp", name, "unknown datapoint", message);
		return;
	}
	if (message.truncated) {
		publish_error("dp", name, "the payload is too long", message);
		return;
	}
	if (!dp->writable()) {
		publish_error("dp", name, "the datapoint is not writable", message);
		return;
	}
	JsonSpan span;
	const char* reason = nullptr;
	if (!json_parse(message.payload, message.length, span, &reason)) {
		std::snprintf(error_, sizeof(error_), "the payload is not a JSON value: %s", reason);
		publish_error("dp", name, error_, message);
		return;
	}
	std::optional<Value> value = read_value_json(span, dp->type(), &reason);
	if (!value) {
		std::snprintf(error_, sizeof(error_), "not a %s value: %s", value_type_name(dp->type()), reason);
		publish_error("dp", name, error_, message);
		return;
	}
	uint16_t id = dp->id();
	DatapointStatus status = model_.command(id, *value);
	if (status != DatapointStatus::ok) {
		std::snprintf(error_, sizeof(error_), "refused: %s", datapoint_status_name(status));
		publish_error("dp", name, error_, message);
		return;
	}
	++commands_;
}

void Mqtt::handle_function_command(const char* id, const MqttMessage& message) {
	if (message.retained) {
		publish_error("fn", id, "retained commands are ignored (they would repeat at every connect)", message);
		return;
	}
	const Function* function = find_function(id);
	if (function == nullptr) {
		publish_error("fn", id, "unknown function", message);
		return;
	}
	if (function->kind != kBlind) {
		publish_error("fn", id, "only a blind takes commands on its function topic", message);
		return;
	}
	auto is = [&message](const char* word) {
		size_t n = std::strlen(word);
		return message.length == n && std::memcmp(message.payload, word, n) == 0;
	};
	uint16_t target = kNone;
	Value value;
	if (is("OPEN")) {
		target = function->roles[kBlindMove];
		value = Value::of_bool(false); // KNX DPT 1.008: 0 up
	} else if (is("CLOSE")) {
		target = function->roles[kBlindMove];
		value = Value::of_bool(true); // 1 down
	} else if (is("STOP")) {
		if (!function->has(kBlindStop)) {
			publish_error("fn", id, "the blind has no stop datapoint", message);
			return;
		}
		target = function->roles[kBlindStop];
		value = Value::of_bool(true); // DPT 1.007: a step stops a moving blind
	} else {
		publish_error("fn", id, "a blind takes OPEN, CLOSE or STOP", message);
		return;
	}
	DatapointStatus status = model_.command(target, value);
	if (status != DatapointStatus::ok) {
		std::snprintf(error_, sizeof(error_), "refused: %s", datapoint_status_name(status));
		publish_error("fn", id, error_, message);
		return;
	}
	++commands_;
}

void Mqtt::handle_sweep(const MqttMessage& message) {
	if (!sweeping_ || !message.retained || message.length == 0) {
		return;
	}
	const MemberConfig& c = table_->config;
	const char* topic = message.topic;
	size_t base_length = std::strlen(c.base);
	bool keep = true;
	if (std::strncmp(topic, c.base, base_length) == 0 && std::strncmp(topic + base_length, "/dp/", 4) == 0) {
		// <base>/dp/<name>/state of a datapoint the model no longer has.
		const char* level = topic + base_length + 4;
		const char* slash = std::strchr(level, '/');
		char name[Datapoint::kMaxName + 1];
		if (slash != nullptr && decode_level(level, static_cast<size_t>(slash - level), name, sizeof(name))) {
			keep = model_.find(name) != nullptr;
		}
	} else {
		// <prefix>/<component>/<node>/<id>/config of a function the image no longer has.
		size_t prefix_length = std::strlen(c.prefix);
		const char* component = topic + prefix_length + 1;
		const char* slash = std::strchr(component, '/');
		if (slash == nullptr) {
			return;
		}
		size_t component_length = static_cast<size_t>(slash - component);
		const char* node = slash + 1;
		size_t node_length = std::strlen(node_id_);
		if (std::strncmp(node, node_id_, node_length) != 0 || node[node_length] != '/') {
			return;
		}
		const char* id = node + node_length + 1;
		const char* end = std::strchr(id, '/');
		if (end == nullptr || std::strcmp(end, "/config") != 0) {
			return;
		}
		char fid[kMaxFunctionId + 1];
		size_t id_length = static_cast<size_t>(end - id);
		keep = false;
		if (c.discovery && id_length <= kMaxFunctionId) {
			std::memcpy(fid, id, id_length);
			fid[id_length] = '\0';
			const Function* function = find_function(fid);
			keep = function != nullptr && std::strlen(component_of(*function)) == component_length &&
				std::strncmp(component_of(*function), component, component_length) == 0;
		}
	}
	if (keep) {
		return;
	}
	char clear[MqttClient::kMaxTopic + 1];
	std::snprintf(clear, sizeof(clear), "%s", topic);
	if (send(clear, "", 0, true, 1)) {
		Log::info(kTag, "removed the retained %s (no longer in the image)", clear);
	} else {
		sweep_again_ = true;
	}
}

} // namespace hc
