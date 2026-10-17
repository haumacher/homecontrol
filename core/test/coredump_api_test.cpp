/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// GET /api/coredump and the `boot` member of GET /api/status (issue #4, doc/diagnostics.md).

#include <string>

#include "doctest.h"
#include "web_test_support.h"

namespace {

/// A platform with a stored crash dump of `bytes` (byte i is i % 251).
class FakeDump : public hc::Diagnostics {
public:
	hc::ResetReason reset_reason() const override { return hc::ResetReason::panic; }
	void visit_boot(hc::DiagnosticsVisitor& visitor) const override { visitor.flag("coredump", bytes != 0); }
	size_t coredump_size() const override { return bytes; }
	bool coredump_read(size_t offset, void* buffer, size_t size) const override {
		if (fail || offset + size > bytes) {
			return false;
		}
		for (size_t i = 0; i < size; ++i) {
			static_cast<uint8_t*>(buffer)[i] = static_cast<uint8_t>((offset + i) % 251);
		}
		return true;
	}
	size_t bytes = 0;
	bool fail = false;
};

struct DumpFixture {
	hc_test::FakePlatform fake;
	FakeDump dump;
	hc::Controller controller{platform()};
	hc::DatapointModel model{fake.clock};
	hc_test::FakeCrypto crypto;
	hc_test::FakeWebHost host;
	hc::Auth auth{fake.settings, fake.clock, crypto};
	hc::WebApi api{controller, model, auth, host};
	char buffer[8192];

	DumpFixture() { auth.start(); }

	hc::Platform platform() {
		hc::Platform p = fake.platform();
		p.diagnostics = &dump;
		return p;
	}

	hc::WebResponse handle(hc::HttpMethod method, const char* path, const std::string& body, const std::string& cookie) {
		hc::WebRequest req;
		req.method = method;
		req.path = path;
		req.query = "";
		req.cookie = cookie.empty() ? nullptr : cookie.c_str();
		req.requested_with = "HomeControl";
		req.source = "192.0.2.7";
		req.body = body.c_str();
		req.body_length = body.size();
		hc::WebResponse resp(buffer, sizeof(buffer));
		api.handle(req, resp);
		if (resp.needs_hash) {
			hc::PasswordHash hash;
			hash.params = resp.hash_params;
			crypto.pbkdf2_sha256(reinterpret_cast<const uint8_t*>(resp.password), resp.password_length,
				hash.params.salt, sizeof(hash.params.salt), hash.params.iterations, hash.key, sizeof(hash.key));
			req.hash = &hash;
			hc::WebResponse second(buffer, sizeof(buffer));
			api.handle(req, second);
			return second;
		}
		return resp;
	}

	std::string onboard() {
		hc::WebResponse r = handle(hc::HttpMethod::post, "/api/auth/onboard", "{\"password\":\"correct horse\"}", "");
		REQUIRE(r.status == 200);
		std::string cookie = r.set_cookie;
		return cookie.substr(0, cookie.find(';'));
	}
};

} // namespace

TEST_CASE("CoreDumpApi.StatusHasTheBootMember") {
	DumpFixture f;
	std::string cookie = f.onboard();
	hc::WebResponse r = f.handle(hc::HttpMethod::get, "/api/status", "", cookie);
	REQUIRE(r.status == 200);
	std::string body(r.body, r.length);
	CHECK(body.find("\"boot\":{\"reason\":\"panic\",\"coredump\":false}") != std::string::npos);
}

TEST_CASE("CoreDumpApi.StreamsTheRawDump") {
	DumpFixture f;
	f.dump.bytes = 5000;
	std::string cookie = f.onboard();
	hc::WebResponse r = f.handle(hc::HttpMethod::get, "/api/coredump", "", cookie);
	REQUIRE(r.status == 200);
	CHECK(r.stream);
	CHECK(r.stream_length == 5000);
	CHECK(std::string(r.content_type) == "application/octet-stream");
	// The platform fetches the pieces as for the image (WebApi::read_stream).
	std::string all;
	char piece[1400];
	while (all.size() < r.stream_length) {
		size_t got = 0;
		REQUIRE(f.api.read_stream(r.stream_token, all.size(), piece, sizeof(piece), got));
		REQUIRE(got > 0);
		all.append(piece, got);
	}
	REQUIRE(all.size() == 5000);
	CHECK(static_cast<uint8_t>(all[0]) == 0);
	CHECK(static_cast<uint8_t>(all[4999]) == 4999 % 251);
	size_t got = 0;
	CHECK_FALSE(f.api.read_stream(r.stream_token, 5000, piece, sizeof(piece), got)); // past the end
	f.dump.fail = true;
	CHECK_FALSE(f.api.read_stream(r.stream_token, 0, piece, sizeof(piece), got));
}

TEST_CASE("CoreDumpApi.RefusalsSpeak") {
	DumpFixture f;
	f.dump.bytes = 100;
	std::string cookie = f.onboard();
	hc::WebResponse r = f.handle(hc::HttpMethod::get, "/api/coredump", "", "");
	CHECK(r.status == 401); // like every API path: the dump holds memory contents
	f.dump.bytes = 0;
	r = f.handle(hc::HttpMethod::get, "/api/coredump", "", cookie);
	CHECK(r.status == 404);
	CHECK(std::string(r.body, r.length).find("coredump.none") != std::string::npos);
	r = f.handle(hc::HttpMethod::del, "/api/coredump", "", cookie);
	CHECK(r.status == 405);
}
