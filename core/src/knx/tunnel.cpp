/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/knx/tunnel.h"

#include <cstdio>
#include <cstring>

#include "hc/log.h"

namespace hc {
namespace knx {

namespace {

constexpr const char* kTag = "hc.knx";

} // namespace

const char* tunnel_state_name(TunnelState state) {
	switch (state) {
	case TunnelState::down:
		return "down";
	case TunnelState::connecting:
		return "connecting";
	case TunnelState::up:
		return "up";
	}
	return "?";
}

const char* send_outcome_name(SendOutcome outcome) {
	switch (outcome) {
	case SendOutcome::confirmed:
		return "confirmed";
	case SendOutcome::negative:
		return "negative confirmation";
	case SendOutcome::no_confirmation:
		return "no confirmation";
	case SendOutcome::lost:
		return "lost";
	}
	return "?";
}

KnxTunnel::KnxTunnel(EventLoop& loop, const Clock& clock, Connectivity& connectivity, UdpSocket& socket)
	: loop_(loop), clock_(clock), connectivity_(connectivity), socket_(socket) {}

KnxTunnel::~KnxTunnel() {
	cancel_timers();
	socket_.close();
}

bool KnxTunnel::add_listener(KnxTunnelListener* listener) {
	if (listener == nullptr) {
		return false;
	}
	if (listener_count_ == kListenerCapacity) {
		Log::error(kTag, "refusing a tunnel listener: capacity of %u exhausted", static_cast<unsigned>(kListenerCapacity));
		return false;
	}
	listeners_[listener_count_++] = listener;
	return true;
}

bool KnxTunnel::refuse_notifying(const char* what) {
	if (notifying_) {
		Log::error(kTag, "refusing %s from a tunnel state listener", what);
		return true;
	}
	return false;
}

void KnxTunnel::set_gateway(const GatewayAddress* gateway, bool nat) {
	if (refuse_notifying("set_gateway")) {
		return;
	}
	gateway_ = gateway != nullptr ? *gateway : GatewayAddress{};
	nat_ = nat;
	char text[GatewayAddress::kMaxText];
	if (gateway_.empty()) {
		Log::info(kTag, "knx gateway cleared");
	} else {
		Log::info(kTag, "knx gateway %s%s", gateway_.format(text, sizeof(text)), nat_ ? " (NAT mode)" : "");
	}
	if (phase_ != Phase::idle || retry_timer_.valid()) {
		teardown(gateway_.empty() ? "gateway cleared" : "gateway changed", phase_ == Phase::up, false, LogLevel::info);
	}
	waiting_ = nullptr;
	backoff_.reset();
	evaluate();
}

void KnxTunnel::start() {
	if (refuse_notifying("start")) {
		return;
	}
	if (started_) {
		Log::warn(kTag, "knx tunnel already started");
		return;
	}
	if (!registered_) {
		registered_ = connectivity_.add_listener(this);
		if (!registered_) {
			Log::error(kTag, "knx tunnel cannot watch the network: it will not connect");
			return;
		}
	}
	started_ = true;
	waiting_ = nullptr;
	backoff_.reset();
	evaluate();
}

void KnxTunnel::stop() {
	if (refuse_notifying("stop")) {
		return;
	}
	if (!started_) {
		return;
	}
	started_ = false;
	if (phase_ != Phase::idle || retry_timer_.valid()) {
		teardown("stopped", phase_ == Phase::up, false, LogLevel::info);
	} else {
		Log::info(kTag, "knx tunnel stopped");
	}
}

bool KnxTunnel::can_connect(const char** why) const {
	if (!started_) {
		*why = "not started";
		return false;
	}
	if (gateway_.empty()) {
		*why = "no gateway set (knx.gateway)";
		return false;
	}
	if (!connectivity_.is_up()) {
		*why = "network not up";
		return false;
	}
	return true;
}

void KnxTunnel::evaluate() {
	if (phase_ != Phase::idle || retry_timer_.valid()) {
		return;
	}
	const char* waiting = nullptr;
	if (!can_connect(&waiting)) {
		if (started_ && waiting != waiting_) {
			Log::info(kTag, "knx tunnel waiting: %s", waiting);
		}
		waiting_ = waiting;
		return;
	}
	waiting_ = nullptr;
	schedule_attempt(0);
}

void KnxTunnel::schedule_attempt(uint32_t delay_ms) {
	loop_.cancel(retry_timer_);
	retry_at_ = clock_.monotonic_ms() + delay_ms;
	retry_timer_ = loop_.schedule_at(retry_at_, Callback::member<KnxTunnel, &KnxTunnel::retry_due>(this));
	if (!retry_timer_.valid()) {
		retry_at_ = EventLoop::kNever;
	}
}

void KnxTunnel::retry_due() {
	retry_timer_ = TimerHandle();
	retry_at_ = EventLoop::kNever;
	const char* why = nullptr;
	if (!can_connect(&why)) {
		Log::info(kTag, "knx tunnel attempt skipped: %s", why);
		return;
	}
	attempt();
}

void KnxTunnel::attempt() {
	++attempts_;
	char text[GatewayAddress::kMaxText];
	gateway_.format(text, sizeof(text));
	std::snprintf(reason_, sizeof(reason_), "connecting to %s", text);
	Log::info(kTag, "knx tunnel connecting to %s%s (attempt %u)", text, nat_ ? " in NAT mode" : "",
		static_cast<unsigned>(attempts_));
	connect_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kConnectTimeoutMs,
		Callback::member<KnxTunnel, &KnxTunnel::connect_timeout>(this));
	set_state(TunnelState::connecting, reason_);
	uint32_t address = 0;
	if (parse_ipv4(gateway_.host, address)) {
		connect_to(address);
		return;
	}
	phase_ = Phase::resolving;
	const char* error = nullptr;
	if (!socket_.resolve(gateway_.host, *this, &error)) {
		char reason[kMaxReason];
		std::snprintf(reason, sizeof(reason), "cannot resolve %s: %s", gateway_.host, error);
		fail(reason, true);
	}
}

void KnxTunnel::udp_resolved(bool ok, uint32_t address, const char* error) {
	if (phase_ != Phase::resolving) {
		return; // a resolution of an attempt that is over
	}
	if (!ok) {
		char reason[kMaxReason];
		std::snprintf(reason, sizeof(reason), "cannot resolve %s: %s", gateway_.host, error);
		fail(reason, true);
		return;
	}
	char ip[kMaxIpv4Text];
	Log::info(kTag, "knx gateway %s is %s", gateway_.host, format_ipv4(address, ip, sizeof(ip)));
	connect_to(address);
}

void KnxTunnel::connect_to(uint32_t address) {
	control_ = ip::Endpoint{address, gateway_.port};
	data_ = control_;
	const char* error = nullptr;
	char reason[kMaxReason];
	if (!socket_.open(*this, &error)) {
		std::snprintf(reason, sizeof(reason), "cannot open a UDP socket: %s", error);
		fail(reason, true);
		return;
	}
	if (nat_) {
		local_ = ip::Endpoint{};
	} else if (!socket_.local_endpoint(control_, local_, &error)) {
		std::snprintf(reason, sizeof(reason), "no local address towards the gateway: %s", error);
		fail(reason, true);
		return;
	}
	phase_ = Phase::connecting;
	ip::FrameBuffer frame;
	ip::build_connect_request(frame, local_, local_);
	if (!transmit(control_, frame, "CONNECT_REQUEST")) {
		fail("cannot send the CONNECT_REQUEST", true);
	}
}

void KnxTunnel::connect_timeout() {
	connect_timer_ = TimerHandle();
	if (phase_ == Phase::resolving) {
		fail("the gateway's host name did not resolve within 10 s", true);
	} else if (phase_ == Phase::connecting) {
		fail("no CONNECT_RESPONSE within 10 s", true);
	}
}

bool KnxTunnel::transmit(const ip::Endpoint& to, const ip::FrameBuffer& frame, const char* what) {
	const char* error = nullptr;
	if (!socket_.send_to(to, frame.data, frame.length, &error)) {
		char text[ip::Endpoint::kMaxText];
		Log::warn(kTag, "knx %s to %s not sent: %s", what, to.format(text, sizeof(text)), error);
		return false;
	}
	return true;
}

// ---- Receiving -----------------------------------------------------------------------------

void KnxTunnel::udp_received(const uint8_t* data, size_t length, const Ipv4Endpoint& from) {
	if (phase_ != Phase::connecting && phase_ != Phase::up) {
		return;
	}
	if (from.address != control_.address) {
		char text[ip::Endpoint::kMaxText];
		Log::debug(kTag, "ignoring a datagram from %s (not the gateway)", from.format(text, sizeof(text)));
		return;
	}
	uint16_t service = 0;
	ip::ParseResult result = ip::parse_header(data, length, service);
	if (!result) {
		Log::warn(kTag, "ignoring a datagram of %u octets from the gateway: %s", static_cast<unsigned>(length),
			result.reason);
		return;
	}
	switch (static_cast<ip::Service>(service)) {
	case ip::Service::connect_response:
		handle_connect_response(data, length, from);
		return;
	case ip::Service::connectionstate_response:
		handle_connectionstate_response(data, length);
		return;
	case ip::Service::disconnect_request:
		handle_disconnect_request(data, length);
		return;
	case ip::Service::disconnect_response:
		Log::debug(kTag, "DISCONNECT_RESPONSE ignored (no disconnect pending)");
		return;
	case ip::Service::tunneling_request:
		handle_tunneling_request(data, length);
		return;
	case ip::Service::tunneling_ack:
		handle_tunneling_ack(data, length);
		return;
	default:
		Log::debug(kTag, "ignoring %s (0x%04X) from the gateway", ip::service_name(service), service);
		return;
	}
}

void KnxTunnel::udp_error(const char* reason) {
	Log::warn(kTag, "knx socket error: %s", reason);
}

void KnxTunnel::handle_connect_response(const uint8_t* data, size_t length, const Ipv4Endpoint& from) {
	if (phase_ != Phase::connecting) {
		Log::debug(kTag, "CONNECT_RESPONSE ignored (not connecting)");
		return;
	}
	ip::ConnectResponse response;
	ip::ParseResult result = ip::parse_connect_response(data, length, response);
	char reason[kMaxReason];
	if (!result) {
		std::snprintf(reason, sizeof(reason), "malformed CONNECT_RESPONSE: %s", result.reason);
		fail(reason, true);
		return;
	}
	if (response.status != ip::status::kNoError) {
		std::snprintf(reason, sizeof(reason), "gateway refused the connection: %s (0x%02X)",
			ip::status_name(response.status), response.status);
		fail(reason, true);
		return;
	}
	loop_.cancel(connect_timer_);
	connect_timer_ = TimerHandle();
	channel_ = response.channel;
	address_ = response.address;
	data_ = response.data;
	// NAT mode, or a gateway that leaves it to us: the data endpoint is where the response came from.
	if (data_.address == 0) {
		data_.address = from.address;
	}
	if (data_.port == 0) {
		data_.port = from.port;
	}
	send_sequence_ = 0;
	receive_sequence_ = 0;
	heartbeat_misses_ = 0;
	heartbeat_outstanding_ = false;
	phase_ = Phase::up;
	backoff_.reset();
	heartbeat_timer_ = loop_.schedule_periodic(clock_.monotonic_ms() + kHeartbeatPeriodMs, kHeartbeatPeriodMs,
		Callback::member<KnxTunnel, &KnxTunnel::heartbeat_due>(this));
	char address[IndividualAddress::kMaxText];
	char control[ip::Endpoint::kMaxText];
	char endpoint[ip::Endpoint::kMaxText];
	std::snprintf(reason_, sizeof(reason_), "channel %u, address %s", static_cast<unsigned>(channel_),
		address_.format(address, sizeof(address)));
	Log::info(kTag, "knx tunnel up: channel %u, address %s, gateway %s (data endpoint %s)",
		static_cast<unsigned>(channel_), address, control_.format(control, sizeof(control)),
		data_.format(endpoint, sizeof(endpoint)));
	set_state(TunnelState::up, reason_);
	pump();
}

void KnxTunnel::handle_connectionstate_response(const uint8_t* data, size_t length) {
	ip::ChannelStatus response;
	ip::ParseResult result = ip::parse_connectionstate_response(data, length, response);
	if (!result) {
		Log::warn(kTag, "ignoring a malformed CONNECTIONSTATE_RESPONSE: %s", result.reason);
		return;
	}
	if (phase_ != Phase::up || response.channel != channel_ || !heartbeat_outstanding_) {
		Log::debug(kTag, "CONNECTIONSTATE_RESPONSE for channel %u ignored", static_cast<unsigned>(response.channel));
		return;
	}
	heartbeat_outstanding_ = false;
	loop_.cancel(heartbeat_timeout_timer_);
	heartbeat_timeout_timer_ = TimerHandle();
	if (response.status != ip::status::kNoError) {
		char reason[kMaxReason];
		std::snprintf(reason, sizeof(reason), "heartbeat answered with %s (0x%02X)", ip::status_name(response.status),
			response.status);
		broken(reason);
		return;
	}
	heartbeat_misses_ = 0;
}

void KnxTunnel::handle_disconnect_request(const uint8_t* data, size_t length) {
	ip::DisconnectRequest request;
	ip::ParseResult result = ip::parse_disconnect_request(data, length, request);
	if (!result) {
		Log::warn(kTag, "ignoring a malformed DISCONNECT_REQUEST: %s", result.reason);
		return;
	}
	if (phase_ != Phase::up || request.channel != channel_) {
		Log::debug(kTag, "DISCONNECT_REQUEST for channel %u ignored", static_cast<unsigned>(request.channel));
		return;
	}
	ip::FrameBuffer frame;
	ip::build_disconnect_response(frame, channel_, ip::status::kNoError);
	transmit(control_, frame, "DISCONNECT_RESPONSE");
	fail("the gateway closed the tunnel (DISCONNECT_REQUEST)", true);
}

void KnxTunnel::handle_tunneling_request(const uint8_t* data, size_t length) {
	ip::TunnelingRequest request;
	ip::ParseResult result = ip::parse_tunneling_request(data, length, request);
	if (!result && result.status != ip::ParseStatus::cemi) {
		Log::warn(kTag, "ignoring a malformed TUNNELING_REQUEST: %s", result.reason);
		return;
	}
	if (phase_ != Phase::up || request.channel != channel_) {
		Log::debug(kTag, "TUNNELING_REQUEST for channel %u ignored", static_cast<unsigned>(request.channel));
		return;
	}
	ip::FrameBuffer ack;
	ip::build_tunneling_ack(ack, channel_, request.sequence, ip::status::kNoError);
	if (request.sequence == static_cast<uint8_t>(receive_sequence_ - 1)) {
		// The gateway missed our ACK and repeats: acknowledge again, do not deliver twice.
		Log::debug(kTag, "repeated TUNNELING_REQUEST %u acknowledged again, not delivered",
			static_cast<unsigned>(request.sequence));
		transmit(data_, ack, "TUNNELING_ACK");
		return;
	}
	if (request.sequence != receive_sequence_) {
		Log::warn(kTag, "ignoring TUNNELING_REQUEST with sequence %u (expected %u)",
			static_cast<unsigned>(request.sequence), static_cast<unsigned>(receive_sequence_));
		return;
	}
	transmit(data_, ack, "TUNNELING_ACK");
	++receive_sequence_;
	if (!result) {
		Log::warn(kTag, "dropping a tunnelled frame: %s", request.cemi.reason);
		return;
	}
	switch (request.frame.code) {
	case MessageCode::ldata_ind:
		for (size_t i = 0; i < listener_count_; ++i) {
			listeners_[i]->tunnel_frame(*this, request.frame);
		}
		return;
	case MessageCode::ldata_con:
		handle_confirmation(request.frame);
		return;
	case MessageCode::ldata_req:
		Log::debug(kTag, "ignoring an L_Data.req from the gateway");
		return;
	}
}

void KnxTunnel::handle_confirmation(const LData& frame) {
	if (queue_count_ == 0 || (!awaiting_ack_ && !awaiting_confirm_) || !confirms(frame, queue_[queue_head_].frame)) {
		Log::debug(kTag, "L_Data.con for no frame in flight ignored");
		return;
	}
	if (awaiting_ack_) {
		early_confirm_ = true;
		early_frame_ = frame;
		return;
	}
	complete(frame.confirm_error ? SendOutcome::negative : SendOutcome::confirmed, &frame);
}

void KnxTunnel::handle_tunneling_ack(const uint8_t* data, size_t length) {
	ip::TunnelingAck ack;
	ip::ParseResult result = ip::parse_tunneling_ack(data, length, ack);
	if (!result) {
		Log::warn(kTag, "ignoring a malformed TUNNELING_ACK: %s", result.reason);
		return;
	}
	if (phase_ != Phase::up || ack.channel != channel_ || !awaiting_ack_ || ack.sequence != send_sequence_) {
		Log::debug(kTag, "TUNNELING_ACK channel %u sequence %u ignored", static_cast<unsigned>(ack.channel),
			static_cast<unsigned>(ack.sequence));
		return;
	}
	if (ack.status != ip::status::kNoError) {
		Log::warn(kTag, "TUNNELING_ACK %u with %s (0x%02X): waiting for the repetition", static_cast<unsigned>(ack.sequence),
			ip::status_name(ack.status), ack.status);
		return;
	}
	loop_.cancel(ack_timer_);
	ack_timer_ = TimerHandle();
	awaiting_ack_ = false;
	++send_sequence_;
	if (early_confirm_) {
		early_confirm_ = false;
		LData confirmation = early_frame_;
		complete(confirmation.confirm_error ? SendOutcome::negative : SendOutcome::confirmed, &confirmation);
		return;
	}
	awaiting_confirm_ = true;
	confirm_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kConfirmTimeoutMs,
		Callback::member<KnxTunnel, &KnxTunnel::confirm_timeout>(this));
}

