/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/live_feed.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "hc/image/image_service.h"
#include "hc/log.h"
#include "hc/value_json.h"
#include "hc/version.h"
#include "hc/websocket.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.ws";
constexpr size_t kNoSnapshot = static_cast<size_t>(-1);

/// Room kept for the closing of a chunked message.
constexpr size_t kClosingReserve = 48;

char lower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// The host part of "host[:port]" or "[v6]:port": start and length.
void host_part(const char* text, const char*& start, size_t& length) {
	start = text;
	if (*text == '[') {
		const char* close = std::strchr(text, ']');
		length = close != nullptr ? static_cast<size_t>(close - text + 1) : std::strlen(text);
		return;
	}
	length = 0;
	while (text[length] != '\0' && text[length] != ':' && text[length] != '/') {
		++length;
	}
}

uint32_t fnv(const char* text) {
	uint32_t h = 2166136261u;
	for (; *text != '\0'; ++text) {
		h = (h ^ static_cast<uint8_t>(*text)) * 16777619u;
	}
	return h;
}

} // namespace

LiveFeed::LiveFeed(WebApi& api, EventLoop& loop, WsTransport& transport)
	: api_(api), loop_(loop), transport_(transport), monitor_(api.controller().clock(), api.model()) {}

bool LiveFeed::start() {
	if (api_.model().add_listener(*this) != DatapointStatus::ok) {
		Log::error(kTag, "live feed not started: no listener slot in the datapoint model");
		return false;
	}
	uint64_t now = api_.controller().clock().monotonic_ms();
	if (!loop_.schedule_periodic(now + kTickMs, kTickMs, Callback::member<LiveFeed, &LiveFeed::tick>(this)).valid()) {
		Log::error(kTag, "live feed: no timer for the status push (heartbeat missing)");
	}
	api_.set_feed(this);
	monitor_.start();
	monitor_.set_appended(Callback::member<LiveFeed, &LiveFeed::schedule_monitor_flush>(this));
	attach_monitor();
	if (api_.image() != nullptr && api_.image()->loaded()) {
		monitor_.model_changed(api_.image()); // the image loaded before the feed started
	}
	return true;
}

void LiveFeed::attach_monitor() {
	if (api_.image() != nullptr) {
		monitor_.attach_image(*api_.image());
	}
}

bool LiveFeed::same_origin(const char* origin, const char* host) {
	const char* scheme_end = std::strstr(origin, "://");
	if (scheme_end == nullptr) {
		return false;
	}
	const char* a;
	size_t a_length;
	const char* b;
	size_t b_length;
	host_part(scheme_end + 3, a, a_length);
	host_part(host, b, b_length);
	if (a_length != b_length || a_length == 0) {
		return false;
	}
	for (size_t i = 0; i < a_length; ++i) {
		if (lower(a[i]) != lower(b[i])) {
			return false;
		}
	}
	return true;
}

size_t LiveFeed::client_count() const {
	size_t n = 0;
	for (size_t i = 0; i < kMaxClients; ++i) {
		n += clients_[i].used ? 1 : 0;
	}
	return n;
}

LiveFeed::Client* LiveFeed::find(uint32_t handle) {
	for (size_t i = 0; i < kMaxClients; ++i) {
		if (clients_[i].used && clients_[i].handle == handle) {
			return &clients_[i];
		}
	}
	return nullptr;
}

void LiveFeed::attach_ehome(WsTransport& transport, uint32_t handle) {
	Client& slot = clients_[kEhomeSlot];
	slot = Client{};
	slot.used = true;
	slot.handle = handle;
	slot.transport = &transport;
	slot.principal = Principal::ehome;
	slot.snapshot_next = kNoSnapshot;
	slot.opened_ms = api_.controller().clock().monotonic_ms();
	Log::info(kTag, "E-Home joined the live feed");
	JsonWriter json(out_, sizeof(out_));
	json.begin_object()
		.member("type", "hello")
		.member("authenticated", true)
		.member("onboarded", api_.auth().has_password())
		.member("device", api_.host().device_name())
		.member("version", kVersion)
		.end_object();
	send(slot, out_, json.length());
}

void LiveFeed::ehome_message(const char* text, size_t length) {
	if (clients_[kEhomeSlot].used) {
		handle_message(clients_[kEhomeSlot], text, length);
	}
}

void LiveFeed::detach_ehome() {
	if (clients_[kEhomeSlot].used) {
		clients_[kEhomeSlot] = Client{};
		Log::info(kTag, "E-Home left the live feed");
	}
}

