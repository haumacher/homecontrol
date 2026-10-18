/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_EHOME_LINK_H
#define HC_EHOME_LINK_H

#include <cstddef>
#include <cstdint>

#include "hc/backoff.h"
#include "hc/event_loop.h"
#include "hc/json.h"
#include "hc/live_feed.h"
#include "hc/platform/http_fetcher.h"
#include "hc/platform/settings.h"
#include "hc/platform/websocket_client.h"
#include "hc/web_api.h"

namespace hc {

/// The settings of the E-Home connection (spec/api.md "E-Home connection", doc/ehome.md).
/// Persisted names: never rename.
namespace setting {

constexpr const char* kEhomeUrl = "ehome.url";
constexpr const char* kEhomeId = "ehome.id";
constexpr const char* kEhomeName = "ehome.name";
/// The per-controller credential (64 hex digits): never listed, shown or logged.
constexpr const char* kEhomeSecret = "ehome.secret";
/// The owner's switch (boolean).
constexpr const char* kEhomeEnabled = "ehome.enabled";
/// The socket URL E-Home named at the enrollment (not listed).
constexpr const char* kEhomeSocket = "ehome.socket";

} // namespace setting

/// An E-Home address: `https://host[:port][/path]` (or `http://` for development), at most
/// Settings::kMaxStringLength bytes, no spaces, quotes, query or fragment.
bool ehome_url_valid(const char* url, const char** error = nullptr);

/// The socket URL E-Home answers: `wss://...`, or `ws://...` when the E-Home address is `http://`.
bool ehome_socket_valid(const char* socket, const char* url, const char** error = nullptr);

/// A controller id: 1 to 64 characters of [A-Za-z0-9._-].
bool ehome_id_valid(const char* id, const char** error = nullptr);

/// The secret: exactly 64 hex digits.
bool ehome_secret_valid(const char* secret, const char** error = nullptr);

/// An enrollment code: 1 to 32 characters of [A-Za-z0-9-].
bool ehome_code_valid(const char* code, const char** error = nullptr);

/// The `mac` of the auth message: HMAC-SHA256 with the 32 bytes the secret's hex digits encode as
/// the key over the bytes the nonce's hex digits encode, in lower-case hex (65 bytes with the NUL).
/// False if the secret or the nonce is not hex of the right length (nonce: 16 to 64 bytes' worth,
/// an even number of digits).
bool ehome_mac(const char* secret, const char* nonce, char out[65]);

/// Whether the controller is enrolled: ehome.id, ehome.secret and ehome.socket are stored.
bool ehome_enrolled(const Settings& settings);

/// The state of the connection as the status reports it.
enum class EhomeState : uint8_t {
	off,        ///< Not enrolled, or switched off.
	connecting, ///< Waiting for the network, opening the socket, authenticating.
	connected,  ///< E-Home said `ready`: the feed and the requests run.
	failed,     ///< The last attempt failed (`error`); the backoff retries.
};

/// "off", "connecting", "connected", "failed".
const char* ehome_state_name(EhomeState state);

/// The controller's side of the E-Home connection (issue #41, spec/api.md "E-Home connection",
/// doc/ehome.md): the enrollment through the platform's HttpFetcher, and the outbound WebSocket
/// over the platform's WebSocketClient while the owner's switch `ehome.enabled` is on and the
/// network is up, with the network backoff (1 s .. 60 s) after every failure.
///
/// On the socket it answers E-Home's challenge with the HMAC of the secret, and after `ready`
/// attaches the socket to the LiveFeed as one more client (principal "E-Home": `subscribe`,
/// `command`, `monitor` work as for a browser) and dispatches `request` envelopes into WebApi with
/// WebRequest::ehome set (a token's rights, nothing more), one at a time; a body announced with
/// `length` arrives in binary frames into a heap buffer of that size (freed after the request), a
/// streamed answer (GET /api/image) goes back in binary frames, a piece per loop turn. It pings
/// every kPingMs and closes when the pong does not come within kPongTimeoutMs.
///
/// Every state change is one log line under the tag "hc.ehome"; the secret never reaches the log.
///
/// Threading: the event loop's thread.
class EhomeLink : public WebSocketClientListener, public WsTransport, public FetchReceiver {
public:
	static constexpr uint32_t kTickMs = 1000;
	static constexpr uint32_t kBackoffInitialMs = Backoff::kDefaultInitialMs;
	static constexpr uint32_t kBackoffCapMs = Backoff::kDefaultCapMs;
	/// From connect() to `ready`: the platform's connect, the TLS handshake, the challenge.
	static constexpr uint32_t kHandshakeTimeoutMs = 30000;
	static constexpr uint32_t kPingMs = 30000;
	static constexpr uint32_t kPongTimeoutMs = 10000;
	/// The enrollment's guard: E-Home's answer must arrive within this time.
	static constexpr uint32_t kEnrollTimeoutMs = 30000;
	/// The largest response body sent in a text message (larger ones are refused by WebApi with
	/// 500 api.response_too_large; the paged routes fill their pages to it).
	static constexpr size_t kMaxResponseBody = 4096;
	/// A piece of a streamed answer (binary frames).
	static constexpr size_t kStreamPiece = 2048;
	/// The largest answer of E-Home's enrollment route.
	static constexpr size_t kMaxEnrollAnswer = 2048;
	/// The live feed's client handle of the E-Home socket.
	static constexpr uint32_t kFeedHandle = 0xE4031u;

	EhomeLink(WebApi& api, LiveFeed& feed, WebSocketClient& client, HttpFetcher& fetcher);
	~EhomeLink();