// ---- Heartbeat -----------------------------------------------------------------------------

void KnxTunnel::heartbeat_due() {
	if (phase_ != Phase::up || heartbeat_outstanding_) {
		return;
	}
	heartbeat_misses_ = 0;
	send_heartbeat();
}

void KnxTunnel::send_heartbeat() {
	ip::FrameBuffer frame;
	ip::build_connectionstate_request(frame, channel_, local_);
	transmit(control_, frame, "CONNECTIONSTATE_REQUEST");
	heartbeat_outstanding_ = true;
	heartbeat_timeout_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kHeartbeatTimeoutMs,
		Callback::member<KnxTunnel, &KnxTunnel::heartbeat_timeout>(this));
}

void KnxTunnel::heartbeat_timeout() {
	heartbeat_timeout_timer_ = TimerHandle();
	if (phase_ != Phase::up || !heartbeat_outstanding_) {
		return;
	}
	heartbeat_outstanding_ = false;
	if (++heartbeat_misses_ >= kHeartbeatAttempts) {
		broken("no answer to 3 heartbeats (CONNECTIONSTATE_REQUEST)");
		return;
	}
	Log::warn(kTag, "knx heartbeat unanswered within 10 s (%u of %u), repeating", static_cast<unsigned>(heartbeat_misses_),
		static_cast<unsigned>(kHeartbeatAttempts));
	send_heartbeat();
}

