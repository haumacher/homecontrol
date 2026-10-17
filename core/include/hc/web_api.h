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
#include "hc/platform/web_host.h"

namespace hc {

class LiveFeed;

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
/// schedules' switches). Pure core: the ESP and POSIX servers parse HTTP, call handle() on
/// the event loop's thread and send what it answers. Every refusal is a JSON object
/// {"error": "<English reason>", "code": "<key>"} with a 4xx/5xx status.
///
/// Access: GET /api/auth/session, POST /api/auth/onboard, /login and /logout are open; everything
/// else needs a session cookie or a bearer token (401 otherwise; 403 with
/// "auth.onboarding_required" while the device has no password). State-changing requests (POST,
/// PUT, DELETE) without a bearer token must carry `X-Requested-With: HomeControl` (CSRF: a
/// cross-site form cannot set it).
class WebApi {
public:
	/// Datapoints per page of GET /api/datapoints.
	static constexpr size_t kPageSize = 50;
	/// Most datapoints in one GET /api/history, and most points over all of them.
	static constexpr size_t kMaxHistoryIds = 8;
	static constexpr size_t kMaxHistoryPoints = 160;
	/// Largest request body accepted (the platforms refuse longer ones with 413).
	static constexpr size_t kMaxBody = 1024;
	static constexpr const char* kCsrfHeaderValue = "HomeControl";

	WebApi(Controller& controller, DatapointModel& model, Auth& auth, WebHost& host);

	/// The live feed, for its client count in /api/status.
	void set_feed(LiveFeed* feed) { feed_ = feed; }

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

	/// One datapoint with its metadata ({"id","name","type","flags","writable","value","quality",
	/// "updated_ms"}).
	static void write_datapoint(JsonWriter& json, const Datapoint& datapoint);

	/// Writes {"error": text, "code": code} and the status.
	static void refuse(WebResponse& response, int status, const char* code, const char* text);

	/// Validates and applies one of the UI-editable settings (tz, ntp.server, update.url). On
	/// refusal returns false with the code and reason.
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
