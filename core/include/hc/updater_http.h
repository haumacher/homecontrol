/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_UPDATER_HTTP_H
#define HC_UPDATER_HTTP_H

#include <cstddef>
#include <cstdint>

namespace hc {

// What the OTA updater's web server decides per request (doc/ota.md "Upload from the browser",
// spec/api.md "The updater's own routes"): the headers of its answers, the CORS preflight and the
// checks of PUT /firmware before the body is read. The updater (platform/esp/updater) only executes
// these decisions with esp_http_server, so they are tested on the host.
//
// CORS (#80): the application's Settings page may be opened under any address (the mDNS name, a DNS
// entry of the router), while the updater answers only at the device's IP address. The page then
// talks to the updater across origins: GET /status and PUT /firmware carry
// Access-Control-Allow-Origin with the request's Origin, and OPTIONS /firmware answers the
// preflight. The upload token authorizes, not the origin: every origin is allowed, and the token
// check is the same from every one.

/// The routes of the updater's web server.
enum class UpdaterRoute : uint8_t {
	page,     ///< GET /: the updater's own page.
	status,   ///< GET /status.
	firmware, ///< PUT /firmware (and its preflight OPTIONS /firmware).
	abort,    ///< POST /abort.
};

/// One response header. The value points to a constant or into the request (the Origin): valid while
/// the request is handled.
struct ResponseHeader {
	const char* name = nullptr;
	const char* value = nullptr;
};

/// The headers of one updater answer, beyond its Content-Type.
struct UpdaterHeaders {
	static constexpr size_t kMax = 5;
	ResponseHeader items[kMax];
	size_t count = 0;

	/// The value of the header `name` (ASCII case ignored), nullptr if it is not there.
	const char* find(const char* name) const;
};

/// The longest Origin the updater echoes (a scheme, a DNS name of up to 253 characters, a port).
constexpr size_t kMaxCorsOrigin = 270;

/// Whether `origin` (a request's Origin header) can be echoed in Access-Control-Allow-Origin: 1 to
/// kMaxCorsOrigin visible ASCII characters (no space, no control character). "null" (a page from a
/// file) is one. nullptr or "" is none.
bool cors_origin_echoable(const char* origin);

/// The headers every answer on `route` carries, whatever its status: `Cache-Control: no-store`, and
/// on /status and /firmware `Vary: Origin` and, for a request with an echoable `origin`,
/// `Access-Control-Allow-Origin: <origin>`. They do not depend on the status, so a refusal carries
/// them like the success, and a page of another origin can read the reason.
UpdaterHeaders updater_response_headers(UpdaterRoute route, const char* origin);

/// The answer to a CORS preflight.
struct UpdaterPreflight {
	const char* status = nullptr; ///< "204 No Content".
	UpdaterHeaders headers;
};

/// The answer to `OPTIONS /firmware`, for every origin: 204 with the headers of
/// updater_response_headers(UpdaterRoute::firmware, origin), `Access-Control-Allow-Methods: PUT`
/// and `Access-Control-Allow-Headers: Authorization, Content-Type`. Only /firmware is preflighted:
/// GET /status is a simple request.
UpdaterPreflight updater_preflight(const char* origin);

/// What `PUT /firmware` finds before it reads the body.
struct FirmwareRequest {
	bool installing = false;              ///< An upload is being written or verified.
	bool installed = false;               ///< An image is installed; the device restarts.
	const char* token = "";               ///< The stored upload token, "" without one.
	const char* authorization = nullptr;  ///< The Authorization header, nullptr if missing or too long.
	bool button_window = false;           ///< The button window is open (doc/ota.md "Recovery with the button").
	/// ota_0 holds a bootable application. Read only for a request that is not busy and not
	/// authorized by the token (the updater inspects ota_0 only then: a verify of the whole image).
	bool app_valid = true;
	bool test_button_window = false;      ///< The updater's test variant: the window counts with a valid ota_0.
	size_t content_length = 0;            ///< The request's Content-Length, 0 without one.
	size_t partition_size = 0;            ///< The size of ota_0.
};

/// Whether PUT /firmware goes on to read the image, and if not, the refusal.
struct FirmwareDecision {
	bool accept = false;
	bool by_button = false;       ///< Accepted without the token, by the button window.
	const char* status = nullptr; ///< The refusal's HTTP status line ("401 Unauthorized"), nullptr if accepted.
	const char* code = nullptr;   ///< The refusal's code ("updater.unauthorized").
	char why[192] = {};           ///< The refusal's reason.
};

/// The checks of PUT /firmware, in this order: 409 `updater.busy` (an upload runs or is installed),
/// 401 `updater.unauthorized` (no `Bearer <token>` matching the stored token, hc::upload_authorized,
/// and no open button window while ota_0 holds no application), 411 `updater.length_required`, 413
/// `updater.too_large`. The origin plays no part.
FirmwareDecision decide_firmware_request(const FirmwareRequest& request);

} // namespace hc

#endif // HC_UPDATER_HTTP_H
