/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// POST /api/system/update/upload (spec/api.md "System", doc/ota.md "Upload from the browser"): the
// one-time token, the updater's address, the deferred restart and the refusals.

#include <string>

#include "doctest.h"
#include "hc/update_check.h"
#include "hc/version.h"
#include "web_test_support.h"

using hc::HttpMethod;
using hc_test::WebFixture;

namespace {

struct UploadFixture : WebFixture {
	std::string cookie;

	UploadFixture() {
		controller.start();
		controller.connectivity().link_up("192.0.2.10");
		cookie = onboard();
	}

	Result upload(const std::string& authorization = "") {
		return request(HttpMethod::post, "/api/system/update/upload", "", authorization.empty() ? cookie : "",
			authorization, true);
	}

	void advance(uint64_t ms) {
		uint64_t end = fake.clock.now_ms + ms;
		while (fake.clock.now_ms < end) {
			fake.clock.advance(50);
			controller.loop().run_once(fake.clock.now_ms);
		}
	}
};

} // namespace

TEST_CASE("WebUpload: the platform takes the token, the answer names the updater's address, the restart follows") {
	UploadFixture f;
	hc_test::ScopedCapture capture;
	f.host.uploads = true;
	f.host.system_actions = true;
	auto r = f.upload();
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["ok"].boolean);
	CHECK(r.json["url"].text == "http://192.0.2.10/");
	CHECK(r.json["restart_in_ms"].number == hc::WebApi::kActionDelayMs);
	CHECK(r.json["timeout_s"].number == 600);
	const std::string token = r.json["token"].text;
	CHECK(hc::upload_token_valid(token.c_str()));
	REQUIRE(f.host.upload_tokens.size() == 1);
	CHECK(f.host.upload_tokens[0] == token);
	CHECK_FALSE(capture.sink.contains(token.c_str())); // the token is never logged
	CHECK(capture.sink.contains("firmware upload requested over the API from 192.0.2.7"));

	// The answer leaves first; meanwhile everything else that restarts is refused.
	CHECK(f.host.actions.empty());
	auto again = f.upload();
	CHECK(again.status == 409);
	CHECK(again.json["code"].text == "system.busy");
	CHECK(f.request(HttpMethod::post, "/api/system/restart", "", f.cookie).json["code"].text == "system.busy");
	CHECK(f.host.upload_tokens.size() == 1);
	f.advance(hc::WebApi::kActionDelayMs + 100);
	REQUIRE(f.host.actions.size() == 1);
	CHECK(f.host.actions[0] == hc::SystemAction::restart);
	CHECK(capture.sink.contains("restarting into the updater for the firmware upload"));

	// A second request after the window gets a fresh token.
	r = f.upload();
	REQUIRE(r.status == 200);
	CHECK(r.json["token"].text != token);
}

TEST_CASE("WebUpload: a platform without an updater answers 501, a refusal 409 with its reason") {
	UploadFixture f;
	hc_test::ScopedCapture capture;
	auto r = f.upload();
	CHECK(r.status == 501);
	CHECK(r.json["code"].text == "system.unavailable");
	CHECK(r.json["error"].text == "this platform does not update itself (no OTA updater)");
	CHECK(capture.sink.contains("refused (system.unavailable)"));

	f.host.uploads = true;
	f.host.upload_refusal = "the updater in this device predates firmware uploads";
	r = f.upload();
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "update.refused");
	CHECK(r.json["error"].text == "the updater in this device predates firmware uploads");
	f.advance(2000);
	CHECK(f.host.actions.empty()); // nothing restarts after a refusal

	CHECK(f.request(HttpMethod::get, "/api/system/update/upload", "", f.cookie).status == 405);
}

TEST_CASE("WebUpload: no address or no random source, no upload") {
	UploadFixture f;
	f.host.uploads = true;
	f.controller.connectivity().link_down("test");
	auto r = f.upload();
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "update.refused");
	CHECK(f.host.upload_tokens.empty());

	f.controller.connectivity().link_up("192.0.2.11");
	f.crypto.fail = true;
	r = f.upload();
	CHECK(r.status == 500);
	CHECK(r.json["code"].text == "auth.no_random");
	CHECK(f.host.upload_tokens.empty());
	f.crypto.fail = false;
	r = f.upload();
	REQUIRE(r.status == 200);
	CHECK(r.json["url"].text == "http://192.0.2.11/");
}

TEST_CASE("WebUpload: a bearer token may start the upload, the open session answer names the build") {
	UploadFixture f;
	f.host.uploads = true;
	auto created = f.request(HttpMethod::post, "/api/tokens", "{\"name\": \"ehome\"}", f.cookie);
	REQUIRE(created.status == 200);
	auto r = f.request(HttpMethod::post, "/api/system/update/upload", "", "", "Bearer " + created.json["token"].text,
		false);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(f.host.upload_tokens.size() == 1);

	auto session = f.get("/api/auth/session");
	REQUIRE(session.status == 200);
	CHECK(session.json["build"].text == hc::build_id());
}