// ---- Sending -------------------------------------------------------------------------------

uint32_t KnxTunnel::send(const LData& frame) {
	if (phase_ != Phase::up) {
		Log::warn(kTag, "refusing to send: the tunnel is %s (%s)", tunnel_state_name(state_), reason_);
		return 0;
	}
	if (queue_count_ == kQueueCapacity) {
		Log::warn(kTag, "refusing to send: the queue of %u frames is full", static_cast<unsigned>(kQueueCapacity));
		return 0;
	}
	ip::FrameBuffer probe;
	CemiResult encoded;
	if (!ip::build_tunneling_request(probe, channel_, 0, frame, &encoded)) {
		Log::warn(kTag, "refusing to send a frame that does not encode: %s", encoded.reason);
		return 0;
	}
	uint32_t token = next_token_++;
	if (next_token_ == 0) {
		next_token_ = 1;
	}
	Pending& entry = queue_[(queue_head_ + queue_count_) % kQueueCapacity];
	entry.frame = frame;
	entry.token = token;
	++queue_count_;
	pump();
	return token;
}

void KnxTunnel::pump() {
	if (phase_ != Phase::up || awaiting_ack_ || awaiting_confirm_ || queue_count_ == 0) {
		return;
	}
	repeated_ = false;
	early_confirm_ = false;
	awaiting_ack_ = true;
	send_head();
}

