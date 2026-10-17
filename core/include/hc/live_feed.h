/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_LIVE_FEED_H
#define HC_LIVE_FEED_H

#include <cstddef>
#include <cstdint>

#include "hc/auth.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/monitor_feed.h"
#include "hc/web_api.h"

namespace hc {

/// The platform's side of the WebSocket connections: sends one complete text message to a client,
/// closes a client. Called on the event loop's thread; must not block. `client` is the platform's
/// handle the feed was given in LiveFeed::open().
class WsTransport {
public:
	/// Queues a text message. Returns false if it cannot take it now (send buffers exhausted, the
	/// client is gone): the feed then resynchronizes the client later (LiveFeed::writable()).
	virtual bool ws_send(uint32_t client, const char* text, size_t length) = 0;

	/// Sends a close frame with `code` and `reason` and closes the connection. The platform reports
	/// LiveFeed::closed() afterwards as for any other close.
	virtual void ws_close(uint32_t client, uint16_t code, const char* reason) = 0;

protected:
	~WsTransport() = default;
};

/// The WebSocket feed at /ws (spec/api.md, "WebSocket"): at most kMaxClients connections, each
/// authenticated by the session cookie of the upgrade request or by a bearer token in its first
/// message. A subscribed client receives a snapshot of all datapoints (in chunks), then every
/// update, batched per event-loop turn: the listener collects the ids of updated datapoints (each
/// once) and a posted flush sends one {"type":"datapoints"} message with their current values. The
/// status is pushed when one of its discrete fields changes and as a heartbeat every
/// kHeartbeatMs. A client that misses a message (no send buffer) gets a full snapshot again.
///
/// The monitor stream (spec/api.md, "Monitor"): the feed owns the MonitorFeed and attaches it to
/// the image service's sources (WebApi::image()) at start() and after every model change. A client
/// that sent {"type":"monitor","subscribe":true} gets the events from its cursor on, batched per
/// event-loop turn into messages of at most kMessageSize; a client that falls behind the ring gets a
/// gap marker, never a queue. Monitor messages that cannot be sent wait for writable() or the next
/// tick (they do not trigger a datapoint resync).
///
/// Threading: the event loop's thread; the platform calls open/message/closed/writable there.
class LiveFeed : public DatapointListener {
public:
	static constexpr size_t kMaxClients = 4;
	/// Distinct datapoints collected per loop turn before an early flush.
	static constexpr size_t kMaxPending = 32;
	/// Largest outgoing message.
	static constexpr size_t kMessageSize = 1400;
	/// Status check period; a heartbeat status goes out every kHeartbeatMs.
	static constexpr uint32_t kTickMs = 1000;
	static constexpr uint32_t kHeartbeatMs = 10000;
	/// A client that has not authenticated within this time is closed.
	static constexpr uint32_t kAuthTimeoutMs = 10000;
	/// Snapshot chunks sent per client and pump call (more follow in the next loop turn).
	static constexpr size_t kChunksPerPump = 4;

	LiveFeed(WebApi& api, EventLoop& loop, WsTransport& transport);

	/// Registers with the model and starts the status timer.
	bool start();

	/// A client completed the WebSocket handshake. `cookie` (Cookie header of the upgrade request)
	/// may authenticate it at once; `origin` and `host` (headers, may be nullptr) must match when
	/// both are given (cross-site WebSocket hijacking). Returns false when the client is refused
	/// (too many clients, foreign origin): the feed has sent the reason and closed it.
	bool open(uint32_t client, const char* cookie, const char* origin, const char* host);

	/// A complete text message from a client.
	void message(uint32_t client, const char* text, size_t length);

	/// The connection is gone (closed by either side).
	void closed(uint32_t client);

	/// The transport can take messages again: resume snapshots.
	void writable();

	/// The datapoint model was replaced (a runtime image swap): ids changed, so every subscribed
	/// client gets a full snapshot again (from id 0, which tells the UI to drop what it knew) and
	/// the status at once; value updates queued for the old model are dropped.
	void resync();

	size_t client_count() const;

	void datapoint_updated(const DatapointUpdate& update) override;

	/// The monitor stream (GET /api/monitor reads it through WebApi).
	MonitorFeed& monitor() { return monitor_; }

	/// Whether `origin` ("http://host[:port]") names the same host as `host` ("host[:port]"),
	/// ignoring the ports (the POSIX build serves /ws on its own port).
	static bool same_origin(const char* origin, const char* host);

private:
	enum class Principal : uint8_t { none, session, token };

	struct Client {
		bool used = false;
		uint32_t handle = 0;
		Principal principal = Principal::none;
		SessionRef session;
		int token_slot = -1;
		uint8_t token_hash[Sha256::kDigestSize] = {};
		bool subscribed = false;
		size_t snapshot_next = 0; ///< Next datapoint id of the snapshot; SIZE_MAX when none runs.
		uint64_t opened_ms = 0;
		bool monitor = false;      ///< Subscribed to the monitor stream.
		uint32_t monitor_next = 0; ///< The next monitor event it gets.
	};

	Client* find(uint32_t handle);
	void send(Client& client, const char* text, size_t length);
	void send_error(Client& client, const char* code, const char* text);
	void refuse_and_close(Client& client, uint16_t ws_code, const char* code, const char* text);
	void authenticate_token(Client& client, const JsonSpan& message);
	void command(Client& client, const JsonSpan& message);
	void subscribe(Client& client);
	void monitor_message(Client& client, const JsonSpan& message);
	void monitor_flush();
	void schedule_monitor_flush();
	void attach_monitor();
	void send_status(Client* only);
	void tick();
	void flush();
	void pump();
	void schedule_pump();
	bool authenticated(const Client& client) const { return client.principal != Principal::none; }
	bool still_authorized(Client& client);

	struct StatusSignature {
		uint8_t link = 0;
		uint8_t mode = 0;
		uint8_t led = 0;
		bool synchronized = false;
		bool improv = false;
		bool mdns = false;
		uint32_t ip_hash = 0;
		uint32_t sessions = 0;
		uint32_t clients = 0;
		uint32_t datapoints = 0;
		bool operator!=(const StatusSignature& o) const {
			return link != o.link || mode != o.mode || led != o.led || synchronized != o.synchronized ||
				improv != o.improv || mdns != o.mdns || ip_hash != o.ip_hash || sessions != o.sessions ||
				clients != o.clients || datapoints != o.datapoints;
		}
	};
	StatusSignature signature();

	WebApi& api_;
	EventLoop& loop_;
	WsTransport& transport_;
	Client clients_[kMaxClients];
	uint16_t pending_[kMaxPending] = {};
	size_t pending_count_ = 0;
	bool flush_posted_ = false;
	bool pump_posted_ = false;
	StatusSignature last_signature_;
	uint64_t last_status_ms_ = 0;
	char out_[kMessageSize] = {};
	MonitorFeed monitor_;
	bool monitor_posted_ = false;
};

} // namespace hc

#endif // HC_LIVE_FEED_H
