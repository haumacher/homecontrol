/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/monitor_feed.h"

#include <cmath>
#include <cstring>

#include "hc/dpt.h"
#include "hc/image/image_service.h"
#include "hc/knx/group_value.h"
#include "hc/knx/tunnel.h"
#include "hc/log.h"
#include "hc/value_json.h"
#include "hc/web_api.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.monitor";

void copy_text(char* out, size_t size, const char* text) {
	size_t n = text != nullptr ? std::strlen(text) : 0;
	if (n >= size) {
		n = size - 1;
		// Never cut inside a UTF-8 sequence.
		while (n > 0 && (static_cast<uint8_t>(text[n]) & 0xC0) == 0x80) {
			--n;
		}
	}
	if (n > 0) {
		std::memcpy(out, text, n);
	}
	out[n] = '\0';
}

int hex_digit(char c) {
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

/// "0C 1A", "0c1a": octets, spaces between them allowed. False if malformed or longer than `size`.
bool parse_hex(const char* text, uint8_t* out, size_t size, size_t& length) {
	length = 0;
	const char* p = text;
	while (*p != '\0') {
		if (*p == ' ') {
			++p;
			continue;
		}
		int high = hex_digit(p[0]);
		int low = high >= 0 ? hex_digit(p[1]) : -1;
		if (low < 0 || length == size) {
			return false;
		}
		out[length++] = static_cast<uint8_t>(high * 16 + low);
		p += 2;
	}
	return true;
}

const char* sub_name(const MonitorEvent& event) {
	switch (event.kind) {
		case MonitorEvent::Kind::knx:
			return event.sub == static_cast<uint8_t>(MonitorEvent::KnxDirection::tx) ? "tx" : "rx";
		case MonitorEvent::Kind::zigbee:
			switch (static_cast<MonitorEvent::ZigbeeEvent>(event.sub)) {
				case MonitorEvent::ZigbeeEvent::action:
					return "action";
				case MonitorEvent::ZigbeeEvent::command:
					return "command";
				default:
					return "report";
			}
		case MonitorEvent::Kind::command:
			switch (static_cast<MonitorEvent::CommandOrigin>(event.sub)) {
				case MonitorEvent::CommandOrigin::ws:
					return "ws";
				case MonitorEvent::CommandOrigin::api:
					return "api";
				default:
					return "app";
			}
		case MonitorEvent::Kind::image:
			switch (static_cast<MonitorEvent::ImageEvent>(event.sub)) {
				case MonitorEvent::ImageEvent::load:
					return "load";
				case MonitorEvent::ImageEvent::unload:
					return "unload";
				default:
					return "reload";
			}
		default:
			return "";
	}
}

} // namespace

const char* MonitorFeed::kind_name(MonitorEvent::Kind kind) {
	switch (kind) {
		case MonitorEvent::Kind::knx:
			return "knx";
		case MonitorEvent::Kind::zigbee:
			return "zigbee";
		case MonitorEvent::Kind::rule:
			return "rule";
		case MonitorEvent::Kind::command:
			return "command";
		case MonitorEvent::Kind::image:
			return "image";
	}
	return "?";
}

MonitorFeed::MonitorFeed(const Clock& clock, DatapointModel& model) : clock_(clock), model_(model) {}

MonitorFeed::~MonitorFeed() {
	// Only the model, which outlives the feed by contract (as it outlives the web API). The sources
	// may be gone already (the POSIX build destroys the KNX objects and the image service before the
	// web server); none of them calls the feed while it is destroyed.
	if (listening_) {
		model_.remove_listener(*this);
	}
}

bool MonitorFeed::start() {
	if (listening_) {
		return true;
	}
	if (model_.add_listener(*this) != DatapointStatus::ok) {
		Log::error(kTag, "monitor stream without commands and decoded values: no listener slot in the datapoint model");
		return false;
	}
	listening_ = true;
	return true;
}