void KnxTunnel::send_head() {
	ip::FrameBuffer frame;
	ip::build_tunneling_request(frame, channel_, send_sequence_, queue_[queue_head_].frame);
	// A failed send is not fatal here: the missing ACK repeats it and then ends the connection.
	transmit(data_, frame, "TUNNELING_REQUEST");
	ack_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kAckTimeoutMs,
		Callback::member<KnxTunnel, &KnxTunnel::ack_timeout>(this));
}

void KnxTunnel::ack_timeout() {
	ack_timer_ = TimerHandle();
	if (phase_ != Phase::up || !awaiting_ack_) {
		return;
	}
	char reason[kMaxReason];
	if (repeated_) {
		std::snprintf(reason, sizeof(reason), "no TUNNELING_ACK for sequence %u after a repetition",
			static_cast<unsigned>(send_sequence_));
		broken(reason);
		return;
	}
	Log::warn(kTag, "no TUNNELING_ACK for sequence %u within 1 s, repeating", static_cast<unsigned>(send_sequence_));
	repeated_ = true;
	send_head();
}

void KnxTunnel::confirm_timeout() {
	confirm_timer_ = TimerHandle();
	if (phase_ != Phase::up || !awaiting_confirm_) {
		return;
	}
	Log::warn(kTag, "no L_Data.con within 3 s for the frame acknowledged with sequence %u",
		static_cast<unsigned>(static_cast<uint8_t>(send_sequence_ - 1)));
	complete(SendOutcome::no_confirmation, nullptr);
}

