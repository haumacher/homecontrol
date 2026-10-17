/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_TUNNEL_H
#define HC_KNX_TUNNEL_H

#include <cstddef>
#include <cstdint>

#include "hc/backoff.h"
#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/knx/address.h"
#include "hc/knx/cemi.h"
#include "hc/knx/knx_settings.h"
#include "hc/knx/knxnetip.h"
#include "hc/log.h"
#include "hc/platform/clock.h"
#include "hc/platform/udp_socket.h"

namespace hc {
namespace knx {

/// The observable state of the tunnel.
enum class TunnelState : uint8_t {
	down,
	connecting, ///< Resolving the gateway or waiting for its CONNECT_RESPONSE.
	up,
};

/// "down", "connecting", "up".
const char* tunnel_state_name(TunnelState state);

/// What became of a frame given to KnxTunnel::send().
enum class SendOutcome : uint8_t {
	confirmed,       ///< The gateway acknowledged it and confirmed it on the bus (L_Data.con).
	negative,        ///< The gateway acknowledged it, but its confirmation says it was not sent.
	no_confirmation, ///< Acknowledged, but no L_Data.con within KnxTunnel::kConfirmTimeoutMs.
	lost,            ///< The tunnel went down before the frame was acknowledged; it was not sent.
};

/// "confirmed", "negative confirmation", "no confirmation", "lost".
const char* send_outcome_name(SendOutcome outcome);

class KnxTunnel;

/// Observes a tunnel. Called on the event loop's thread, in registration order. A listener may
/// send() from any callback; it must not call start(), stop() or set_gateway() from
/// tunnel_state_changed() (refused and logged).
class KnxTunnelListener {
public:
	/// The state changed (the reason is KnxTunnel::reason()).
	virtual void tunnel_state_changed(const KnxTunnel& tunnel, TunnelState previous) {
		(void) tunnel;
		(void) previous;
	}

	/// An L_Data.ind arrived: a telegram from the bus.
	virtual void tunnel_frame(const KnxTunnel& tunnel, const LData& frame) {
		(void) tunnel;
		(void) frame;
	}

	/// A frame given to send() is done. For `confirmed` and `negative`, `frame` is the gateway's
	/// L_Data.con (with the tunnel's address as source); otherwise the frame as it was given.
	virtual void tunnel_sent(const KnxTunnel& tunnel, uint32_t token, const LData& frame, SendOutcome outcome) {
		(void) tunnel;
		(void) token;
		(void) frame;
		(void) outcome;
	}

protected:
	~KnxTunnelListener() = default;
};

/// A KNXnet/IP tunnelling connection on the link layer to one gateway (KNX 03.08.04), driven by the
/// event loop (doc/knx.md has the state machine):
///
/// - It connects only while it is started, a gateway is set and the network (Connectivity) is up;
///   it tears down without reconnecting when the network goes down, and connects again when it
///   comes back.
/// - Connecting: resolve the gateway's host name (an IPv4 literal needs none), open the socket,
///   CONNECT_REQUEST; an attempt without a CONNECT_RESPONSE after kConnectTimeoutMs fails. A
///   refused or failed attempt is retried after the Backoff's delay (1 s, 2 s, ... 60 s; reset
///   when the tunnel comes up and when the network comes up).
/// - Heartbeat: CONNECTIONSTATE_REQUEST every kHeartbeatPeriodMs; an unanswered one is repeated
///   after kHeartbeatTimeoutMs, kHeartbeatAttempts unanswered in a row or an error status end the
///   connection (DISCONNECT_REQUEST, then reconnect).
/// - Sending: one TUNNELING_REQUEST at a time from a queue of kQueueCapacity frames, with the send
///   sequence counter; without a TUNNELING_ACK after kAckTimeoutMs it is repeated once, a second
///   miss ends the connection. After the ACK the next frame waits for the gateway's L_Data.con (at
///   most kConfirmTimeoutMs), which paces the queue to the bus and tells the sender the outcome.
/// - Receiving: a TUNNELING_REQUEST with the expected sequence number is acknowledged and
///   delivered; one with the previous number (the gateway repeats a request whose ACK it missed)
///   is acknowledged again but not delivered; any other is ignored (logged).
/// - The gateway's DISCONNECT_REQUEST is answered and ends the connection (reconnect follows).
/// - stop() sends a DISCONNECT_REQUEST and does not reconnect.
///
/// Every transition is logged under the tag "hc.knx" with its reason. Frames still queued when the
/// connection ends are reported `lost`, never sent later.
///
/// Threading: every method is called on the event loop's thread.
class KnxTunnel : public ConnectivityListener, public UdpReceiver {
public:
	static constexpr uint32_t kConnectTimeoutMs = 10000;
	static constexpr uint32_t kHeartbeatPeriodMs = 60000;
	static constexpr uint32_t kHeartbeatTimeoutMs = 10000;
	static constexpr uint32_t kHeartbeatAttempts = 3;
	static constexpr uint32_t kAckTimeoutMs = 1000;
	static constexpr uint32_t kConfirmTimeoutMs = 3000;
	static constexpr size_t kQueueCapacity = 32;
	static constexpr size_t kListenerCapacity = 4;
	/// Longest reason text including the NUL (a gateway host name has up to 127 characters).
	static constexpr size_t kMaxReason = 200;

	KnxTunnel(EventLoop& loop, const Clock& clock, Connectivity& connectivity, UdpSocket& socket);
	~KnxTunnel();

	KnxTunnel(const KnxTunnel&) = delete;
	KnxTunnel& operator=(const KnxTunnel&) = delete;