void MonitorFeed::attach_knx(knx::TelegramLog* log, knx::KnxTunnel* tunnel) {
	tunnel_ = tunnel;
	if (log == log_) {
		return;
	}
	if (log_ != nullptr && log_->listener() == this) {
		log_->set_listener(nullptr);
	}
	log_ = nullptr;
	if (log == nullptr) {
		return;
	}
	if (log->listener() != nullptr) {
		Log::warn(kTag, "the KNX telegram log has another listener: KNX telegrams are not monitored");
		return;
	}
	log->set_listener(this);
	log_ = log;
}

void MonitorFeed::attach_zigbee(zigbee::ZigbeeAdapter* adapter) {
	if (adapter == zigbee_) {
		return;
	}
	if (zigbee_ != nullptr && zigbee_->traffic_listener() == this) {
		zigbee_->set_traffic_listener(nullptr);
	}
	zigbee_ = nullptr;
	if (adapter == nullptr) {
		return;
	}
	if (adapter->traffic_listener() != nullptr) {
		Log::warn(kTag, "the Zigbee adapter has another traffic listener: Zigbee frames are not monitored");
		return;
	}
	adapter->set_traffic_listener(this);
	zigbee_ = adapter;
}

void MonitorFeed::attach_rules(rules::Engine* engine) {
	if (engine == engine_) {
		return;
	}
	if (engine_ != nullptr) {
		engine_->set_observer(nullptr);
	}
	engine_ = engine;
	if (engine_ != nullptr) {
		engine_->set_observer(this);
	}
}

void MonitorFeed::attach_image(image::ImageService& image) {
	knx::KnxAdapter& adapter = image.knx_adapter();
	attach_knx(adapter.telegram_log(), &adapter.tunnel());
	attach_rules(&image.engine());
	for (size_t i = 0; i < image.provider_count(); ++i) {
		image::DynamicSpecProvider* provider = image.provider(i);
		// The Zigbee adapter is the provider named "zigbee" (no RTTI in the core: the name is the type).
		if (provider != nullptr && std::strcmp(provider->provider_name(), "zigbee") == 0) {
			attach_zigbee(static_cast<zigbee::ZigbeeAdapter*>(provider));
		}
	}
}

MonitorEvent& MonitorFeed::append(MonitorEvent::Kind kind, uint8_t sub) {
	size_t slot;
	if (count_ == kCapacity) {
		slot = head_;
		head_ = (head_ + 1) % kCapacity;
	} else {
		slot = (head_ + count_) % kCapacity;
		++count_;
	}
	MonitorEvent& event = events_[slot];
	event = MonitorEvent();
	event.ms = clock_.monotonic_ms();
	WallTime wall = clock_.wall_time();
	if (wall.synchronized) {
		event.wall = wall.seconds;
		event.flags |= MonitorEvent::kWallTime;
	}
	event.kind = kind;
	event.sub = sub;
	event.generation = generation_;
	++total_;
	awaiting_value_ = false;
	if (appended_) {
		appended_();
	}
	return event;
}

MonitorEvent* MonitorFeed::newest() {
	if (count_ == 0) {
		return nullptr;
	}
	return &events_[(head_ + count_ - 1) % kCapacity];
}

const MonitorEvent* MonitorFeed::find(uint32_t sequence) const {
	if (sequence < first() || sequence >= total_) {
		return nullptr;
	}
	return &events_[(head_ + (sequence - first())) % kCapacity];
}

uint32_t MonitorFeed::start_of(uint32_t since, bool& reset) const {
	reset = since > total_;
	return reset ? first() : since;
}

void MonitorFeed::model_changed(const image::ImageService* image) {
	++generation_;
	awaiting_value_ = false;
	MonitorEvent::ImageEvent kind = MonitorEvent::ImageEvent::reload;
	bool loaded = image != nullptr && image->loaded();
	if (loaded && image->loaded_ms() != image_loaded_ms_) {
		kind = MonitorEvent::ImageEvent::load;
	} else if (!loaded && image_known_) {
		kind = MonitorEvent::ImageEvent::unload;
	}
	image_known_ = loaded;
	image_loaded_ms_ = loaded ? image->loaded_ms() : 0;
	MonitorEvent& event = append(MonitorEvent::Kind::image, static_cast<uint8_t>(kind));
	if (loaded) {
		copy_text(event.named.text, sizeof(event.named.text), image->info().project_id);
		event.named.revision = image->info().revision;
		event.named.source = static_cast<uint8_t>(image->source());
	}
}