void KnxTunnel::complete(SendOutcome outcome, const LData* confirmation) {
	loop_.cancel(confirm_timer_);
	confirm_timer_ = TimerHandle();
	awaiting_confirm_ = false;
	awaiting_ack_ = false;
	Pending done = queue_[queue_head_];
	queue_head_ = (queue_head_ + 1) % kQueueCapacity;
	--queue_count_;
	const LData& reported = confirmation != nullptr ? *confirmation : done.frame;
	for (size_t i = 0; i < listener_count_; ++i) {
		listeners_[i]->tunnel_sent(*this, done.token, reported, outcome);
	}
	pump();
}

void KnxTunnel::flush_queue() {
	awaiting_ack_ = false;
	awaiting_confirm_ = false;
	early_confirm_ = false;
	size_t count = queue_count_;
	if (count > 0) {
		Log::warn(kTag, "knx: %u queued frame(s) not sent (tunnel down)", static_cast<unsigned>(count));
	}
	// Listeners may call send() meanwhile; it refuses because the phase is idle.
	while (queue_count_ > 0) {
		Pending done = queue_[queue_head_];
		queue_head_ = (queue_head_ + 1) % kQueueCapacity;
		--queue_count_;
		for (size_t i = 0; i < listener_count_; ++i) {
			listeners_[i]->tunnel_sent(*this, done.token, done.frame, SendOutcome::lost);
		}
	}
	queue_head_ = 0;
}

