/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_BUS_SESSION_H
#define HC_BUS_SESSION_H

#include <cstddef>
#include <cstdint>

#include "hc/event_loop.h"
#include "hc/json.h"
#include "hc/knx/tunnel.h"
#include "hc/platform/clock.h"
#include "hc/platform/websocket_client.h"

namespace hc {

/// The bus session of the E-Home connection (homecontrol #85, spec/api.md "Bus session"): E-Home
/// programs devices through the controller's KNX tunnel. The controller forwards cEMI frames in both
/// directions, like a remote tunnel; transport connections, sequence numbers, acknowledgements and
/// load procedures run in E-Home.
///
/// - `open` (one session at a time) answers the tunnel's individual address and the longest frame
///   forwarded; it ends by `close`, by the end of the connection (connection_ended()), when the
///   tunnel goes down, and after kIdleMs without a frame in either direction.
/// - `send` takes an L_Data.req (to an individual address or a broadcast) as hex digits, queues it on
///   the tunnel as it is (KnxTunnel::send_raw(), beside the controller's own telegrams) and answers
///   `sent` with the outcome the tunnel reports, or `refused` at once.
/// - Every L_Data.ind to the tunnel's individual address or a broadcast goes to E-Home in `frames`
///   messages, in the order received; group telegrams take their usual way only (datapoints, rules,
///   monitor).
///
/// The session's memory (kSlots frames for the tunnel and the outbox of messages to E-Home, about
/// 3.4 KB) is taken from the heap at `open` and given back when the session has ended and the tunnel
/// reported the last of its frames, so a controller without a session pays nothing. Messages leave
/// in the order the controller saw what they report; they wait in the outbox while the socket's send
/// queue has no room. Every session is logged (tag "hc.ehome"): its begin, its end with the reason
/// and the frames in both directions; every refusal with its reason.
///
/// Threading: the event loop's thread.
class BusSession : public knx::KnxTunnelListener {
public:
	/// A session without a frame in either direction for this long ends.
	static constexpr uint32_t kIdleMs = 60000;
	/// Frames of E-Home's on the tunnel at a time (a further `send` is refused `bus.queue_full`).
	static constexpr size_t kSlots = 4;
	/// Messages waiting for room in the socket's send queue.
	static constexpr size_t kOutbox = 2048;
	/// What the outbox keeps free of indications for the answers to E-Home's messages.
	static constexpr size_t kAnswerReserve = 512;
	/// The longest `ref` echoed (its JSON text).
	static constexpr size_t kMaxRef = 40;

	BusSession(EventLoop& loop, const Clock& clock, WebSocketClient& client);
	~BusSession();

	BusSession(const BusSession&) = delete;
	BusSession& operator=(const BusSession&) = delete;

	/// The controller's KNX tunnel (nullptr: none; `open` is then refused `bus.knx_unavailable`).
	/// Registers as its listener; call once, before the first message.
	void set_tunnel(knx::KnxTunnel* tunnel);

	/// A `{"type": "bus", ...}` message of E-Home's (the connection is authenticated).
	void message(const JsonSpan& message);

	/// The connection to E-Home ended: an open session ends without a message, the outbox is dropped.
	void connection_ended();

	/// The link's tick: the idle end, and the outbox again when the socket could not take it.
	void tick();

	/// The socket's send queue has room again.
	void writable();

	/// The `bus` member of the E-Home status: `{"open": false}`, or the open session.
	void write_status(JsonWriter& json) const;

	bool is_open() const { return open_; }
	/// While open: the frames E-Home sent (taken by the tunnel) and the indications forwarded.
	uint32_t sent() const { return sent_; }
	uint32_t received() const { return received_; }
	/// Whether the session's memory is held (open, or frames of an ended session still on the tunnel).
	bool holds_memory() const { return block_ != nullptr; }

	// knx::KnxTunnelListener
	void tunnel_state_changed(const knx::KnxTunnel& tunnel, knx::TunnelState previous) override;
	void tunnel_raw_frame(const knx::KnxTunnel& tunnel, const uint8_t* cemi, size_t length) override;
	void tunnel_raw_sent(const knx::KnxTunnel& tunnel, uint32_t token, knx::SendOutcome outcome) override;

private:
	struct Slot {
		knx::RawFrame frame;
		uint32_t token = 0;
		uint32_t generation = 0; ///< The session that sent it.
		bool busy = false;
		char ref[kMaxRef] = {};  ///< The send's `ref` (JSON text).
	};

	/// The session's memory, on the heap from `open` until it is no longer needed.
	struct Block {
		Slot slots[kSlots];
		/// Records of [kind, length high, length low, octets...]: a whole message (text) or an
		/// indication (its cEMI octets, batched into a `frames` message when sent).
		uint8_t outbox[kOutbox];
		size_t used = 0;
	};

	void open(const char* ref);
	void close(const char* ref);
	void send(const JsonSpan& message, const char* ref);
	/// Ends an open session: the `closed` message (unless the connection ended) and the log line.
	void end(const char* code, const char* reason, const char* ref, bool tell);
	void refuse(const char* op, const char* ref, const char* code, const char* error);
	/// Appends a whole message to the outbox; false when it does not fit (the session then ends).
	bool queue_text(const char* text, size_t length);
	bool queue_record(uint8_t kind, const uint8_t* data, size_t length);
	void schedule_flush();
	void flush();
	/// Frees the block when no session is open and no frame of it is on the tunnel.
	void release_if_unused();
	void drop_outbox();

	EventLoop& loop_;
	const Clock& clock_;
	WebSocketClient& client_;
	knx::KnxTunnel* tunnel_ = nullptr;
	Block* block_ = nullptr;
	bool open_ = false;
	bool flush_posted_ = false;
	uint32_t generation_ = 0;
	uint64_t opened_ms_ = 0;
	uint64_t activity_ms_ = 0;
	uint32_t sent_ = 0;
	uint32_t received_ = 0;
	uint32_t dropped_ = 0;        ///< Indications the outbox had no room for, since the last `frames`.
	uint32_t dropped_total_ = 0;  ///< ... in the session.
	char address_[knx::IndividualAddress::kMaxText] = {};
	uint16_t address_raw_ = 0;
};

} // namespace hc

#endif // HC_BUS_SESSION_H