void MonitorFeed::datapoint_updated(const DatapointUpdate& update) {
	const Datapoint& dp = update.datapoint;
	if (update.origin == UpdateOrigin::report) {
		MonitorEvent* event = newest();
		if (awaiting_value_ && event != nullptr && event->datapoint == dp.id() && event->generation == generation_) {
			event->value = dp.value();
			event->flags |= MonitorEvent::kHasValue;
			awaiting_value_ = false;
		}
		return;
	}
	if (update.origin != UpdateOrigin::command) {
		return;
	}
	MonitorEvent& event = append(MonitorEvent::Kind::command, static_cast<uint8_t>(origin_));
	if (origin_via_ == MonitorEvent::Via::token) {
		event.flags |= MonitorEvent::kViaToken;
	} else if (origin_via_ == MonitorEvent::Via::ehome) {
		event.flags |= MonitorEvent::kViaEhome;
	}
	event.datapoint = dp.id();
	event.value = dp.value();
	event.flags |= MonitorEvent::kHasValue;
}

void MonitorFeed::telegram_logged(const knx::TelegramRecord& record, uint64_t) {
	bool tx = record.direction == knx::TelegramRecord::Direction::sent;
	bool from_monitor = tx && take_pending(record);
	MonitorEvent& event = append(MonitorEvent::Kind::knx,
		static_cast<uint8_t>(tx ? MonitorEvent::KnxDirection::tx : MonitorEvent::KnxDirection::rx));
	event.ms = record.monotonic_ms;
	if (record.wall_synchronized) {
		event.wall = record.wall_seconds;
		event.flags |= MonitorEvent::kWallTime;
	} else {
		event.flags &= static_cast<uint8_t>(~MonitorEvent::kWallTime);
	}
	event.knx.source = record.source.raw;
	event.knx.destination = record.destination.raw;
	event.knx.service = static_cast<uint8_t>(record.service);
	event.knx.priority = static_cast<uint8_t>(record.priority);
	event.knx.hops = record.hop_count;
	event.knx.length = record.length <= sizeof(event.knx.data) ? record.length : sizeof(event.knx.data);
	std::memcpy(event.knx.data, record.data, event.knx.length);
	if (record.repeated) {
		event.flags |= MonitorEvent::kRepeated;
	}
	if (record.short_form) {
		event.flags |= MonitorEvent::kShortForm;
	}
	if (from_monitor) {
		event.flags |= MonitorEvent::kFromMonitor;
	}
	event.datapoint = record.datapoint;
	bool carries_value = record.service == knx::GroupService::write || record.service == knx::GroupService::response;
	if (record.datapoint == knx::TelegramRecord::kNoDatapoint || !carries_value) {
		return;
	}
	// The adapter reports the decoded value right after logging (the other listening datapoints of a
	// sent telegram too); a sent telegram's sender holds the value already.
	if (tx) {
		const Datapoint* dp = model_.get(record.datapoint);
		if (dp != nullptr && dp->quality() != Quality::unknown) {
			event.value = dp->value();
			event.flags |= MonitorEvent::kHasValue;
		}
	}
	awaiting_value_ = true;
}

void MonitorFeed::zigbee_traffic(const zigbee::ZigbeeTraffic& traffic) {
	MonitorEvent::ZigbeeEvent sub = MonitorEvent::ZigbeeEvent::report;
	if (traffic.kind == zigbee::ZigbeeTraffic::Kind::action) {
		sub = MonitorEvent::ZigbeeEvent::action;
	} else if (traffic.kind == zigbee::ZigbeeTraffic::Kind::command) {
		sub = MonitorEvent::ZigbeeEvent::command;
	}
	MonitorEvent& event = append(MonitorEvent::Kind::zigbee, static_cast<uint8_t>(sub));
	event.zigbee.ieee = traffic.ieee;
	event.zigbee.short_addr = traffic.short_addr;
	event.zigbee.endpoint = traffic.endpoint;
	event.zigbee.cluster = traffic.cluster;
	event.zigbee.code = traffic.code;
	event.datapoint = traffic.datapoint;
	if (traffic.value != nullptr) {
		event.value = *traffic.value;
		event.flags |= MonitorEvent::kHasValue;
	}
}