bool LiveFeed::open(uint32_t handle, const char* cookie, const char* origin, const char* host) {
	const char* refusal_code = nullptr;
	const char* refusal_text = nullptr;
	uint16_t ws_code = websocket::kClosePolicy;
	if (origin != nullptr && host != nullptr && !same_origin(origin, host)) {
		refusal_code = "ws.foreign_origin";
		refusal_text = "WebSocket from a foreign page refused";
	}
	Client* slot = nullptr;
	if (refusal_code == nullptr) {
		for (size_t i = 0; i < kMaxClients; ++i) {
			if (!clients_[i].used) {
				slot = &clients_[i];
				break;
			}
		}
		if (slot == nullptr) {
			refusal_code = "ws.too_many_clients";
			refusal_text = "at most 4 live connections: close another browser tab or app";
			ws_code = websocket::kCloseTryAgainLater;
		}
	}
	if (refusal_code != nullptr) {
		Log::warn(kTag, "WebSocket client refused: %s", refusal_text);
		JsonWriter json(out_, sizeof(out_));
		json.begin_object().member("type", "error").member("error", refusal_text).member("code", refusal_code).end_object();
		transport_.ws_send(handle, out_, json.length());
		transport_.ws_close(handle, ws_code, refusal_text);
		return false;
	}
	*slot = Client{};
	slot->used = true;
	slot->handle = handle;
	slot->transport = &transport_;
	slot->snapshot_next = kNoSnapshot;
	slot->opened_ms = api_.controller().clock().monotonic_ms();
	Auth& auth = api_.auth();
	if (auth.has_password()) {
		slot->session = auth.session_from_cookie(cookie);
		if (slot->session.valid()) {
			slot->principal = Principal::session;
		}
	}
	Log::info(kTag, "WebSocket client %u connected (%s, %u of %u)", static_cast<unsigned>(handle),
		authenticated(*slot) ? "session" : "awaiting a token", static_cast<unsigned>(client_count()),
		static_cast<unsigned>(kMaxClients));
	JsonWriter json(out_, sizeof(out_));
	json.begin_object()
		.member("type", "hello")
		.member("authenticated", authenticated(*slot))
		.member("onboarded", auth.has_password())
		.member("device", api_.host().device_name())
		.member("version", kVersion)
		.end_object();
	send(*slot, out_, json.length());
	return true;
}

void LiveFeed::closed(uint32_t handle) {
	Client* c = find(handle);
	if (c == nullptr) {
		return;
	}
	*c = Client{};
	Log::info(kTag, "WebSocket client %u disconnected (%u left)", static_cast<unsigned>(handle),
		static_cast<unsigned>(client_count()));
}

void LiveFeed::send(Client& client, const char* text, size_t length) {
	if (!client.transport->ws_send(client.handle, text, length) && client.subscribed) {
		if (client.snapshot_next != 0) {
			Log::warn(kTag, "WebSocket client %u: message dropped (send buffers full): full resync follows",
				static_cast<unsigned>(client.handle));
		}
		client.snapshot_next = 0;
	}
}

void LiveFeed::send_error(Client& client, const char* code, const char* text) {
	JsonWriter json(out_, sizeof(out_));
	json.begin_object().member("type", "error").member("error", text).member("code", code).end_object();
	send(client, out_, json.length());
}

void LiveFeed::refuse_and_close(Client& client, uint16_t ws_code, const char* code, const char* text) {
	Log::warn(kTag, "WebSocket client %u closed: %s", static_cast<unsigned>(client.handle), text);
	send_error(client, code, text);
	uint32_t handle = client.handle;
	WsTransport* transport = client.transport;
	client = Client{};
	transport->ws_close(handle, ws_code, text);
}

void LiveFeed::message(uint32_t handle, const char* text, size_t length) {
	Client* c = find(handle);
	if (c != nullptr) {
		handle_message(*c, text, length);
	}
}

