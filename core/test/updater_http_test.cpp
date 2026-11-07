/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The updater's request handling (#80, doc/ota.md "Upload from the browser"): CORS on GET /status
// and PUT /firmware, the preflight of PUT /firmware, and the checks of PUT /firmware before the body
// is read. The token authorizes, not the origin.

#include <cstring>
#include <string>

#include "doctest.h"
#include "hc/updater_http.h"

using hc::FirmwareDecision;
using hc::FirmwareRequest;
using hc::UpdaterHeaders;
using hc::UpdaterRoute;

namespace {

constexpr const char* kToken = "0123456789abcdef0123456789abcdef";
constexpr const char* kBearer = "Bearer 0123456789abcdef0123456789abcdef";

/// The origins a Settings page may have: the device's name, its IP (same origin), a DNS entry of the
/// router, another port, a page from a file; and none (curl, a same-origin GET).
const char* const kOrigins[] = {
	"http://homecontrol-f883ec.local",
	"http://192.0.2.10",
	"http://homecontrol.fritz.box",
	"http://192.0.2.10:8080",
	"null",
	nullptr,
};

std::string text(const char* value) {
	return value != nullptr ? value : "<none>";
}

/// The CORS headers of an answer on /status or /firmware for `origin`.
void check_cors(const UpdaterHeaders& headers, const char* origin) {
	CAPTURE(text(origin));
	CHECK(text(headers.find("Cache-Control")) == "no-store");
	CHECK(text(headers.find("Vary")) == "Origin");
	if (origin != nullptr) {
		CHECK(text(headers.find("Access-Control-Allow-Origin")) == origin);
	} else {
		CHECK(headers.find("Access-Control-Allow-Origin") == nullptr);
	}
}

/// A PUT /firmware that passes every check: the stored token presented, a length that fits.
FirmwareRequest valid_request() {
	FirmwareRequest request;
	request.token = kToken;
	request.authorization = kBearer;
	request.content_length = 1500000;
	request.partition_size = 0x2b0000;
	return request;
}

/// The answer PUT /firmware gives for `request` from `origin`: its status and its headers.
struct Answer {
	std::string status;
	std::string code;
	UpdaterHeaders headers;
};

Answer answer(const FirmwareRequest& request, const char* origin) {
	FirmwareDecision decision = hc::decide_firmware_request(request);
	Answer result;
	result.status = decision.accept ? "200 OK" : decision.status;
	result.code = decision.accept ? "" : decision.code;
	result.headers = hc::updater_response_headers(UpdaterRoute::firmware, origin);
	return result;
}

} // namespace

TEST_CASE("UpdaterHttp: the preflight of PUT /firmware allows every origin") {
	for (const char* origin : kOrigins) {
		hc::UpdaterPreflight preflight = hc::updater_preflight(origin);
		CHECK(std::string(preflight.status) == "204 No Content");
		check_cors(preflight.headers, origin);
		CHECK(text(preflight.headers.find("Access-Control-Allow-Methods")) == "PUT");
		CHECK(text(preflight.headers.find("access-control-allow-headers")) == "Authorization, Content-Type");
		CHECK(preflight.headers.count <= UpdaterHeaders::kMax);
	}
}

TEST_CASE("UpdaterHttp: GET /status carries the request's origin") {
	for (const char* origin : kOrigins) {
		check_cors(hc::updater_response_headers(UpdaterRoute::status, origin), origin);
	}
}

TEST_CASE("UpdaterHttp: the page and /abort are same-origin only") {
	for (UpdaterRoute route : {UpdaterRoute::page, UpdaterRoute::abort}) {
		UpdaterHeaders headers = hc::updater_response_headers(route, "http://homecontrol-f883ec.local");
		CHECK(text(headers.find("Cache-Control")) == "no-store");
		CHECK(headers.find("Access-Control-Allow-Origin") == nullptr);
	}
}

TEST_CASE("UpdaterHttp: an origin that cannot be a header value is not echoed") {
	const std::string longest = "http://" + std::string(hc::kMaxCorsOrigin - 7, 'a');
	const std::string longer = longest + "a";
	CHECK(hc::cors_origin_echoable(longest.c_str()));
	CHECK_FALSE(hc::cors_origin_echoable(longer.c_str()));
	CHECK_FALSE(hc::cors_origin_echoable(""));
	CHECK_FALSE(hc::cors_origin_echoable(nullptr));
	CHECK_FALSE(hc::cors_origin_echoable("http://a\r\nSet-Cookie: x=y"));
	CHECK_FALSE(hc::cors_origin_echoable("http://a b"));
	CHECK_FALSE(hc::cors_origin_echoable("http://\xc3\xa4.local"));
	for (const std::string& origin : {longer, std::string("http://a\r\nX: y")}) {
		UpdaterHeaders headers = hc::updater_response_headers(UpdaterRoute::firmware, origin.c_str());
		CHECK(headers.find("Access-Control-Allow-Origin") == nullptr);
		CHECK(text(headers.find("Vary")) == "Origin");
	}
}

TEST_CASE("UpdaterHttp: PUT /firmware with the token is accepted from every origin and carries CORS") {
	for (const char* origin : kOrigins) {
		Answer a = answer(valid_request(), origin);
		CHECK(a.status == "200 OK");
		check_cors(a.headers, origin);
	}
	FirmwareDecision decision = hc::decide_firmware_request(valid_request());
	CHECK(decision.accept);
	CHECK_FALSE(decision.by_button);
	CHECK(decision.status == nullptr);
}