void MonitorFeed::rule_commanded(const char* rule, const Datapoint& datapoint, const Value& value) {
	// The model told the command first (origin app, no caller declared one): that event becomes the
	// rule's. A command the rule made without that event (none is lost: defensive) gets its own.
	MonitorEvent* event = newest();
	if (event == nullptr || event->kind != MonitorEvent::Kind::command ||
		event->sub != static_cast<uint8_t>(MonitorEvent::CommandOrigin::app) || event->datapoint != datapoint.id() ||
		event->generation != generation_) {
		event = &append(MonitorEvent::Kind::rule, 0);
		event->datapoint = datapoint.id();
		event->value = value;
		event->flags |= MonitorEvent::kHasValue;
	}
	event->kind = MonitorEvent::Kind::rule;
	event->sub = static_cast<uint8_t>(MonitorEvent::RuleAction::command);
	event->flags &= static_cast<uint8_t>(~(MonitorEvent::kViaToken | MonitorEvent::kViaEhome));
	copy_text(event->named.text, sizeof(event->named.text), rule);
}

void MonitorFeed::rule_read(const char* rule, const Datapoint& datapoint) {
	// A rule's read action (spec/rules.md 7.12): the technology's telegram follows as its own event;
	// this one says which rule asked.
	MonitorEvent& event = append(MonitorEvent::Kind::rule, static_cast<uint8_t>(MonitorEvent::RuleAction::read));
	event.datapoint = datapoint.id();
	copy_text(event.named.text, sizeof(event.named.text), rule);
}

// ---- JSON -------------------------------------------------------------------------------------

void MonitorFeed::write_event(JsonWriter& json, const MonitorEvent& event, uint32_t sequence) const {
	json.begin_object().member("seq", sequence).member("ms", event.ms);
	if ((event.flags & MonitorEvent::kWallTime) != 0) {
		json.member("t", event.wall);
	}
	json.member("kind", kind_name(event.kind));
	switch (event.kind) {
		case MonitorEvent::Kind::knx: {
			char source[knx::IndividualAddress::kMaxText];
			char destination[knx::GroupAddress::kMaxText];
			char hex[knx::GroupData::kMaxData * 3 + 1];
			json.member("dir", sub_name(event))
				.member("src", knx::IndividualAddress{event.knx.source}.format(source, sizeof(source)))
				.member("dst", knx::GroupAddress{event.knx.destination}.format(destination, sizeof(destination)))
				.member("service", knx::group_service_name(static_cast<knx::GroupService>(event.knx.service)))
				.member("data", knx::format_hex(event.knx.data, event.knx.length, hex, sizeof(hex)))
				.member("prio", static_cast<uint32_t>(event.knx.priority))
				.member("hops", static_cast<uint32_t>(event.knx.hops));
			if ((event.flags & MonitorEvent::kShortForm) != 0) {
				json.member("short", true);
			}
			if ((event.flags & MonitorEvent::kRepeated) != 0) {
				json.member("repeated", true);
			}
			if ((event.flags & MonitorEvent::kFromMonitor) != 0) {
				json.member("origin", "monitor");
			}
			break;
		}
		case MonitorEvent::Kind::zigbee: {
			char ieee[zigbee::kIeeeText];
			zigbee::format_ieee(event.zigbee.ieee, ieee);
			json.member("event", sub_name(event)).member("ieee", ieee);
			if (event.zigbee.short_addr != zigbee::kNoShortAddress) {
				json.member("short", static_cast<uint32_t>(event.zigbee.short_addr));
			}
			json.member("ep", static_cast<uint32_t>(event.zigbee.endpoint))
				.member("cluster", static_cast<uint32_t>(event.zigbee.cluster))
				.member(event.sub == static_cast<uint8_t>(MonitorEvent::ZigbeeEvent::report) ? "attr" : "cmd",
					static_cast<uint32_t>(event.zigbee.code));
			break;
		}
		case MonitorEvent::Kind::rule:
			json.member("rule", event.named.text);
			if (event.sub == static_cast<uint8_t>(MonitorEvent::RuleAction::read)) {
				json.member("action", "read");
			}
			break;
		case MonitorEvent::Kind::command:
			json.member("origin", sub_name(event));
			if (event.sub != static_cast<uint8_t>(MonitorEvent::CommandOrigin::app)) {
				json.member("via", (event.flags & MonitorEvent::kViaEhome) != 0 ? "ehome"
					: (event.flags & MonitorEvent::kViaToken) != 0 ? "token" : "session");
			}
			break;
		case MonitorEvent::Kind::image:
			json.member("event", sub_name(event));
			if (event.named.text[0] != '\0') {
				json.member("project", event.named.text)
					.member("revision", event.named.revision)
					.member("source", image::source_name(static_cast<image::Source>(event.named.source)));
			}
			break;
	}
	if (event.datapoint != MonitorEvent::kNoDatapoint && event.generation == generation_) {
		const Datapoint* dp = model_.get(event.datapoint);
		if (dp != nullptr) {
			json.member("dp", static_cast<uint32_t>(event.datapoint)).member("name", dp->name());
		}
	}
	if ((event.flags & MonitorEvent::kHasValue) != 0) {
		json.key("value");
		write_value_json(json, event.value);
	}
	json.end_object();
}