void LiveFeed::handle_message(Client& client, const char* text, size_t length) {
	Client* c = &client;
	JsonSpan body;
	JsonSpan type;
	const char* error = nullptr;
	if (!json_parse(text, length, body, &error) || body.kind != JsonSpan::Kind::object ||
		!json_member(body, "type", type) || type.kind != JsonSpan::Kind::string) {
		send_error(*c, "ws.bad_message", "a message is a JSON object with a \"type\"");
		return;
	}
	if (json_string_equals(type, "auth")) {
		if (c->principal == Principal::ehome) {
			send_error(*c, "ws.bad_message", "the E-Home connection is authenticated already");
			return;
		}
		authenticate_token(*c, body);
		return;
	}
	if (!authenticated(*c)) {
		refuse_and_close(*c, websocket::kClosePolicy, "auth.required",
			"authenticate first: log in, or send {\"type\":\"auth\",\"token\":\"...\"}");
		return;
	}
	if (json_string_equals(type, "ping")) {
		// Application-level keep-alive: the UI pings every 20 s, which keeps an idle feed off the
		// ESP server's least-recently-used purge and detects a dead connection in the browser.
		static const char kPong[] = "{\"type\":\"pong\"}";
		send(*c, kPong, sizeof(kPong) - 1);
	} else if (json_string_equals(type, "subscribe")) {
		subscribe(*c);
	} else if (json_string_equals(type, "command")) {
		command(*c, body);
	} else if (json_string_equals(type, "monitor")) {
		monitor_message(*c, body);
	} else {
		send_error(*c, "ws.unknown_type", "unknown message type");
	}
}

void LiveFeed::authenticate_token(Client& client, const JsonSpan& message) {
	JsonSpan token;
	char header[8 + Auth::kTokenText + 8] = "Bearer ";
	Auth& auth = api_.auth();
	int slot = -1;
	if (json_member(message, "token", token) && json_string(token, header + 7, sizeof(header) - 7)) {
		slot = auth.token_from_authorization(header);
	}
	if (!auth.has_password()) {
		refuse_and_close(client, websocket::kClosePolicy, auth_status_code(AuthStatus::no_password),
			auth_status_text(AuthStatus::no_password));
		return;
	}
	if (slot < 0) {
		refuse_and_close(client, websocket::kClosePolicy, "auth.token_invalid", "the token is unknown or revoked");
		return;
	}
	client.principal = Principal::token;
	client.token_slot = slot;
	std::memcpy(client.token_hash, auth.token_hash(slot), sizeof(client.token_hash));
	JsonWriter json(out_, sizeof(out_));
	json.begin_object().member("type", "auth").member("ok", true).end_object();
	send(client, out_, json.length());
}

void LiveFeed::command(Client& client, const JsonSpan& message) {
	JsonSpan id_span;
	JsonSpan value_span;
	JsonSpan ref;
	bool has_ref = json_member(message, "ref", ref) && ref.kind == JsonSpan::Kind::number;
	double id_number = -1;
	DatapointStatus status = DatapointStatus::ok;
	const char* reason = nullptr;
	const char* code = nullptr;
	DatapointModel& model = api_.model();
	const Datapoint* dp = nullptr;
	if (!json_member(message, "id", id_span) || !json_number(id_span, id_number) || id_number < 0 ||
		id_number > 65535 || id_number != std::floor(id_number) ||
		(dp = model.get(static_cast<uint16_t>(id_number))) == nullptr) {
		code = "dp.unknown_id";
		reason = "no datapoint with this id";
	} else if (!json_member(message, "value", value_span)) {
		code = "dp.value_missing";
		reason = "the command needs a \"value\"";
	} else if (!dp->writable()) {
		code = "dp.not_writable";
		reason = "the datapoint is not writable";
	} else {
		std::optional<Value> value = read_value_json(value_span, dp->type(), &reason);
		if (!value) {
			code = "dp.wrong_type";
		} else {
			MonitorOrigin origin(&monitor_, MonitorEvent::CommandOrigin::ws,
				client.principal == Principal::ehome ? MonitorEvent::Via::ehome
				: client.principal == Principal::token ? MonitorEvent::Via::token : MonitorEvent::Via::session);
			status = model.command(dp->id(), *value);
			if (status != DatapointStatus::ok) {
				code = datapoint_status_code(status);
				reason = datapoint_command_reason(status);
			}
		}
	}
	JsonWriter json(out_, sizeof(out_));
	json.begin_object().member("type", "result");
	if (id_number >= 0 && id_number <= 65535) {
		json.member("id", static_cast<uint32_t>(id_number));
	}
	if (has_ref) {
		json.key("ref").raw(ref.data, ref.length);
	}
	json.member("ok", code == nullptr);
	if (code != nullptr) {
		json.member("error", reason).member("code", code);
	}
	json.end_object();
	send(client, out_, json.length());
}

