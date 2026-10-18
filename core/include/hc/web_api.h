/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_WEB_API_H
#define HC_WEB_API_H

#include <cstddef>
#include <cstdint>

#include "hc/auth.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/json.h"
#include "hc/log_ring.h"
#include "hc/platform/blob_store.h"
#include "hc/platform/web_host.h"
#include "hc/zigbee/adapter.h"

namespace hc {

class EhomeLink;
class ExternalValues;
class LanguagePacks;
class Mqtt;
class LiveFeed;

namespace knx {
class KnxTunnel;
class TelegramLog;
}

namespace image {
class ImageService;
}

enum class HttpMethod : uint8_t { get, post, put, del, other };

/// A password hash the platform computed for the API (the second pass of a login or onboarding).
struct PasswordHash {
	HashParams params;
	uint8_t key[Auth::kKeySize] = {};
};

/// One HTTP request under /api/ as the platform's server hands it to the core. The strings are
/// the header values (nullptr when the header is absent); everything stays valid during handle().
struct WebRequest {
	HttpMethod method = HttpMethod::get;
	const char* path = "";            ///< Without the query, e.g. "/api/status".
	const char* query = "";           ///< After the '?', "" if none.
	const char* cookie = nullptr;     ///< Cookie
	const char* authorization = nullptr; ///< Authorization
	const char* requested_with = nullptr; ///< X-Requested-With
	const char* source = "";          ///< The client's address (rate limiting, log).
	const char* body = nullptr;
	size_t body_length = 0;
	/// Set by the platform on the second pass, after it computed the hash WebResponse asked for.
	const PasswordHash* hash = nullptr;
	/// The request came over the E-Home connection (doc/ehome.md): the principal "E-Home", with a
	/// token's rights minus what WebApi::ehome_forbidden() names. Only EhomeLink sets it; the HTTP
	/// servers never do.
	bool ehome = false;
};

/// What the platform sends back. The body is written into the platform's buffer.
struct WebResponse {
	WebResponse(char* buffer, size_t capacity) : body(buffer), capacity(capacity) {}

	int status = 200;
	const char* content_type = "application/json";
	/// A Set-Cookie header value, "" for none.
	char set_cookie[128] = {};
	/// Retry-After in seconds, 0 for none.
	uint32_t retry_after_s = 0;
	char* body;
	size_t capacity;
	size_t length = 0;

	/// The body is the stored runtime image (GET /api/image), too large for the buffer: the platform
	/// sends `stream_length` bytes, fetching them in pieces with WebApi::read_stream(stream_token, ...)
	/// on the event loop. `body` is empty then.
	bool stream = false;
	size_t stream_length = 0;
	uint32_t stream_token = 0;

	/// The answer waits for another service (the E-Home enrollment): the platform calls
	/// WebApi::poll_deferred() on the event loop every kDeferredPollMs until it returns true, then
	/// sends this response. The core bounds the wait itself (EhomeLink::kEnrollTimeoutMs).
	bool deferred = false;

	/// The request needs a password hash: the platform derives `hash_params` from the password
	/// (Crypto::pbkdf2_sha256, off the event loop) and calls handle() again with WebRequest::hash.
	bool needs_hash = false;
	HashParams hash_params;
	char password[Auth::kMaxPassword + 1] = {};
	size_t password_length = 0;
};

/// The JSON API under /api/ (spec/api.md): authentication, status, datapoints, the log, the
/// settings the UI may change, the runtime image, and what the dashboard reads of it (the model of
/// rooms and functions, the history, the rules' diagnostics and adjustable parameters, the
/// schedules' switches); and what an installed device's owner needs of the serial console (issue
/// #38): restart, factory reset, the update, log levels, the core dump, the API tokens, the WiFi
/// credentials, the Zigbee coordinator (pairing), a manual fetch of an external source, the language
/// pack sync and the MQTT discovery. What only the platform can do goes through WebHost's hooks; a
/// platform that cannot answers 501 "system.unavailable" with the reason. Pure core: the ESP and POSIX servers parse HTTP, call handle() on
/// the event loop's thread and send what it answers. Every refusal is a JSON object
/// {"error": "<English reason>", "code": "<key>"} with a 4xx/5xx status.
///
/// Access: GET /api/auth/session, POST /api/auth/onboard, /login and /logout are open; everything
/// else needs a session cookie or a bearer token (401 otherwise; 403 with
/// "auth.onboarding_required" while the device has no password). State-changing requests (POST,
/// PUT, DELETE) without a bearer token must carry `X-Requested-With: HomeControl` (CSRF: a
/// cross-site form cannot set it).
class WebApi : public zigbee::ZigbeeEventListener {
public:
	/// Datapoints per page of GET /api/datapoints.
	static constexpr size_t kPageSize = 50;
	/// Most datapoints in one GET /api/history, and most points over all of them.
	static constexpr size_t kMaxHistoryIds = 8;
	static constexpr size_t kMaxHistoryPoints = 160;
	/// Largest request body accepted (the platforms refuse longer ones with 413).
	static constexpr size_t kMaxBody = 1024;
	static constexpr const char* kCsrfHeaderValue = "HomeControl";
	/// How long a restart, a factory reset or new WiFi credentials wait after the answer, so that it
	/// reaches the client before the connection goes.
	static constexpr uint32_t kActionDelayMs = 750;
	/// Zigbee devices per page of GET /api/zigbee.
	static constexpr size_t kZigbeePageSize = 16;
	/// How often a platform polls a deferred response (WebResponse::deferred), and the longest it
	/// waits before it gives up (504 api.timeout; the core answers before).
	static constexpr uint32_t kDeferredPollMs = 100;
	static constexpr uint32_t kDeferredLimitMs = 60000;