uint32_t MonitorFeed::write_events(JsonWriter& json, uint32_t cursor, size_t limit) const {
	uint32_t oldest = first();
	if (cursor < oldest) {
		JsonWriter::Mark mark = json.mark();
		json.begin_object().member("kind", "gap").member("seq", cursor).member("count", oldest - cursor).end_object();
		if (json.overflowed()) {
			json.reset(mark);
			return cursor;
		}
		cursor = oldest;
	}
	for (size_t n = 0; n < limit && cursor < total_; ++n) {
		JsonWriter::Mark mark = json.mark();
		write_event(json, *find(cursor), cursor);
		if (json.overflowed()) {
			json.reset(mark);
			break;
		}
		++cursor;
	}
	return cursor;
}

void handle_monitor_request(const MonitorFeed* feed, const WebRequest& request, WebResponse& response) {
	if (request.method != HttpMethod::get) {
		WebApi::refuse(response, 405, "api.method_not_allowed", "use GET");
		return;
	}
	if (feed == nullptr) {
		WebApi::refuse(response, 404, "monitor.unavailable", "this server has no monitor stream");
		return;
	}
	uint32_t since = 0;
	uint32_t limit = 50;
	bool has_since = query_uint(request.query, "since", since);
	bool has_limit = query_uint(request.query, "limit", limit);
	if ((!has_since && std::strstr(request.query, "since=") != nullptr) ||
		(!has_limit && std::strstr(request.query, "limit=") != nullptr) || limit == 0 || limit > MonitorFeed::kCapacity) {
		WebApi::refuse(response, 400, "monitor.bad_query",
			"since is a sequence number, limit a number of events from 1 to 128");
		return;
	}
	bool reset = false;
	uint32_t start = has_since ? feed->start_of(since, reset) : feed->first(); // without since: all held
	JsonWriter json(response.body, response.capacity);
	json.reserve(MonitorFeed::kClosingReserve);
	json.begin_object().member("first", feed->first()).member("next", feed->next()).member("since", start);
	if (reset) {
		json.member("reset", true);
	}
	json.key("events").begin_array();
	uint32_t after = feed->write_events(json, start, limit);
	json.release();
	json.end_array().member("next_since", after).member("more", after < feed->next()).end_object();
	if (json.overflowed()) {
		WebApi::refuse(response, 500, "api.response_too_large", "the response does not fit the server's buffer");
		return;
	}
	response.length = json.length();
}

// ---- Sending ----------------------------------------------------------------------------------