void LiveFeed::subscribe(Client& client) {
	client.subscribed = true;
	client.snapshot_next = 0;
	send_status(&client);
	schedule_pump();
}

LiveFeed::StatusSignature LiveFeed::signature() {
	Controller& ctl = api_.controller();
	StatusSignature s;
	s.link = static_cast<uint8_t>(ctl.connectivity().state());
	s.mode = static_cast<uint8_t>(ctl.connectivity().mode());
	s.led = static_cast<uint8_t>(ctl.led_status());
	s.synchronized = ctl.clock().wall_time().synchronized;
	s.improv = ctl.improv().active();
	s.mdns = api_.host().mdns_announced();
	s.ip_hash = fnv(ctl.connectivity().ip());
	s.sessions = static_cast<uint32_t>(api_.auth().session_count());
	s.clients = static_cast<uint32_t>(client_count());
	s.datapoints = static_cast<uint32_t>(api_.model().size());
	s.ehome = api_.ehome_signature();
	return s;
}

void LiveFeed::zigbee_event(const zigbee::ZigbeeEvent& event) {
	JsonWriter json(out_, sizeof(out_));
	json.begin_object().member("type", "zigbee").member("event", zigbee::zigbee_event_name(event.kind));
	if (event.ieee != 0) {
		char ieee[zigbee::kIeeeText];
		zigbee::format_ieee(event.ieee, ieee);
		json.member("ieee", ieee);
	}
	if (event.kind == zigbee::ZigbeeEvent::Kind::permit) {
		json.member("permit_join_s", event.permit_join_s);
	}
	json.member("generation", api_.zigbee() != nullptr ? api_.zigbee()->generation() : 0);
	json.end_object();
	size_t length = json.length();
	for (Client& c : clients_) {
		if (c.used && c.subscribed) {
			send(c, out_, length);
		}
	}
}

void LiveFeed::send_status(Client* only) {
	JsonWriter json(out_, sizeof(out_));
	json.begin_object().member("type", "status");
	api_.write_status(json);
	json.end_object();
	if (json.overflowed()) {
		Log::error(kTag, "status message larger than %u bytes: not sent", static_cast<unsigned>(sizeof(out_)));
		return;
	}
	size_t length = json.length();
	if (only != nullptr) {
		send(*only, out_, length);
		return;
	}
	for (Client& c : clients_) {
		if (c.used && c.subscribed) {
			send(c, out_, length);
		}
	}
	last_status_ms_ = api_.controller().clock().monotonic_ms();
}

bool LiveFeed::still_authorized(Client& client) {
	if (client.principal == Principal::ehome) {
		return true; // the E-Home link closes its socket itself
	}
	Auth& auth = api_.auth();
	if (client.principal == Principal::session) {
		if (!auth.session_alive(client.session)) {
			return false;
		}
		auth.touch(client.session); // an open UI keeps its session alive
		return true;
	}
	return auth.token_alive(client.token_slot, client.token_hash);
}

void LiveFeed::tick() {
	uint64_t now = api_.controller().clock().monotonic_ms();
	for (Client& c : clients_) {
		if (!c.used) {
			continue;
		}
		if (!authenticated(c)) {
			if (now - c.opened_ms >= kAuthTimeoutMs) {
				refuse_and_close(c, websocket::kClosePolicy, "auth.timeout", "not authenticated within 10 s");
			}
			continue;
		}
		if (!still_authorized(c)) {
			refuse_and_close(c, websocket::kClosePolicy, "auth.session_ended",
				"the session ended (logout, expiry, password reset) or the token was revoked");
		}
	}
	StatusSignature now_signature = signature();
	if (now_signature != last_signature_ || now - last_status_ms_ >= kHeartbeatMs) {
		last_signature_ = now_signature;
		send_status(nullptr);
	}
	pump();
	monitor_flush(); // retries a client whose send buffers were full
}

void LiveFeed::datapoint_updated(const DatapointUpdate& update) { queue_datapoint(update.datapoint.id()); }

void LiveFeed::datapoint_pending(const Datapoint& datapoint) { queue_datapoint(datapoint.id()); }

void LiveFeed::queue_datapoint(uint16_t id) {
	bool any = false;
	for (const Client& c : clients_) {
		any = any || (c.used && c.subscribed);
	}
	if (!any) {
		return;
	}
	for (size_t i = 0; i < pending_count_; ++i) {
		if (pending_[i] == id) {
			return; // the flush sends the latest value
		}
	}
	if (pending_count_ == kMaxPending) {
		flush();
	}
	pending_[pending_count_++] = id;
	if (!flush_posted_) {
		flush_posted_ = loop_.post(Callback::member<LiveFeed, &LiveFeed::flush>(this));
		if (!flush_posted_) {
			flush();
		}
	}
}