// ---- Tearing down --------------------------------------------------------------------------

void KnxTunnel::broken(const char* reason) {
	teardown(reason, true, true, LogLevel::warn);
}

void KnxTunnel::fail(const char* reason, bool retry) {
	teardown(reason, false, retry, LogLevel::warn);
}

void KnxTunnel::teardown(const char* reason, bool send_disconnect, bool retry, LogLevel level) {
	cancel_timers();
	if (send_disconnect && phase_ == Phase::up) {
		ip::FrameBuffer frame;
		ip::build_disconnect_request(frame, channel_, local_);
		transmit(control_, frame, "DISCONNECT_REQUEST");
	}
	socket_.close();
	phase_ = Phase::idle;
	heartbeat_outstanding_ = false;
	flush_queue();
	std::snprintf(reason_, sizeof(reason_), "%s", reason);
	const char* why = nullptr;
	if (retry && can_connect(&why)) {
		uint32_t delay = backoff_.next();
		Log::warn(kTag, "knx tunnel down: %s; reconnect in %u s (failure %u)", reason, static_cast<unsigned>(delay / 1000),
			static_cast<unsigned>(backoff_.failures()));
		schedule_attempt(delay);
	} else {
		Log::write(level, kTag, "knx tunnel down: %s", reason);
	}
	set_state(TunnelState::down, reason_);
}