bool MonitorFeed::knx_target(const JsonSpan& spec, JsonSpan& knx, const char** code, const char** reason) {
	JsonSpan other;
	if (spec.kind == JsonSpan::Kind::object && json_member(spec, "knx", knx) && knx.kind == JsonSpan::Kind::object) {
		return true;
	}
	if (spec.kind == JsonSpan::Kind::object && json_member(spec, "zigbee", other)) {
		*code = "monitor.unsupported";
		*reason = "the monitor sends only to KNX: command the Zigbee datapoint instead";
		return false;
	}
	*code = "monitor.bad_request";
	*reason = "expected {\"knx\": {\"address\": \"1/2/3\", ...}}";
	return false;
}

bool MonitorFeed::tunnel_ready(const char** code, const char** reason) const {
	if (tunnel_ == nullptr) {
		*code = "monitor.knx_unavailable";
		*reason = "this controller has no KNX tunnel";
		return false;
	}
	if (!tunnel_->is_up()) {
		*code = "monitor.tunnel_down";
		*reason = "the KNX tunnel is not up";
		return false;
	}
	return true;
}

namespace {

bool read_address(const JsonSpan& knx, knx::GroupAddress& address, const char** code, const char** reason) {
	JsonSpan span;
	char text[16];
	if (!json_member(knx, "address", span) || !json_string(span, text, sizeof(text))) {
		*code = "monitor.bad_address";
		*reason = "\"address\" must be a group address such as \"1/2/3\"";
		return false;
	}
	const char* why = nullptr;
	std::optional<knx::GroupAddress> parsed = knx::GroupAddress::parse(text, &why);
	if (!parsed) {
		*code = "monitor.bad_address";
		*reason = why != nullptr ? why : "not a group address";
		return false;
	}
	address = *parsed;
	return true;
}

} // namespace

bool MonitorFeed::send(const JsonSpan& spec, const char** code, const char** reason) {
	JsonSpan knx_spec;
	knx::GroupAddress address;
	if (!knx_target(spec, knx_spec, code, reason) || !read_address(knx_spec, address, code, reason)) {
		return false;
	}
	knx::GroupService service = knx::GroupService::write;
	JsonSpan span;
	if (json_member(knx_spec, "service", span)) {
		if (json_string_equals(span, "response")) {
			service = knx::GroupService::response;
		} else if (!json_string_equals(span, "write")) {
			*code = "monitor.bad_request";
			*reason = "\"service\" is \"write\" or \"response\" (a read is {\"read\": ...})";
			return false;
		}
	}
	knx::LData frame;
	JsonSpan dpt_span;
	JsonSpan data_span;
	if (json_member(knx_spec, "dpt", dpt_span)) {
		char text[Dpt::kMaxText + 4];
		std::optional<Dpt> dpt;
		if (json_string(dpt_span, text, sizeof(text))) {
			dpt = Dpt::parse(text);
		}
		if (!dpt) {
			*code = "monitor.bad_dpt";
			*reason = "\"dpt\" must be a datapoint type such as \"1.001\" or \"9.001\"";
			return false;
		}
		if (!dpt->supported()) {
			*code = "monitor.bad_dpt";
			*reason = dpt->unsupported_reason();
			return false;
		}
		JsonSpan value_span;
		if (!json_member(knx_spec, "value", value_span)) {
			*code = "monitor.value_missing";
			*reason = "a typed send needs \"value\"";
			return false;
		}
		const char* why = nullptr;
		std::optional<Value> value = read_value_json(value_span, dpt->value_type(), &why);
		if (!value) {
			*code = "monitor.bad_value";
			*reason = why != nullptr ? why : "the value does not fit the datapoint type";
			return false;
		}
		if (!tunnel_ready(code, reason)) {
			return false;
		}
		if (!knx::make_group_value(frame, address, service, *dpt, *value, &why)) {
			*code = "monitor.bad_value";
			*reason = why != nullptr ? why : "the value cannot be encoded";
			return false;
		}
	} else if (json_member(knx_spec, "data", data_span)) {
		char text[knx::GroupData::kMaxData * 3 + 1];
		uint8_t data[knx::GroupData::kMaxData];
		size_t length = 0;
		bool short_form = false;
		JsonSpan short_span;
		if (json_member(knx_spec, "short", short_span) && !json_bool(short_span, short_form)) {
			short_form = false;
		}
		if (!json_string(data_span, text, sizeof(text)) || !parse_hex(text, data, sizeof(data), length) ||
			(short_form && (length != 1 || data[0] > 0x3F))) {
			*code = "monitor.bad_value";
			*reason = "\"data\" must be 1 to 14 hex octets (\"short\": one octet up to 3F)";
			return false;
		}
		if (length == 0) {
			*code = "monitor.bad_value";
			*reason = "a write or response carries data";
			return false;
		}
		if (!tunnel_ready(code, reason)) {
			return false;
		}
		if (!knx::make_group_request(frame, address, service, data, length, short_form)) {
			*code = "monitor.bad_value";
			*reason = "the data does not form a group telegram";
			return false;
		}
	} else {
		*code = "monitor.value_missing";
		*reason = "a send needs \"dpt\" and \"value\", or \"data\"";
		return false;
	}
	uint32_t token = tunnel_->send(frame);
	char text[knx::GroupAddress::kMaxText];
	address.format(text, sizeof(text));
	if (token == 0) {
		*code = tunnel_->is_up() ? "monitor.queue_full" : "monitor.tunnel_down";
		*reason = tunnel_->is_up() ? "the tunnel's send queue is full: try again" : "the KNX tunnel is not up";
		Log::warn(kTag, "monitor: knx %s %s refused: %s", knx::group_service_name(service), text, *reason);
		return false;
	}
	knx::GroupData data;
	if (knx::group_data(frame, data)) {
		remember_send(address.raw, static_cast<uint8_t>(service), data.data, data.length, data.short_form);
	}
	Log::info(kTag, "monitor: knx %s %s sent", knx::group_service_name(service), text);
	return true;
}