	EhomeLink(const EhomeLink&) = delete;
	EhomeLink& operator=(const EhomeLink&) = delete;

	/// Registers with the API (WebApi::set_ehome) and starts the tick; connects when enrolled and
	/// switched on.
	void start();

	/// ehome.enabled changed (WebApi stores it): connects at once or closes at once.
	void enabled_changed();

	/// The routes /api/ehome (WebApi forwards them): POST /api/ehome/enroll (answered deferred:
	/// WebResponse::deferred, then poll_enroll()), DELETE /api/ehome.
	void handle_route(const char* rest, const WebRequest& request, WebResponse& response);

	/// The deferred answer of the enrollment: false while E-Home has not answered; true when
	/// `response` holds the answer.
	bool poll_enroll(WebResponse& response);

	/// The `ehome` member of the status (its value: an object).
	void write_status(JsonWriter& json) const;

	EhomeState state() const { return state_; }
	bool enrolled() const { return enrolled_; }
	bool enabled() const { return enabled_; }
	const char* error() const { return error_; }
	/// Failures since the last success (the backoff's count).
	uint32_t failures() const { return backoff_.failures(); }

	// WebSocketClientListener
	void ws_client_opened() override;
	void ws_client_text(const char* text, size_t length) override;
	void ws_client_binary(const uint8_t* data, size_t length, bool last) override;
	void ws_client_pong() override;
	void ws_client_closed(const char* reason) override;

	// WsTransport: the live feed's messages to E-Home
	bool ws_send(uint32_t client, const char* text, size_t length) override;
	void ws_close(uint32_t client, uint16_t code, const char* reason) override;

	// FetchReceiver: E-Home's answer to the enrollment
	void fetch_done(const FetchResult& result) override;

private:
	/// Where the socket is: off, waiting for the network, backoff, opening (connect() called),
	/// awaiting the challenge, awaiting ready (auth sent), connected.
	enum class Phase : uint8_t { idle, waiting, backoff, opening, challenge, authenticating, connected };

	void tick();
	void retry();
	void reload();
	void attempt();
	void fail(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
	void stop_socket(uint16_t code, const char* reason);
	void go_idle(const char* why);
	void set_state(EhomeState state);
	void handshake_message(const JsonSpan& message, const JsonSpan& type);
	void request(const JsonSpan& message);
	void dispatch();
	void answer(const char* id, int status, const char* code, const char* text);
	void send_response(WebResponse& response);
	void stream_next();
	void schedule_stream();
	void drop_request();
	void enroll(const WebRequest& request, WebResponse& response);
	void disconnect(WebResponse& response);
	void finish_enroll(int status, const char* code, const char* text);
	void enroll_timeout();
	bool send_text(const char* text, size_t length);

	WebApi& api_;
	LiveFeed& feed_;
	WebSocketClient& client_;
	HttpFetcher& fetcher_;
	EventLoop& loop_;
	Backoff backoff_;
	TimerHandle tick_timer_;
	TimerHandle retry_timer_; ///< Backoff: the next attempt.

	EhomeState state_ = EhomeState::off;
	Phase phase_ = Phase::idle;
	uint64_t since_ms_ = 0;     ///< When state_ began.
	uint64_t phase_ms_ = 0;     ///< When phase_ began (the handshake's timeout).
	uint64_t retry_at_ms_ = 0;  ///< Backoff: the next attempt.
	uint64_t ping_at_ms_ = 0;   ///< Connected: the next ping.
	uint64_t pong_due_ms_ = 0;  ///< A ping is out: the pong is due by then (0: none out).
	char error_[128] = {};
	char installation_[Settings::kMaxStringLength + 1] = {};
	char url_[Settings::kMaxStringLength + 1] = {}; ///< ehome.url
	bool enrolled_ = false; ///< ehome_enrolled(): read at start and on every change.
	bool enabled_ = false;  ///< ehome.enabled
	bool socket_ = false;   ///< A session of the client runs (connect() called, no close since).
	char last_logged_[128] = {}; ///< The last failure logged as a warning (repeats go to info).

	// The request in progress (one at a time).
	bool busy_ = false;
	char request_id_[40] = {};     ///< The id's JSON text, echoed.
	HttpMethod method_ = HttpMethod::get;
	char path_[256] = {};           ///< The path; the query follows its NUL.
	const char* query_ = "";
	char* body_ = nullptr;          ///< A body announced with `length` (heap, length + 1).
	size_t body_length_ = 0;
	size_t body_have_ = 0;
	size_t discard_ = 0;            ///< Binary bytes of a refused request still to be dropped.
	// A streamed answer in progress.
	bool streaming_ = false;
	uint32_t stream_token_ = 0;
	size_t stream_length_ = 0;
	size_t stream_sent_ = 0;
	bool stream_posted_ = false;
	TimerHandle stream_timer_;

	// The enrollment in progress.
	enum class Enroll : uint8_t { none, running, done };
	Enroll enroll_ = Enroll::none;
	TimerHandle enroll_timer_;
	int enroll_status_ = 0;
	char enroll_code_[32] = {};
	char enroll_text_[160] = {};
	char enroll_url_[Settings::kMaxStringLength + 1] = {};

	/// The response envelope: the prefix, the body WebApi writes, the closing brace. On the heap
	/// while a session runs (connect() to its end), so a controller without E-Home pays nothing.
	static constexpr size_t kPrefix = 96;
	static constexpr size_t kOutBytes = kPrefix + kMaxResponseBody + 2;
	char* out_ = nullptr;
	void free_out();
};

} // namespace hc

#endif // HC_EHOME_LINK_H