	WebApi(Controller& controller, DatapointModel& model, Auth& auth, WebHost& host);

	/// The live feed, for its client count in /api/status.
	void set_feed(LiveFeed* feed) { feed_ = feed; }

	/// The language packs (doc/i18n.md): a changed ui.lang starts their sync; GET /api/settings
	/// reports their state as "i18n". Optional (the POSIX build serves the bundled packs only).
	void set_language_packs(LanguagePacks* packs) { packs_ = packs; }
	/// The MQTT client (doc/mqtt.md): the settings mqtt.user and mqtt.password reconnect it.
	void set_mqtt(Mqtt* mqtt) { mqtt_ = mqtt; }
	/// The KNX tunnel and its telegram log: the "knx" member of /api/status; the settings knx.gateway
	/// and knx.nat are handed to the running tunnel (without one they are only stored).
	void set_knx(knx::KnxTunnel* tunnel, const knx::TelegramLog* telegrams) {
		knx_ = tunnel;
		telegrams_ = telegrams;
	}
	/// The Zigbee coordinator behind /api/zigbee (pairing, spec/api.md "Zigbee"); its events reach the
	/// live feed. nullptr with the reason when the platform has none (the routes answer 501).
	void set_zigbee(zigbee::ZigbeeAdapter* adapter, const char* unavailable_reason = nullptr);
	zigbee::ZigbeeAdapter* zigbee() const { return zigbee_; }
	/// The external values (doc/external.md): POST /api/external/<id>/fetch, the "external" status.
	void set_external(ExternalValues* external) { external_ = external; }
	/// The blob store (doc/persistence.md): the file count and bytes of the "storage" status.
	void set_blob_store(const BlobStore* blobs) { blobs_ = blobs; }
	/// The E-Home connection (doc/ehome.md): /api/ehome, the setting ehome.enabled, the "ehome"
	/// status. Set by EhomeLink::start(); without one /api/ehome answers 501.
	void set_ehome(EhomeLink* link) { ehome_ = link; }
	EhomeLink* ehome() const { return ehome_; }
	/// The E-Home state for the live feed's status signature (0 without a link).
	uint8_t ehome_signature() const;

	/// Whether the E-Home principal may not make `request` (spec/api.md "E-Home connection"): the
	/// authentication routes (onboarding, login, the password), WiFi, the factory reset, the firmware
	/// upload and its source (update.url), the API tokens, the enrollment. With the reason.
	static bool ehome_forbidden(const WebRequest& request, const char** reason);

	/// A deferred response (WebResponse::deferred): true when `response` holds the answer.
	bool poll_deferred(WebResponse& response);

	/// The runtime image service: /api/image and the "image" member of /api/status (doc/image.md).
	/// The runtime image behind /api/image and the status; its changes resync the live feed.
	void set_image(image::ImageService* image);
	image::ImageService* image() const { return image_; }

	/// The largest body a request may carry: kMaxBody, the image size limit for PUT /api/image.
	/// The platforms refuse longer bodies with 413 before they read them.
	static size_t body_limit(HttpMethod method, const char* path);

	/// A piece of the stored image for a streamed response (WebResponse::stream), on the loop. False
	/// when the stored image changed since the response began (the platform aborts the transfer).
	bool read_stream(uint32_t token, size_t offset, char* buffer, size_t size, size_t& length);

	void handle(const WebRequest& request, WebResponse& response);

	/// The members of the status object (also the WebSocket's status message).
	void write_status(JsonWriter& json);
	/// The members GET /api/status adds to write_status() (the WebSocket's status message has no
	/// room for them): zigbee, storage, knx, external, lang, mqtt.
	void write_status_details(JsonWriter& json);

	// zigbee::ZigbeeEventListener
	void zigbee_event(const zigbee::ZigbeeEvent& event) override;

	/// One datapoint with its metadata ({"id","name","type","flags","writable","value","quality",
	/// "updated_ms"}).
	static void write_datapoint(JsonWriter& json, const Datapoint& datapoint);

	/// Writes {"error": text, "code": code} and the status.
	static void refuse(WebResponse& response, int status, const char* code, const char* text);