bool MonitorFeed::read(const JsonSpan& spec, const char** code, const char** reason) {
	JsonSpan knx_spec;
	knx::GroupAddress address;
	if (!knx_target(spec, knx_spec, code, reason) || !read_address(knx_spec, address, code, reason) ||
		!tunnel_ready(code, reason)) {
		return false;
	}
	knx::LData frame;
	knx::make_group_read(frame, address);
	uint32_t token = tunnel_->send(frame);
	char text[knx::GroupAddress::kMaxText];
	address.format(text, sizeof(text));
	if (token == 0) {
		*code = "monitor.queue_full";
		*reason = "the tunnel's send queue is full: try again";
		Log::warn(kTag, "monitor: knx read %s refused: %s", text, *reason);
		return false;
	}
	remember_send(address.raw, static_cast<uint8_t>(knx::GroupService::read), nullptr, 0, false);
	Log::info(kTag, "monitor: knx read %s sent", text);
	return true;
}

void MonitorFeed::remember_send(uint16_t destination, uint8_t service, const uint8_t* data, uint8_t length,
	bool short_form) {
	uint64_t now = clock_.monotonic_ms();
	PendingSend* slot = &pending_[0];
	for (PendingSend& p : pending_) {
		if (!p.used || p.until_ms <= now) {
			slot = &p;
			break;
		}
		if (p.until_ms < slot->until_ms) {
			slot = &p; // all in use: replace the oldest
		}
	}
	*slot = PendingSend{};
	slot->used = true;
	slot->destination = destination;
	slot->service = service;
	slot->length = length <= sizeof(slot->data) ? length : sizeof(slot->data);
	slot->short_form = short_form;
	if (slot->length > 0) {
		std::memcpy(slot->data, data, slot->length);
	}
	slot->until_ms = now + kPendingSendMs;
}

bool MonitorFeed::take_pending(const knx::TelegramRecord& record) {
	uint64_t now = clock_.monotonic_ms();
	for (PendingSend& p : pending_) {
		if (!p.used || p.until_ms <= now) {
			p.used = false;
			continue;
		}
		if (p.destination == record.destination.raw && p.service == static_cast<uint8_t>(record.service) &&
			p.length == record.length && p.short_form == record.short_form &&
			std::memcmp(p.data, record.data, p.length) == 0) {
			p.used = false;
			return true;
		}
	}
	return false;
}

} // namespace hc