void LiveFeed::flush() {
	flush_posted_ = false;
	if (pending_count_ == 0) {
		return;
	}
	DatapointModel& model = api_.model();
	uint64_t now = api_.controller().clock().monotonic_ms();
	size_t index = 0;
	while (index < pending_count_) {
		JsonWriter json(out_, sizeof(out_));
		json.reserve(kClosingReserve);
		json.begin_object().member("type", "datapoints").member("now_ms", now).key("updates").begin_array();
		size_t in_message = 0;
		for (; index < pending_count_; ++index) {
			const Datapoint* dp = model.get(pending_[index]);
			if (dp == nullptr) {
				continue; // the model was replaced meanwhile
			}
			JsonWriter::Mark mark = json.mark();
			json.begin_object().member("id", static_cast<uint32_t>(dp->id()));
			json.key("value");
			write_value_json(json, dp->value());
			json.member("quality", quality_name(dp->quality()))
				.member("updated_ms", dp->updated_ms())
				.member("pending", dp->pending())
				.end_object();
			if (json.overflowed()) {
				json.reset(mark);
				break;
			}
			++in_message;
		}
		json.release();
		json.end_array().end_object();
		if (in_message == 0 && index < pending_count_) {
			++index; // cannot happen with a 1400-byte buffer; never loop forever
			continue;
		}
		if (in_message > 0) {
			for (Client& c : clients_) {
				if (c.used && c.subscribed) {
					send(c, out_, json.length());
				}
			}
		}
	}
	pending_count_ = 0;
}

void LiveFeed::schedule_pump() {
	if (!pump_posted_) {
		pump_posted_ = loop_.post(Callback::member<LiveFeed, &LiveFeed::pump>(this));
	}
}

void LiveFeed::writable() {
	schedule_pump();
	schedule_monitor_flush();
}

void LiveFeed::resync() {
	attach_monitor();
	monitor_.model_changed(api_.image());
	pending_count_ = 0;
	bool any = false;
	for (Client& c : clients_) {
		if (c.used && c.subscribed) {
			c.snapshot_next = 0;
			any = true;
		}
	}
	if (any) {
		send_status(nullptr);
		schedule_pump();
	}
}

void LiveFeed::pump() {
	pump_posted_ = false;
	DatapointModel& model = api_.model();
	uint64_t now = api_.controller().clock().monotonic_ms();
	bool more = false;
	for (Client& c : clients_) {
		if (!c.used || !c.subscribed || c.snapshot_next == kNoSnapshot) {
			continue;
		}
		bool blocked = false;
		for (size_t chunk = 0; chunk < kChunksPerPump && c.snapshot_next != kNoSnapshot; ++chunk) {
			size_t start = c.snapshot_next;
			JsonWriter json(out_, sizeof(out_));
			json.reserve(kClosingReserve);
			json.begin_object()
				.member("type", "datapoints")
				.member("snapshot", true)
				.member("now_ms", now)
				.member("count", static_cast<uint32_t>(model.size()))
				.member("from", static_cast<uint32_t>(start))
				.key("updates")
				.begin_array();
			size_t id = start;
			for (; id < model.size(); ++id) {
				JsonWriter::Mark mark = json.mark();
				WebApi::write_datapoint(json, *model.get(static_cast<uint16_t>(id)));
				if (json.overflowed()) {
					json.reset(mark);
					break;
				}
			}
			if (id == start && id < model.size()) {
				Log::error(kTag, "datapoint %u does not fit a WebSocket message: skipped in the snapshot",
					static_cast<unsigned>(id));
				++id;
			}
			json.release();
			bool done = id >= model.size();
			json.end_array().member("done", done).end_object();
			if (!c.transport->ws_send_bulk(c.handle, out_, json.length())) {
				blocked = true; // retried on writable() or the next tick
				break;
			}
			c.snapshot_next = done ? kNoSnapshot : id;
		}
		if (c.snapshot_next != kNoSnapshot && !blocked) {
			more = true;
		}
	}
	if (more) {
		schedule_pump();
	}
}

// ---- The monitor stream -----------------------------------------------------------------------

