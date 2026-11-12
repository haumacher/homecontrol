/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/bus_session.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "hc/log.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.ehome";

constexpr uint8_t kText = 1;
constexpr uint8_t kFrame = 2;
constexpr size_t kRecordHeader = 3;

constexpr char kFramesPrefix[] = "{\"type\":\"bus\",\"event\":\"frames\",\"frames\":[";

const char* outcome_code(knx::SendOutcome outcome) {
	switch (outcome) {
		case knx::SendOutcome::confirmed: return "confirmed";
		case knx::SendOutcome::negative: return "negative";
		case knx::SendOutcome::no_confirmation: return "no_confirmation";
		case knx::SendOutcome::lost: return "lost";
	}
	return "lost";
}

int nibble(char c) {
	return (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

constexpr char kHex[] = "0123456789ABCDEF";

/// Writes `ref` (JSON text, "" for none) as the member "ref".
void ref_member(JsonWriter& json, const char* ref) {
	if (ref[0] != '\0') {
		json.key("ref").raw(ref, std::strlen(ref));
	}
}

} // namespace

BusSession::BusSession(EventLoop& loop, const Clock& clock, WebSocketClient& client)
	: loop_(loop), clock_(clock), client_(client) {}

BusSession::~BusSession() {
	delete block_;
}

void BusSession::set_tunnel(knx::KnxTunnel* tunnel) {
	if (tunnel_ != nullptr || tunnel == nullptr) {
		return;
	}
	if (!tunnel->add_listener(this)) {
		Log::error(kTag, "the bus session cannot listen to the KNX tunnel: sessions are refused");
		return;
	}
	tunnel_ = tunnel;
}

// ---- E-Home's messages ------------------------------------------------------------------------

void BusSession::message(const JsonSpan& message) {
	char ref[kMaxRef] = {};
	JsonSpan span;
	bool has_ref = json_member(message, "ref", span);
	if (has_ref) {
		if ((span.kind != JsonSpan::Kind::number && span.kind != JsonSpan::Kind::string) || span.length >= sizeof(ref)) {
			refuse("?", "", "bus.bad_request", "\"ref\" is a number or a string of at most 37 bytes");
			return;
		}
		std::memcpy(ref, span.data, span.length);
		ref[span.length] = '\0';
	}
	JsonSpan op;
	if (!json_member(message, "op", op) || op.kind != JsonSpan::Kind::string) {
		refuse("?", ref, "bus.bad_request", "a bus message carries \"op\": open, send or close");
		return;
	}
	if (json_string_equals(op, "open")) {
		open(ref);
	} else if (json_string_equals(op, "close")) {
		close(ref);
	} else if (json_string_equals(op, "send")) {
		if (!has_ref) {
			refuse("send", ref, "bus.bad_request", "a send needs a \"ref\": its outcome is reported under it");
			return;
		}
		send(message, ref);
	} else {
		refuse("?", ref, "bus.bad_request", "\"op\" is open, send or close");
	}
}

void BusSession::open(const char* ref) {
	if (open_) {
		refuse("open", ref, "bus.busy", "a bus session is open already: one at a time");
		return;
	}
	if (tunnel_ == nullptr) {
		refuse("open", ref, "bus.knx_unavailable", "this controller has no KNX tunnel");
		return;
	}
	if (!tunnel_->is_up()) {
		char error[knx::KnxTunnel::kMaxReason + 32];
		std::snprintf(error, sizeof(error), "the KNX tunnel is not up (%s)", tunnel_->reason());
		refuse("open", ref, "bus.tunnel_down", error);
		return;
	}
	if (block_ == nullptr) {
		block_ = new (std::nothrow) Block();
		if (block_ == nullptr) {
			char error[96];
			std::snprintf(error, sizeof(error), "no memory for the bus session (%u bytes)",
				static_cast<unsigned>(sizeof(Block)));
			refuse("open", ref, "bus.no_memory", error);
			return;
		}
	}
	open_ = true;
	++generation_;
	opened_ms_ = clock_.monotonic_ms();
	activity_ms_ = opened_ms_;
	sent_ = 0;
	received_ = 0;
	dropped_ = 0;
	dropped_total_ = 0;
	address_raw_ = tunnel_->address().raw;
	tunnel_->address().format(address_, sizeof(address_));
	Log::info(kTag, "bus session opened by E-Home: tunnel address %s", address_);
	char text[256];
	JsonWriter json(text, sizeof(text));
	json.begin_object().member("type", "bus").member("event", "opened");
	ref_member(json, ref);
	json.member("address", address_)
		.member("max_frame", static_cast<uint32_t>(knx::RawFrame::kMaxCemi))
		.member("max_apdu", static_cast<uint32_t>(knx::RawFrame::kMaxApdu))
		.member("idle_s", kIdleMs / 1000)
		.end_object();
	queue_text(text, json.length());
}

void BusSession::close(const char* ref) {
	if (!open_) {
		refuse("close", ref, "bus.no_session", "no bus session is open");
		return;
	}
	end("bus.closed", "closed by E-Home", ref, true);
}

void BusSession::send(const JsonSpan& message, const char* ref) {
	if (!open_) {
		refuse("send", ref, "bus.no_session", "no bus session is open");
		return;
	}
	JsonSpan frame;
	if (!json_member(message, "frame", frame) || frame.kind != JsonSpan::Kind::string) {
		refuse("send", ref, "bus.bad_frame", "\"frame\" is a cEMI L_Data.req in hex digits");
		return;
	}
	const char* digits = frame.data + 1;
	size_t count = frame.length - 2;
	for (size_t i = 0; i < count; ++i) {
		if (nibble(digits[i]) < 0) {
			refuse("send", ref, "bus.bad_frame", "\"frame\" holds a character that is no hex digit");
			return;
		}
	}
	if (count == 0 || count % 2 != 0) {
		refuse("send", ref, "bus.bad_frame", "\"frame\" is an even number of hex digits");
		return;
	}
	if (count / 2 > knx::RawFrame::kMaxCemi) {
		char error[96];
		std::snprintf(error, sizeof(error), "the frame has %u octets, more than the %u forwarded",
			static_cast<unsigned>(count / 2), static_cast<unsigned>(knx::RawFrame::kMaxCemi));
		refuse("send", ref, "bus.bad_frame", error);
		return;
	}
	Slot* slot = nullptr;
	for (Slot& s : block_->slots) {
		if (!s.busy) {
			slot = &s;
			break;
		}
	}
	if (slot == nullptr) {
		char error[96];
		std::snprintf(error, sizeof(error), "%u frames of the session wait for their outcome: wait for one",
			static_cast<unsigned>(kSlots));
		refuse("send", ref, "bus.queue_full", error);
		return;
	}
	if (tunnel_->queued() >= knx::KnxTunnel::kQueueCapacity) {
		refuse("send", ref, "bus.queue_full", "the KNX tunnel's queue is full: try again");
		return;
	}
	uint8_t* octets = slot->frame.cemi();
	size_t length = count / 2;
	for (size_t i = 0; i < length; ++i) {
		octets[i] = static_cast<uint8_t>((nibble(digits[2 * i]) << 4) | nibble(digits[2 * i + 1]));
	}
	slot->frame.length = static_cast<uint16_t>(length);
	knx::RawView view;
	knx::CemiResult viewed = knx::view(octets, length, view);
	if (!viewed) {
		refuse("send", ref, "bus.bad_frame", viewed.reason);
		return;
	}
	if (view.code != knx::MessageCode::ldata_req) {
		refuse("send", ref, "bus.bad_frame", "the frame is no L_Data.req (message code 11)");
		return;
	}
	if (view.group && !view.broadcast()) {
		refuse("send", ref, "bus.bad_frame",
			"the frame is a group telegram: the session sends to individual addresses and broadcasts (group "
			"telegrams: the monitor's send)");
		return;
	}
	if (!tunnel_->is_up()) {
		refuse("send", ref, "bus.tunnel_down", "the KNX tunnel is not up");
		return;
	}
	uint32_t token = tunnel_->send_raw(slot->frame);
	if (token == 0) {
		refuse("send", ref, "bus.queue_full", "the KNX tunnel did not take the frame (see the log): try again");
		return;
	}
	slot->busy = true;
	slot->token = token;
	slot->generation = generation_;
	std::memcpy(slot->ref, ref, sizeof(slot->ref));
	++sent_;
	activity_ms_ = clock_.monotonic_ms();
	Log::debug(kTag, "bus session: frame %s of %u octets queued (token %u)", ref, static_cast<unsigned>(length),
		static_cast<unsigned>(token));
}

void BusSession::refuse(const char* op, const char* ref, const char* code, const char* error) {
	Log::warn(kTag, "bus %s%s%s refused: %s (%s)", op, ref[0] != '\0' ? " " : "", ref, error, code);
	char text[512];
	JsonWriter json(text, sizeof(text));
	json.begin_object().member("type", "bus").member("event", "refused").member("op", op);
	ref_member(json, ref);
	json.member("code", code).member("error", error).end_object();
	if (json.overflowed()) {
		return;
	}
	queue_text(text, json.length());
}

void BusSession::end(const char* code, const char* reason, const char* ref, bool tell) {
	if (!open_) {
		return;
	}
	open_ = false;
	uint64_t seconds = (clock_.monotonic_ms() - opened_ms_) / 1000;
	Log::info(kTag, "bus session ended (%s): %u frames sent, %u received, %u dropped, after %u s", reason,
		static_cast<unsigned>(sent_), static_cast<unsigned>(received_), static_cast<unsigned>(dropped_total_),
		static_cast<unsigned>(seconds));
	if (tell) {
		char text[512];
		JsonWriter json(text, sizeof(text));
		json.begin_object().member("type", "bus").member("event", "closed");
		ref_member(json, ref);
		json.member("code", code)
			.member("reason", reason)
			.member("sent", sent_)
			.member("received", received_)
			.member("dropped", dropped_total_)
			.end_object();
		if (!json.overflowed()) {
			queue_text(text, json.length());
		}
	}
	release_if_unused();
}

void BusSession::connection_ended() {
	end("bus.connection_ended", "the connection to E-Home ended", "", false);
	drop_outbox();
	release_if_unused();
}

// ---- The tunnel -------------------------------------------------------------------------------

void BusSession::tunnel_state_changed(const knx::KnxTunnel& tunnel, knx::TunnelState) {
	if (open_ && !tunnel.is_up()) {
		char reason[knx::KnxTunnel::kMaxReason + 32];
		std::snprintf(reason, sizeof(reason), "the KNX tunnel went down: %s", tunnel.reason());
		end("bus.tunnel_down", reason, "", true);
	}
}

void BusSession::tunnel_raw_frame(const knx::KnxTunnel&, const uint8_t* cemi, size_t length) {
	if (!open_) {
		return;
	}
	knx::RawView view;
	if (!knx::view(cemi, length, view) || view.code != knx::MessageCode::ldata_ind) {
		return;
	}
	bool to_us = !view.group && view.destination == address_raw_;
	if (!to_us && !view.broadcast()) {
		return; // a group telegram (datapoints, rules, monitor) or a frame to another device
	}
	activity_ms_ = clock_.monotonic_ms();
	if (block_->used + kRecordHeader + length > kOutbox - kAnswerReserve) {
		++dropped_;
		++dropped_total_;
		Log::debug(kTag, "bus session: an indication of %u octets dropped (the socket does not take the messages)",
			static_cast<unsigned>(length));
		return;
	}
	queue_record(kFrame, cemi, length);
	++received_;
	schedule_flush();
}

void BusSession::tunnel_raw_sent(const knx::KnxTunnel&, uint32_t token, knx::SendOutcome outcome) {
	if (block_ == nullptr) {
		return;
	}
	for (Slot& slot : block_->slots) {
		if (!slot.busy || slot.token != token) {
			continue;
		}
		slot.busy = false;
		if (open_ && slot.generation == generation_) {
			if (outcome != knx::SendOutcome::confirmed) {
				Log::info(kTag, "bus session: frame %s %s", slot.ref, knx::send_outcome_name(outcome));
			}
			char text[160];
			JsonWriter json(text, sizeof(text));
			json.begin_object().member("type", "bus").member("event", "sent");
			ref_member(json, slot.ref);
			json.member("outcome", outcome_code(outcome)).end_object();
			queue_text(text, json.length());
		}
		break;
	}
	release_if_unused();
}

// ---- The outbox -------------------------------------------------------------------------------

bool BusSession::queue_record(uint8_t kind, const uint8_t* data, size_t length) {
	if (block_ == nullptr || block_->used + kRecordHeader + length > kOutbox) {
		return false;
	}
	uint8_t* p = block_->outbox + block_->used;
	p[0] = kind;
	p[1] = static_cast<uint8_t>(length >> 8);
	p[2] = static_cast<uint8_t>(length);
	std::memcpy(p + kRecordHeader, data, length);
	block_->used += kRecordHeader + length;
	return true;
}

bool BusSession::queue_text(const char* text, size_t length) {
	if (block_ == nullptr) {
		// No session's memory (a refused open): straight to the socket.
		if (!client_.send_text(text, length)) {
			client_.want_writable();
			Log::warn(kTag, "a bus answer of %u bytes is lost: the socket's send queue is full",
				static_cast<unsigned>(length));
			return false;
		}
		return true;
	}
	if (!queue_record(kText, reinterpret_cast<const uint8_t*>(text), length)) {
		Log::warn(kTag, "the bus session's messages to E-Home outran the connection (%u bytes waiting)",
			static_cast<unsigned>(block_->used));
		if (open_) {
			drop_outbox();
			end("bus.overflow", "the messages to E-Home outran the connection", "", true);
		}
		return false;
	}
	schedule_flush();
	return true;
}

void BusSession::schedule_flush() {
	if (!flush_posted_) {
		flush_posted_ = loop_.post(Callback::member<BusSession, &BusSession::flush>(this));
	}
}

void BusSession::writable() {
	flush();
}

void BusSession::tick() {
	if (open_ && clock_.monotonic_ms() - activity_ms_ >= kIdleMs) {
		char reason[64];
		std::snprintf(reason, sizeof(reason), "no frame for %u s", static_cast<unsigned>(kIdleMs / 1000));
		end("bus.idle", reason, "", true);
	}
	if (block_ != nullptr && block_->used > 0) {
		flush();
	}
}

void BusSession::flush() {
	flush_posted_ = false;
	if (block_ == nullptr) {
		return;
	}
	uint8_t* box = block_->outbox;
	while (block_->used > 0) {
		size_t length = static_cast<size_t>((box[1] << 8) | box[2]);
		if (box[0] == kText) {
			if (!client_.send_text(reinterpret_cast<const char*>(box + kRecordHeader), length)) {
				client_.want_writable();
				return;
			}
			std::memmove(box, box + kRecordHeader + length, block_->used - kRecordHeader - length);
			block_->used -= kRecordHeader + length;
			continue;
		}
		// Consecutive indications in one `frames` message of at most WebSocketClient::kMaxText bytes.
		char suffix[40];
		int suffix_length = dropped_ > 0
			? std::snprintf(suffix, sizeof(suffix), "],\"dropped\":%u}", static_cast<unsigned>(dropped_))
			: std::snprintf(suffix, sizeof(suffix), "]}");
		size_t total = sizeof(kFramesPrefix) - 1 + static_cast<size_t>(suffix_length);
		size_t consumed = 0;
		size_t frames = 0;
		while (consumed < block_->used && box[consumed] == kFrame) {
			size_t n = static_cast<size_t>((box[consumed + 1] << 8) | box[consumed + 2]);
			size_t cost = 2 * n + 2 + (frames > 0 ? 1 : 0);
			if (total + cost > WebSocketClient::kMaxText) {
				break;
			}
			total += cost;
			consumed += kRecordHeader + n;
			++frames;
		}
		char* out = client_.claim_text(total);
		if (out == nullptr) {
			client_.want_writable();
			return;
		}
		char* p = out;
		std::memcpy(p, kFramesPrefix, sizeof(kFramesPrefix) - 1);
		p += sizeof(kFramesPrefix) - 1;
		for (size_t at = 0, i = 0; i < frames; ++i) {
			size_t n = static_cast<size_t>((box[at + 1] << 8) | box[at + 2]);
			const uint8_t* octets = box + at + kRecordHeader;
			if (i > 0) {
				*p++ = ',';
			}
			*p++ = '"';
			for (size_t k = 0; k < n; ++k) {
				*p++ = kHex[octets[k] >> 4];
				*p++ = kHex[octets[k] & 0x0F];
			}
			*p++ = '"';
			at += kRecordHeader + n;
		}
		std::memcpy(p, suffix, static_cast<size_t>(suffix_length));
		client_.commit_text(total);
		dropped_ = 0;
		std::memmove(box, box + consumed, block_->used - consumed);
		block_->used -= consumed;
	}
	release_if_unused();
}

void BusSession::drop_outbox() {
	if (block_ != nullptr) {
		block_->used = 0;
	}
	dropped_ = 0;
}

void BusSession::release_if_unused() {
	if (block_ == nullptr || open_ || block_->used > 0) {
		return;
	}
	for (const Slot& slot : block_->slots) {
		if (slot.busy) {
			return;
		}
	}
	delete block_;
	block_ = nullptr;
}

// ---- Status -----------------------------------------------------------------------------------

void BusSession::write_status(JsonWriter& json) const {
	json.begin_object().member("open", open_);
	if (open_) {
		json.member("since_s", (clock_.monotonic_ms() - opened_ms_) / 1000)
			.member("address", address_)
			.member("sent", sent_)
			.member("received", received_)
			.member("dropped", dropped_total_);
	}
	json.end_object();
}

} // namespace hc