void KnxTunnel::cancel_timers() {
	loop_.cancel(retry_timer_);
	loop_.cancel(connect_timer_);
	loop_.cancel(heartbeat_timer_);
	loop_.cancel(heartbeat_timeout_timer_);
	loop_.cancel(ack_timer_);
	loop_.cancel(confirm_timer_);
	retry_timer_ = TimerHandle();
	connect_timer_ = TimerHandle();
	heartbeat_timer_ = TimerHandle();
	heartbeat_timeout_timer_ = TimerHandle();
	ack_timer_ = TimerHandle();
	confirm_timer_ = TimerHandle();
	retry_at_ = EventLoop::kNever;
}

void KnxTunnel::connectivity_changed(const Connectivity& connectivity, LinkState previous) {
	bool up = connectivity.state() == LinkState::up;
	if (up && previous != LinkState::up) {
		backoff_.reset();
		evaluate();
		return;
	}
	if (!up && previous == LinkState::up && started_) {
		if (phase_ != Phase::idle || retry_timer_.valid()) {
			// The link is gone: a DISCONNECT_REQUEST would not get out. Connect again when it is back.
			teardown("network down", false, false, LogLevel::warn);
		}
		waiting_ = "network not up";
	}
}

void KnxTunnel::set_state(TunnelState state, const char* reason) {
	(void) reason;
	if (state == state_) {
		return;
	}
	TunnelState previous = state_;
	state_ = state;
	notifying_ = true;
	for (size_t i = 0; i < listener_count_; ++i) {
		listeners_[i]->tunnel_state_changed(*this, previous);
	}
	notifying_ = false;
}

} // namespace knx
} // namespace hc