TEST_CASE("UpdaterHttp: a missing or wrong token is refused with 401 from every origin") {
	const char* const kAuthorizations[] = {
		nullptr,                                     // missing (or longer than the updater reads)
		"",                                          // empty
		"Bearer ",                                   // no token
		"Bearer 0123456789abcdef0123456789abcdee",   // wrong in the last digit
		"Bearer 0123456789ABCDEF0123456789ABCDEF",   // upper case: not the token
		"Bearer 0123456789abcdef0123456789abcdef0",  // one digit too many
		"bearer 0123456789abcdef0123456789abcdef",   // the scheme's case
		"Basic MDEyMzQ1Njc4OWFiY2RlZg==",            // another scheme
		"0123456789abcdef0123456789abcdef",          // the token without the scheme
	};
	for (const char* origin : kOrigins) {
		for (const char* authorization : kAuthorizations) {
			CAPTURE(text(authorization));
			FirmwareRequest request = valid_request();
			request.authorization = authorization;
			Answer a = answer(request, origin);
			CHECK(a.status == "401 Unauthorized");
			CHECK(a.code == "updater.unauthorized");
			check_cors(a.headers, origin);
		}
		// No stored token (an updater serving the button window only): the right-looking token is refused.
		FirmwareRequest request = valid_request();
		request.token = "";
		CHECK(answer(request, origin).status == "401 Unauthorized");
		request.token = nullptr;
		CHECK(answer(request, origin).status == "401 Unauthorized");
	}
	FirmwareRequest request = valid_request();
	request.authorization = nullptr;
	FirmwareDecision decision = hc::decide_firmware_request(request);
	CHECK(std::strstr(decision.why, "Settings page") != nullptr);
	CHECK(std::strstr(decision.why, "button") == nullptr); // a valid ota_0: the button opens nothing
	request.app_valid = false;
	decision = hc::decide_firmware_request(request);
	CHECK(std::strstr(decision.why, "hold the device's button for 3 s") != nullptr);
}

TEST_CASE("UpdaterHttp: every refusal of PUT /firmware carries CORS, in the order of the checks") {
	struct Case {
		const char* what;
		FirmwareRequest request;
		const char* status;
		const char* code;
	};
	FirmwareRequest busy = valid_request();
	busy.installing = true;
	FirmwareRequest done = valid_request();
	done.installed = true;
	FirmwareRequest busy_without_token = busy;
	busy_without_token.authorization = nullptr; // busy is decided before the token
	FirmwareRequest unauthorized = valid_request();
	unauthorized.authorization = "Bearer ffffffffffffffffffffffffffffffff";
	FirmwareRequest unauthorized_without_length = unauthorized;
	unauthorized_without_length.content_length = 0; // the token is decided before the length
	FirmwareRequest no_length = valid_request();
	no_length.content_length = 0;
	FirmwareRequest too_large = valid_request();
	too_large.content_length = too_large.partition_size + 1;
	FirmwareRequest just_fits = valid_request();
	just_fits.content_length = just_fits.partition_size;
	const Case kCases[] = {
		{"busy", busy, "409 Conflict", "updater.busy"},
		{"done", done, "409 Conflict", "updater.busy"},
		{"busy before the token", busy_without_token, "409 Conflict", "updater.busy"},
		{"wrong token", unauthorized, "401 Unauthorized", "updater.unauthorized"},
		{"token before the length", unauthorized_without_length, "401 Unauthorized", "updater.unauthorized"},
		{"no length", no_length, "411 Length Required", "updater.length_required"},
		{"too large", too_large, "413 Content Too Large", "updater.too_large"},
		{"just fits", just_fits, "200 OK", ""},
	};
	for (const Case& c : kCases) {
		CAPTURE(c.what);
		for (const char* origin : kOrigins) {
			Answer a = answer(c.request, origin);
			CHECK(a.status == c.status);
			CHECK(a.code == c.code);
			check_cors(a.headers, origin);
		}
	}
	FirmwareDecision decision = hc::decide_firmware_request(too_large);
	CHECK(std::string(decision.why) == "the image has 2818049 bytes, the application partition 2818048");
	CHECK(std::string(hc::decide_firmware_request(done).why) == "the image is installed; the device restarts");
	CHECK(std::string(hc::decide_firmware_request(busy).why) == "an upload is running");
	// The refusals while the image streams in (400 updater.connection, 422 updater.wrong_image and
	// updater.verify_failed, 500 updater.flash) are sent on the same route: the headers above do
	// not depend on the status (updater_response_headers takes none), and the updater sends every
	// answer of /firmware through them.
}

TEST_CASE("UpdaterHttp: the button window authorizes only while ota_0 holds no application") {
	FirmwareRequest request = valid_request();
	request.authorization = nullptr;
	request.button_window = true;
	request.app_valid = true;
	CHECK(answer(request, "http://homecontrol-f883ec.local").status == "401 Unauthorized");
	request.app_valid = false;
	FirmwareDecision decision = hc::decide_firmware_request(request);
	CHECK(decision.accept);
	CHECK(decision.by_button);
	request.app_valid = true;
	request.test_button_window = true; // the test variant counts the window with a valid ota_0
	CHECK(hc::decide_firmware_request(request).by_button);
	// The token wins over the window: an upload with the token is not "by the button".
	request = valid_request();
	request.button_window = true;
	request.app_valid = false;
	decision = hc::decide_firmware_request(request);
	CHECK(decision.accept);
	CHECK_FALSE(decision.by_button);
	// A closed window, no application, no token: refused.
	request.authorization = nullptr;
	request.button_window = false;
	CHECK(answer(request, nullptr).status == "401 Unauthorized");
}