	/// Sets the gateway (nullptr or an empty address: none) and the NAT mode. A connection to the
	/// previous gateway is closed (DISCONNECT_REQUEST); if started, the tunnel connects to the new
	/// one at once.
	void set_gateway(const GatewayAddress* gateway, bool nat);

	/// Starts the tunnel: it connects as soon as a gateway is set and the network is up.
	void start();

	/// Closes the connection gracefully (DISCONNECT_REQUEST) and stays down until start().
	void stop();

	/// Queues `frame` (an L_Data.req) for sending. Returns a token (never 0) that the listeners'
	/// tunnel_sent() reports with the outcome; 0 when refused (logged): the tunnel is not up, the
	/// queue is full, or the frame does not encode.
	uint32_t send(const LData& frame);

	/// Registers a listener; false (logged) when kListenerCapacity are registered.
	bool add_listener(KnxTunnelListener* listener);

	TunnelState state() const { return state_; }
	bool is_up() const { return state_ == TunnelState::up; }
	bool started() const { return started_; }
	/// Why the tunnel is in its state ("" if never set).
	const char* reason() const { return reason_; }
	const GatewayAddress& gateway() const { return gateway_; }
	bool nat() const { return nat_; }
	/// While up: the channel and the individual address the gateway assigned.
	uint8_t channel() const { return channel_; }
	IndividualAddress address() const { return address_; }
	/// The gateway's control and data endpoints of the current or last attempt.
	ip::Endpoint control_endpoint() const { return control_; }
	ip::Endpoint data_endpoint() const { return data_; }
	/// Frames waiting or in flight.
	size_t queued() const { return queue_count_; }
	/// Monotonic time of the next connection attempt, EventLoop::kNever if none is scheduled.
	uint64_t next_attempt_at() const { return retry_at_; }
	const Backoff& backoff() const { return backoff_; }
	/// Connection attempts since start.
	uint32_t attempts() const { return attempts_; }

	// ConnectivityListener
	void connectivity_changed(const Connectivity& connectivity, LinkState previous) override;

	// UdpReceiver
	void udp_received(const uint8_t* data, size_t length, const Ipv4Endpoint& from) override;
	void udp_error(const char* reason) override;
	void udp_resolved(bool ok, uint32_t address, const char* error) override;

private:
	enum class Phase : uint8_t {
		idle,       ///< Down; maybe a retry scheduled.
		resolving,  ///< Waiting for the host name's address.
		connecting, ///< CONNECT_REQUEST sent.
		up,
	};

	struct Pending {
		LData frame;
		uint32_t token;
	};

	bool can_connect(const char** why) const;
	/// Schedules an attempt now if nothing runs and every condition holds; logs what it waits for.
	void evaluate();
	void schedule_attempt(uint32_t delay_ms);
	void attempt();
	void connect_to(uint32_t address);
	void retry_due();
	void connect_timeout();
	void heartbeat_due();
	void heartbeat_timeout();
	void send_heartbeat();
	void ack_timeout();
	void confirm_timeout();
	void send_head();
	void pump();
	void complete(SendOutcome outcome, const LData* confirmation);
	void flush_queue();
	void broken(const char* reason);
	void fail(const char* reason, bool retry);
	void teardown(const char* reason, bool send_disconnect, bool retry, LogLevel level);
	void cancel_timers();
	bool transmit(const ip::Endpoint& to, const ip::FrameBuffer& frame, const char* what);
	void handle_connect_response(const uint8_t* data, size_t length, const Ipv4Endpoint& from);
	void handle_connectionstate_response(const uint8_t* data, size_t length);
	void handle_disconnect_request(const uint8_t* data, size_t length);
	void handle_tunneling_request(const uint8_t* data, size_t length);
	void handle_tunneling_ack(const uint8_t* data, size_t length);
	void handle_confirmation(const LData& frame);
	void set_state(TunnelState state, const char* reason);
	bool refuse_notifying(const char* what);

	EventLoop& loop_;
	const Clock& clock_;
	Connectivity& connectivity_;
	UdpSocket& socket_;
	Backoff backoff_;
	GatewayAddress gateway_;
	bool nat_ = false;
	bool started_ = false;
	bool registered_ = false;
	bool notifying_ = false;
	TunnelState state_ = TunnelState::down;
	Phase phase_ = Phase::idle;
	char reason_[kMaxReason] = {};
	const char* waiting_ = nullptr; ///< The last "waiting for" reason logged.

	ip::Endpoint control_;
	ip::Endpoint data_;
	ip::Endpoint local_;
	uint8_t channel_ = 0;
	IndividualAddress address_;
	uint8_t send_sequence_ = 0;
	uint8_t receive_sequence_ = 0;
	uint32_t heartbeat_misses_ = 0;
	bool heartbeat_outstanding_ = false;
	uint32_t attempts_ = 0;
	uint64_t retry_at_ = EventLoop::kNever;

	TimerHandle retry_timer_;
	TimerHandle connect_timer_;
	TimerHandle heartbeat_timer_;
	TimerHandle heartbeat_timeout_timer_;
	TimerHandle ack_timer_;
	TimerHandle confirm_timer_;

	Pending queue_[kQueueCapacity];
	size_t queue_head_ = 0;
	size_t queue_count_ = 0;
	bool awaiting_ack_ = false;
	bool awaiting_confirm_ = false;
	bool repeated_ = false;
	/// An L_Data.con for the head that arrived before its TUNNELING_ACK.
	bool early_confirm_ = false;
	LData early_frame_;
	uint32_t next_token_ = 1;

	KnxTunnelListener* listeners_[kListenerCapacity] = {};
	size_t listener_count_ = 0;
};

} // namespace knx
} // namespace hc

#endif // HC_KNX_TUNNEL_H