void LiveFeed::monitor_message(Client& client, const JsonSpan& message) {
	JsonSpan member;
	JsonSpan ref;
	bool has_ref = json_member(message, "ref", ref) &&
		(ref.kind == JsonSpan::Kind::number || ref.kind == JsonSpan::Kind::string);
	if (json_member(message, "subscribe", member)) {
		bool on = false;
		if (!json_bool(member, on)) {
			send_error(client, "ws.bad_message", "\"subscribe\" is true or false");
			return;
		}
		JsonWriter json(out_, sizeof(out_));
		json.begin_object().member("type", "monitor").member("subscribed", on);
		if (on) {
			JsonSpan since_span;
			double since = 0;
			bool has_since = json_member(message, "since", since_span);
			if (has_since && (!json_number(since_span, since) || since < 0 || since > 4294967295.0 ||
				since != std::floor(since))) {
				send_error(client, "ws.bad_message", "\"since\" is a sequence number");
				return;
			}
			bool reset = false;
			uint32_t start = has_since ? monitor_.start_of(static_cast<uint32_t>(since), reset) : monitor_.next();
			client.monitor = true;
			client.monitor_next = start;
			json.member("since", start).member("first", monitor_.first()).member("next", monitor_.next())
				.member("capacity", static_cast<uint32_t>(MonitorFeed::kCapacity));
			if (reset) {
				json.member("reset", true);
			}
			Log::info(kTag, "WebSocket client %u: monitor stream from event %u", static_cast<unsigned>(client.handle),
				static_cast<unsigned>(start));
		} else {
			client.monitor = false;
		}
		if (has_ref) {
			json.key("ref").raw(ref.data, ref.length);
		}
		json.end_object();
		send(client, out_, json.length());
		if (on) {
			schedule_monitor_flush();
		}
		return;
	}
	bool is_send = json_member(message, "send", member);
	if (!is_send && !json_member(message, "read", member)) {
		send_error(client, "ws.bad_message", "a monitor message carries \"subscribe\", \"send\" or \"read\"");
		return;
	}
	const char* code = nullptr;
	const char* reason = nullptr;
	bool ok = is_send ? monitor_.send(member, &code, &reason) : monitor_.read(member, &code, &reason);
	if (!ok) {
		Log::warn(kTag, "WebSocket client %u: monitor %s refused: %s", static_cast<unsigned>(client.handle),
			is_send ? "send" : "read", reason);
	}
	JsonWriter json(out_, sizeof(out_));
	json.begin_object().member("type", "monitor").member("result", is_send ? "send" : "read").member("ok", ok);
	if (!ok) {
		json.member("error", reason).member("code", code);
	}
	if (has_ref) {
		json.key("ref").raw(ref.data, ref.length);
	}
	json.end_object();
	send(client, out_, json.length());
}

void LiveFeed::schedule_monitor_flush() {
	if (!monitor_posted_) {
		monitor_posted_ = loop_.post(Callback::member<LiveFeed, &LiveFeed::monitor_flush>(this));
	}
}

void LiveFeed::monitor_flush() {
	monitor_posted_ = false;
	uint64_t now = api_.controller().clock().monotonic_ms();
	bool more = false;
	for (Client& c : clients_) {
		if (!c.used || !c.monitor || !authenticated(c)) {
			continue;
		}
		bool blocked = false;
		for (size_t chunk = 0; chunk < kChunksPerPump && c.monitor_next != monitor_.next(); ++chunk) {
			JsonWriter json(out_, sizeof(out_));
			json.reserve(MonitorFeed::kClosingReserve);
			json.begin_object().member("type", "monitor").member("now_ms", now).key("events").begin_array();
			uint32_t next = monitor_.write_events(json, c.monitor_next, static_cast<size_t>(-1));
			json.release();
			if (next == c.monitor_next) {
				// Cannot happen with 1400 bytes (an event is at most a few hundred); never loop forever.
				Log::error(kTag, "monitor event %u does not fit a WebSocket message: skipped",
					static_cast<unsigned>(next));
				c.monitor_next = next < monitor_.first() ? monitor_.first() : next + 1;
				continue;
			}
			json.end_array().member("next", next).end_object();
			if (!c.transport->ws_send_bulk(c.handle, out_, json.length())) {
				blocked = true; // retried on writable() or the next tick; the ring may overtake it (gap)
				break;
			}
			c.monitor_next = next;
		}
		more = more || (!blocked && c.monitor_next != monitor_.next());
	}
	if (more) {
		schedule_monitor_flush();
	}
}

} // namespace hc