	/// Validates and applies one of the UI-editable settings (tz, ntp.server, update.url, ui.lang,
	/// wifi.ps, mqtt.user, mqtt.password, knx.gateway, knx.nat). On refusal returns false with the code
	/// and reason.
	bool apply_setting(const char* key, const char* value, const char** code, const char** error);

	Controller& controller() { return controller_; }
	DatapointModel& model() { return model_; }
	Auth& auth() { return auth_; }
	WebHost& host() { return host_; }

private:
	void image_changed();
	void auth_session(const WebRequest& request, WebResponse& response);
	void auth_onboard(const WebRequest& request, WebResponse& response);
	void auth_login(const WebRequest& request, WebResponse& response);
	void auth_logout(const WebRequest& request, WebResponse& response);
	void status(WebResponse& response);
	void datapoints(const WebRequest& request, WebResponse& response);
	void datapoint(uint16_t id, WebResponse& response);
	void command(uint16_t id, const WebRequest& request, WebResponse& response);
	void log(const WebRequest& request, WebResponse& response);
	void settings(WebResponse& response);
	void put_setting(const char* key, const WebRequest& request, WebResponse& response);
	void coredump(WebResponse& response);
	void image(const WebRequest& request, WebResponse& response);
	void ui_model(const WebRequest& request, WebResponse& response);
	void history(const WebRequest& request, WebResponse& response);
	void rules(const WebRequest& request, WebResponse& response);
	void rule_parameter(const char* rule, const char* parameter, const WebRequest& request, WebResponse& response);
	void schedules(const WebRequest& request, WebResponse& response);
	void put_schedule(const char* id, const WebRequest& request, WebResponse& response);
	void write_rule(JsonWriter& json, size_t index);
	// The console's commands (web_api_system.cpp)
	void system(const char* what, const WebRequest& request, WebResponse& response);
	void system_action(SystemAction action, const WebRequest& request, WebResponse& response);
	void system_update(bool force, const WebRequest& request, WebResponse& response);
	void system_upload(const WebRequest& request, WebResponse& response);
	void log_level(const WebRequest& request, WebResponse& response);
	void wifi(const WebRequest& request, WebResponse& response);
	void clear_coredump(WebResponse& response);
	void tokens(const char* rest, const WebRequest& request, WebResponse& response);
	void zigbee(const char* rest, const WebRequest& request, WebResponse& response);
	void zigbee_list(const WebRequest& request, WebResponse& response);
	void zigbee_export(const WebRequest& request, WebResponse& response);
	void external_fetch(const char* rest, const WebRequest& request, WebResponse& response);
	void lang_sync(const WebRequest& request, WebResponse& response);
	void mqtt_discovery(const WebRequest& request, WebResponse& response);
	void diagnostics(const WebRequest& request, WebResponse& response); // the System page (web_api_diagnostics.cpp)
	void action_due();
	bool parse_body(const WebRequest& request, WebResponse& response, JsonSpan& body, bool optional = false);
	void ok(WebResponse& response);
	void write_schedule(JsonWriter& json, size_t index);
	bool read_password(const WebRequest& request, WebResponse& response);
	void set_session_cookie(WebResponse& response, const char* session);
	void finish(WebResponse& response, JsonWriter& json);

	Controller& controller_;
	DatapointModel& model_;
	Auth& auth_;
	WebHost& host_;
	LiveFeed* feed_ = nullptr;
	image::ImageService* image_ = nullptr;
	LanguagePacks* packs_ = nullptr;
	Mqtt* mqtt_ = nullptr;
	knx::KnxTunnel* knx_ = nullptr;
	const knx::TelegramLog* telegrams_ = nullptr;
	zigbee::ZigbeeAdapter* zigbee_ = nullptr;
	const char* zigbee_reason_ = nullptr;
	ExternalValues* external_ = nullptr;
	const BlobStore* blobs_ = nullptr;
	EhomeLink* ehome_ = nullptr;
	/// The deferred system action (restart, factory reset, WiFi reconnect, the restart into the
	/// updater for an upload) and its timer.
	enum class Pending : uint8_t { none, restart, factory_reset, wifi, upload };
	Pending pending_ = Pending::none;
	TimerHandle action_timer_;
	/// A consistent copy of the controller's log ring for GET /api/log.
	LogRing log_copy_;
};

/// The API's error code of a datapoint refusal ("dp.not_writable", ...).
const char* datapoint_status_code(DatapointStatus status);
/// The English reason of a refused command.
const char* datapoint_command_reason(DatapointStatus status);

/// Reads the signed decimal query parameter `name`; false if absent or invalid.
bool query_int(const char* query, const char* name, int64_t& out);

/// Reads the unsigned decimal query parameter `name` ("from=50&x=1"); false if absent or invalid.
bool query_uint(const char* query, const char* name, uint32_t& out);

} // namespace hc

#endif // HC_WEB_API_H
