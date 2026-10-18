/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The E-Home connection (issue #41, spec/api.md "E-Home connection", doc/ehome.md): the enrollment
// through a fake HTTP fetcher, the socket against a fake E-Home server behind a fake WebSocket
// client, the E-Home principal, the status.

#include <functional>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "fake_fetch.h"
#include "hc/ehome_link.h"
#include "hc/image/image_service.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "hc/live_feed.h"
#include "hc/sha.h"
#include "hc/signature.h"
#include "hc/websocket.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "web_test_support.h"

using hc::EhomeLink;
using hc::EhomeState;
using hc::HttpMethod;
using hc_test::Json;
using hc_test::JsonReader;
using hc_test::WebFixture;

namespace {

constexpr const char* kSecret = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
constexpr const char* kNonce = "0123456789abcdef0123456789abcdef";
/// HMAC-SHA256(key = the secret's 32 bytes, message = the nonce's 16 bytes), from Python's hmac.
constexpr const char* kMac = "696a8d99e57711fdb022863b6c952a393186157fa4f95d43d5b9943e657f13e9";
constexpr const char* kSocket = "wss://ehome.example/api/controllers/c-17/socket";

Json parse(const std::string& text) {
	Json json;
	std::string error;
	JsonReader(text).parse(json, error);
	return json;
}

/// A scripted WebSocket client: records what the link sends, delivers what the test plays E-Home.
class FakeWsClient : public hc::WebSocketClient {
public:
	bool connect(const char* url, hc::WebSocketClientListener& listener, const char** error) override {
		if (refuse_connect) {
			*error = "refused by the test";
			return false;
		}
		++connects;
		url_ = url;
		listener_ = &listener;
		session = true;
		return true;
	}
	bool send_text(const char* text, size_t length) override {
		if (!session || fail_sends) {
			return false;
		}
		texts.push_back(std::string(text, length));
		return true;
	}
	bool send_binary(const uint8_t* data, size_t length) override {
		if (!session) {
			return false;
		}
		binary.append(reinterpret_cast<const char*>(data), length);
		++binary_messages;
		return true;
	}
	bool send_ping() override {
		if (!session) {
			return false;
		}
		++pings;
		return true;
	}
	void close(uint16_t code, const char* reason) override {
		closes.push_back({code, reason});
		session = false;
	}

	// ---- The test plays the server ----
	void open() { listener_->ws_client_opened(); }
	void deliver(const std::string& text) { listener_->ws_client_text(text.data(), text.size()); }
	void deliver_binary(const std::string& data, size_t piece) {
		for (size_t i = 0; i < data.size(); i += piece) {
			size_t n = data.size() - i < piece ? data.size() - i : piece;
			listener_->ws_client_binary(reinterpret_cast<const uint8_t*>(data.data() + i), n, i + n == data.size());
		}
	}
	void pong() { listener_->ws_client_pong(); }
	void drop(const char* reason) {
		session = false;
		listener_->ws_client_closed(reason);
	}

	/// The messages of `type` the link sent.
	std::vector<Json> sent(const std::string& type) const {
		std::vector<Json> out;
		for (const std::string& t : texts) {
			Json json = parse(t);
			if (json["type"].text == type) {
				out.push_back(json);
			}
		}
		return out;
	}

	struct Close {
		uint16_t code;
		std::string reason;
	};
	bool refuse_connect = false;
	bool fail_sends = false;
	bool session = false;
	int connects = 0;
	int pings = 0;
	int binary_messages = 0;
	std::string url_;
	std::vector<std::string> texts;
	std::string binary;
	std::vector<Close> closes;
	hc::WebSocketClientListener* listener_ = nullptr;
};

/// An HTTP client whose POST answers the test delivers.
class FakeFetcher : public hc::HttpFetcher {
public:
	bool fetch(const char*, hc::FetchReceiver&, const char** error) override {
		*error = "GET is not used here";
		return false;
	}
	bool post(const char* url, const char* body, size_t length, hc::FetchReceiver& receiver,
		const char** error) override {
		if (busy_) {
			*error = "busy: a fetch is in flight";
			return false;
		}
		posts.push_back({url, std::string(body, length)});
		receiver_ = &receiver;
		busy_ = true;
		return true;
	}
	bool busy() const override { return busy_; }

	void answer(int status, const std::string& body) {
		REQUIRE(busy_);
		busy_ = false;
		hc_test::Delivery delivery;
		delivery.piece = 64; // the answer arrives in pieces
		hc_test::respond(*receiver_, status, body, delivery);
	}
	void unreachable(const char* why) {
		REQUIRE(busy_);
		busy_ = false;
		hc_test::respond_failure(*receiver_, why);
	}

	struct Post {
		std::string url;
		std::string body;
	};
	std::vector<Post> posts;
	bool busy_ = false;
	hc::FetchReceiver* receiver_ = nullptr;
};

std::string image_fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

struct EhomeFixture : WebFixture {
	hc_test::ScopedCapture capture{hc::LogLevel::debug};
	hc_test::FakeTransport transport;
	hc::LiveFeed feed{api, controller.loop(), transport};
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::ImageService service{controller, model, blobs, adapter, &tunnel};
	FakeWsClient client;
	FakeFetcher fetcher;
	EhomeLink link{api, feed, client, fetcher};
	std::string cookie;

	explicit EhomeFixture(bool enrolled = true, bool enabled = true) {
		controller.start();
		controller.connectivity().link_up("192.0.2.100");
		service.start();
		api.set_image(&service);
		cookie = onboard();
		REQUIRE(feed.start());
		if (enrolled) {
			store_enrollment(enabled);
		}
		link.start();
	}

	void store_enrollment(bool enabled) {
		hc::Settings& s = fake.settings;
		REQUIRE(s.set_string("ehome.url", "https://ehome.example") == hc::SettingsStatus::ok);
		REQUIRE(s.set_string("ehome.id", "c-17") == hc::SettingsStatus::ok);
		REQUIRE(s.set_string("ehome.name", "Example House") == hc::SettingsStatus::ok);
		REQUIRE(s.set_string("ehome.secret", kSecret) == hc::SettingsStatus::ok);
		REQUIRE(s.set_string("ehome.socket", kSocket) == hc::SettingsStatus::ok);
		REQUIRE(s.set_bool("ehome.enabled", enabled) == hc::SettingsStatus::ok);
	}

	/// Advances the clock in 100 ms steps, running the loop.
	void advance(uint64_t ms) {
		uint64_t end = fake.clock.now_ms + ms;
		while (fake.clock.now_ms < end) {
			fake.clock.advance(100);
			controller.loop().run_once(fake.clock.now_ms);
		}
	}

	void run() {
		for (int i = 0; i < 4; ++i) {
			controller.loop().run_once(fake.clock.now_ms);
		}
	}

	/// Plays E-Home's side of the authentication up to `ready`.
	void handshake() {
		REQUIRE(client.session);
		client.open();
		client.deliver(std::string("{\"type\":\"challenge\",\"nonce\":\"") + kNonce + "\"}");
		auto auth = client.sent("auth");
		REQUIRE(auth.size() >= 1);
		CHECK(auth.back()["controller"].text == "c-17");
		CHECK(auth.back()["mac"].text == kMac);
		client.deliver("{\"type\":\"ready\",\"installation\":\"Example House\"}");
		REQUIRE(link.state() == EhomeState::connected);
	}

	/// The response to request `id` (the last one), parsed.
	Json response(int id) {
		Json found;
		for (const Json& r : client.sent("response")) {
			if (r["id"].number == id) {
				found = r;
			}
		}
		return found;
	}

	Result put(const std::string& target, const std::string& body) {
		return request(HttpMethod::put, target, body, cookie);
	}
	Result del(const std::string& target) { return request(HttpMethod::del, target, "", cookie); }

	/// A request as a platform makes it, polling a deferred answer the way the servers do.
	Result enroll(const std::string& body, std::function<void()> server) {
		hc::WebRequest req;
		req.method = HttpMethod::post;
		req.path = "/api/ehome/enroll";
		req.query = "";
		req.cookie = cookie.c_str();
		req.requested_with = "HomeControl";
		req.source = "192.0.2.7";
		req.body = body.c_str();
		req.body_length = body.size();
		hc::WebResponse resp(buffer, sizeof(buffer));
		api.handle(req, resp);
		if (resp.deferred) {
			CHECK_FALSE(api.poll_deferred(resp));
			if (server) {
				server();
			}
			for (int i = 0; i < 400 && !api.poll_deferred(resp); ++i) {
				advance(100);
			}
		}
		return result(resp);
	}

	Json status() { return get("/api/status", cookie).json["ehome"]; }
};

constexpr const char* kEnrollAnswer =
	R"({"controller": "c-17", "installation": "Example House", "secret": "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", "socket": "wss://ehome.example/api/controllers/c-17/socket"})";

} // namespace

TEST_CASE("E-Home: the MAC is HMAC-SHA256 over the decoded nonce with the decoded secret") {
	char mac[65];
	REQUIRE(hc::ehome_mac(kSecret, kNonce, mac));
	CHECK(std::string(mac) == kMac);
	CHECK_FALSE(hc::ehome_mac(kSecret, "0123", mac));     // a nonce too short
	CHECK_FALSE(hc::ehome_mac(kSecret, "0123456789abcdef0123456789abcdeg", mac));
	CHECK_FALSE(hc::ehome_mac("00", kNonce, mac));
	CHECK(hc::ehome_url_valid("https://ehome.example"));
	CHECK(hc::ehome_url_valid("http://192.0.2.5:8080/ehome"));
	CHECK_FALSE(hc::ehome_url_valid("ftp://ehome.example"));
	CHECK_FALSE(hc::ehome_url_valid("https://"));
	CHECK_FALSE(hc::ehome_url_valid("https://ehome.example/?x=1"));
	CHECK_FALSE(hc::ehome_url_valid("https://user@ehome.example"));
	CHECK(hc::ehome_socket_valid(kSocket, "https://ehome.example"));
	CHECK_FALSE(hc::ehome_socket_valid("ws://ehome.example/s", "https://ehome.example"));
	CHECK(hc::ehome_socket_valid("ws://192.0.2.5:8080/s", "http://192.0.2.5:8080"));
	CHECK(hc::ehome_code_valid("ABCD-EFGH"));
	CHECK_FALSE(hc::ehome_code_valid("ABCD EFGH"));
	CHECK(hc::setting_is_secret("ehome.secret"));
}

TEST_CASE("E-Home: the full exchange — challenge, auth, ready, the feed, a request, the image in binary frames") {
	EhomeFixture f;
	CHECK(f.client.connects == 1);
	CHECK(f.client.url_ == kSocket);
	CHECK(f.link.state() == EhomeState::connecting);
	CHECK(f.status()["state"].text == "connecting");
	f.handshake();
	CHECK(f.capture.sink.contains("I hc.ehome: connected to E-Home (installation \"Example House\")"));
	auto hello = f.client.sent("hello");
	REQUIRE(hello.size() == 1);
	CHECK(hello[0]["authenticated"].boolean);

	// The live feed: subscribe → status (with the ehome member) and the snapshot.
	f.client.deliver("{\"type\":\"subscribe\"}");
	f.run();
	auto status = f.client.sent("status");
	REQUIRE(status.size() >= 1);
	CHECK(status[0]["ehome"]["state"].text == "connected");
	CHECK(status[0]["ehome"]["installation"].text == "Example House");

	CHECK(f.client.sent("datapoints").size() >= 1);

	// A request: GET /api/status → response 200 with the body.
	f.client.deliver("{\"type\":\"request\",\"id\":7,\"method\":\"GET\",\"path\":\"/api/status\"}");
	Json r = f.response(7);
	CHECK(r["status"].number == 200);
	CHECK(r["body"]["device"]["name"].text == "homecontrol-a1b2c3");
	CHECK(r["body"]["ehome"]["state"].text == "connected");

	// A request with a query.
	f.client.deliver("{\"type\":\"request\",\"id\":\"q\",\"method\":\"GET\",\"path\":\"/api/datapoints?from=0\"}");
	Json q;
	for (const Json& x : f.client.sent("response")) {
		if (x["id"].text == "q") {
			q = x;
		}
	}
	CHECK(q["status"].number == 200);
	CHECK(q["body"]["from"].number == 0);

	// PUT /api/image with `length` and binary frames.
	std::string image = image_fixture("example.json");
	REQUIRE(image.size() > hc::WebSocketClient::kMaxText);
	f.client.deliver("{\"type\":\"request\",\"id\":8,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(image.size()) + "}");
	CHECK_FALSE(f.response(8).has("status")); // not before the last byte
	f.client.deliver("{\"type\":\"ping\"}"); // the feed keeps working between the frames
	CHECK(f.client.sent("pong").size() == 1);
	f.client.deliver_binary(image, 1024);
	Json loaded = f.response(8);
	CHECK(loaded["status"].number == 200);
	CHECK(f.service.loaded());
	CHECK(f.capture.sink.contains("PUT /api/image from E-Home"));

	// GET /api/image comes back as a header and binary frames, a piece per loop turn.
	f.client.deliver("{\"type\":\"request\",\"id\":9,\"method\":\"GET\",\"path\":\"/api/image\"}");
	Json header = f.response(9);
	CHECK(header["status"].number == 200);
	CHECK(header["length"].number == static_cast<double>(image.size()));
	f.run();
	f.run();
	CHECK(f.client.binary == image);
	CHECK(f.client.binary_messages >= 3);

	// The status of the API shows the connection.
	Json e = f.status();
	CHECK(e["enabled"].boolean);
	CHECK(e["enrolled"].boolean);
	CHECK(e["installation"].text == "Example House");
	// The secret never reached the log.
	for (const std::string& line : f.capture.sink.lines) {
		CHECK(line.find(kSecret) == std::string::npos);
		CHECK(line.find(kMac) == std::string::npos);
	}
}

TEST_CASE("E-Home: a wrong secret is refused, the link shows failed with the reason and retries by backoff") {
	EhomeFixture f;
	f.client.open();
	f.client.deliver(std::string("{\"type\":\"challenge\",\"nonce\":\"") + kNonce + "\"}");
	f.client.deliver("{\"type\":\"refused\",\"code\":\"ehome.auth_failed\",\"error\":\"the MAC does not match\"}");
	CHECK(f.link.state() == EhomeState::failed);
	CHECK(f.client.closes.size() == 1);
	CHECK(f.capture.sink.contains("W hc.ehome: E-Home refused the connection: the MAC does not match "
		"(ehome.auth_failed); retry in 1 s (failure 1)"));
	Json e = f.status();
	CHECK(e["state"].text == "failed");
	CHECK(e["error"].text == "E-Home refused the connection: the MAC does not match (ehome.auth_failed)");
	f.advance(1100);
	CHECK(f.client.connects == 2);
	CHECK(f.link.state() == EhomeState::connecting);
	f.client.drop("closed by the server (1008)");
	CHECK(f.link.failures() == 2);
	f.advance(1500);
	CHECK(f.client.connects == 2); // the second delay is 2 s
	f.advance(600);
	CHECK(f.client.connects == 3);
}

TEST_CASE("E-Home: ehome.enabled false closes at once and never reconnects") {
	EhomeFixture f;
	f.handshake();
	REQUIRE(f.feed.ehome_attached());
	auto r = f.put("/api/settings/ehome.enabled", "{\"value\":\"false\"}");
	CHECK(r.status == 200);
	CHECK(f.link.state() == EhomeState::off);
	CHECK(f.client.closes.size() == 1);
	CHECK_FALSE(f.feed.ehome_attached());
	CHECK(f.capture.sink.contains("I hc.ehome: connection closed: switched off by the owner"));
	f.advance(120000);
	CHECK(f.client.connects == 1);
	CHECK(f.status()["state"].text == "off");
	// On again: connects at once.
	r = f.put("/api/settings/ehome.enabled", "{\"value\":\"true\"}");
	CHECK(r.status == 200);
	CHECK(f.client.connects == 2);
	// The settings list: the first three read-only, the switch editable, the secret absent.
	auto list = f.get("/api/settings", f.cookie);
	bool url = false, enabled = false;
	for (const Json& s : list.json["settings"].items) {
		CHECK(s["key"].text != "ehome.secret");
		CHECK(s["key"].text != "ehome.socket");
		if (s["key"].text == "ehome.url") {
			url = true;
			CHECK_FALSE(s["editable"].boolean);
			CHECK(s["value"].text == "https://ehome.example");
		}
		if (s["key"].text == "ehome.enabled") {
			enabled = true;
			CHECK(s["editable"].boolean);
			CHECK(s["value"].text == "true");
		}
	}
	CHECK(url);
	CHECK(enabled);
	CHECK(f.put("/api/settings/ehome.url", "{\"value\":\"https://other.example\"}").status == 403);
}

TEST_CASE("E-Home: a second request before the response is refused 429 ehome.busy") {
	EhomeFixture f;
	f.handshake();
	std::string image = image_fixture("example.json");
	f.client.deliver("{\"type\":\"request\",\"id\":1,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(image.size()) + "}");
	f.client.deliver("{\"type\":\"request\",\"id\":2,\"method\":\"GET\",\"path\":\"/api/status\"}");
	Json busy = f.response(2);
	CHECK(busy["status"].number == 429);
	CHECK(busy["body"]["code"].text == "ehome.busy");
	f.client.deliver_binary(image, 3000);
	CHECK(f.response(1)["status"].number == 200);
	// Now the next one runs.
	f.client.deliver("{\"type\":\"request\",\"id\":3,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK(f.response(3)["status"].number == 200);
	// A body announced while another runs cannot be told apart: the socket closes.
	f.client.deliver("{\"type\":\"request\",\"id\":4,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":100}");
	f.client.deliver("{\"type\":\"request\",\"id\":5,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":100}");
	CHECK(f.response(5)["status"].number == 429);
	CHECK(f.link.state() == EhomeState::failed);
}

TEST_CASE("E-Home: the principal is refused onboarding, WiFi, the factory reset, the enrollment") {
	EhomeFixture f;
	f.handshake();
	struct Case {
		const char* method;
		const char* path;
		const char* body;
	};
	const Case cases[] = {
		{"POST", "/api/auth/onboard", "{\"password\":\"another horse\"}"},
		{"POST", "/api/auth/login", "{\"password\":\"correct horse\"}"},
		{"PUT", "/api/system/wifi", "{\"ssid\":\"x\",\"psk\":\"12345678\"}"},
		{"POST", "/api/system/factory-reset", "{\"confirm\":\"homecontrol-a1b2c3\"}"},
		{"POST", "/api/system/update/upload", "{}"},
		{"POST", "/api/ehome/enroll", "{\"url\":\"https://ehome.example\",\"code\":\"ABCD\"}"},
		{"DELETE", "/api/ehome", "null"},
		{"POST", "/api/tokens", "{\"name\":\"x\"}"},
		{"PUT", "/api/settings/update.url", "{\"value\":\"https://evil.example\"}"},
		{"PUT", "/api/settings/ehome.enabled", "{\"value\":\"false\"}"},
	};
	int id = 100;
	for (const Case& c : cases) {
		f.client.deliver(std::string("{\"type\":\"request\",\"id\":") + std::to_string(id) + ",\"method\":\"" + c.method +
			"\",\"path\":\"" + c.path + "\",\"body\":" + c.body + "}");
		Json r = f.response(id);
		CHECK_MESSAGE(r["status"].number == 403, c.path);
		CHECK_MESSAGE(r["body"]["code"].text == "ehome.forbidden", c.path);
		++id;
	}
	CHECK(f.link.state() == EhomeState::connected); // the settings are untouched
	CHECK(f.capture.sink.contains("W hc.web: PUT /api/system/wifi over the E-Home connection refused"));
	// What a token may do works: a setting, a command.
	f.client.deliver("{\"type\":\"request\",\"id\":200,\"method\":\"PUT\",\"path\":\"/api/settings/ntp.server\","
		"\"body\":{\"value\":\"ntp.example\"}}");
	CHECK(f.response(200)["status"].number == 200);
}

TEST_CASE("E-Home: the enrollment stores the settings and starts the link") {
	EhomeFixture f(false);
	CHECK(f.status()["state"].text == "off");
	CHECK_FALSE(f.status()["enrolled"].boolean);
	CHECK(f.client.connects == 0);
	// Switching on without an enrollment is refused.
	CHECK(f.put("/api/settings/ehome.enabled", "{\"value\":\"true\"}").json["code"].text == "ehome.not_enrolled");
	auto r = f.enroll("{\"url\":\"https://ehome.example/\",\"code\":\"ABCD-EFGH\"}",
		[&] { f.fetcher.answer(200, kEnrollAnswer); });
	CHECK(r.status == 200);
	CHECK(r.json["ok"].boolean);
	CHECK(r.json["installation"].text == "Example House");
	REQUIRE(f.fetcher.posts.size() == 1);
	CHECK(f.fetcher.posts[0].url == "https://ehome.example/api/controllers/enroll");
	Json sent = parse(f.fetcher.posts[0].body);
	CHECK(sent["code"].text == "ABCD-EFGH");
	CHECK(sent["name"].text == "homecontrol-a1b2c3");
	CHECK(sent.has("version"));
	CHECK(sent.has("build"));
	char value[130];
	REQUIRE(f.fake.settings.get_string("ehome.secret", value, sizeof(value)) == hc::SettingsStatus::ok);
	CHECK(std::string(value) == kSecret);
	REQUIRE(f.fake.settings.get_string("ehome.url", value, sizeof(value)) == hc::SettingsStatus::ok);
	CHECK(std::string(value) == "https://ehome.example");
	bool on = false;
	CHECK(f.fake.settings.get_bool("ehome.enabled", on) == hc::SettingsStatus::ok);
	CHECK(on);
	CHECK(f.client.connects == 1);
	CHECK(f.client.url_ == kSocket);
	f.handshake();
	for (const std::string& line : f.capture.sink.lines) {
		CHECK(line.find(kSecret) == std::string::npos);
	}
	// A second enrollment is refused while enrolled.
	auto again = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"ABCD-EFGH\"}", nullptr);
	CHECK(again.status == 409);
	CHECK(again.json["code"].text == "ehome.enrolled");
}

TEST_CASE("E-Home: every refusal of the enrollment comes through with its code") {
	EhomeFixture f(false);
	auto r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"WRONG\"}",
		[&] { f.fetcher.answer(404, "{\"error\":\"no such code\",\"code\":\"ehome.code_unknown\"}"); });
	CHECK(r.status == 404);
	CHECK(r.json["code"].text == "ehome.code_unknown");
	CHECK(r.json["error"].text == "no such code");
	r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"OLD\"}", [&] { f.fetcher.answer(410, ""); });
	CHECK(r.status == 410);
	CHECK(r.json["code"].text == "ehome.code_expired");
	r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"TAKEN\"}",
		[&] { f.fetcher.answer(409, "{\"error\":\"enrolled in another project\",\"code\":\"ehome.enrolled\"}"); });
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "ehome.enrolled");
	r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"X\"}",
		[&] { f.fetcher.unreachable("cannot connect: ESP_ERR_HTTP_CONNECT"); });
	CHECK(r.status == 502);
	CHECK(r.json["code"].text == "ehome.unreachable");
	CHECK(r.json["error"].text == "cannot reach E-Home: cannot connect: ESP_ERR_HTTP_CONNECT");
	r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"X\"}", [&] { f.fetcher.answer(500, "{}"); });
	CHECK(r.status == 502);
	CHECK(r.json["error"].text == "E-Home answered HTTP 500");
	r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"X\"}",
		[&] { f.fetcher.answer(200, "{\"controller\":\"c\",\"installation\":\"I\",\"secret\":\"short\",\"socket\":\"wss://x\"}"); });
	CHECK(r.status == 502);
	CHECK(r.json["code"].text == "ehome.bad_answer");
	r = f.enroll("{\"url\":\"ftp://ehome.example\",\"code\":\"X\"}", nullptr);
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "ehome.url_invalid");
	r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"A B\"}", nullptr);
	CHECK(r.json["code"].text == "ehome.code_invalid");
	// No answer at all: the guard answers.
	r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"SLOW\"}", nullptr);
	CHECK(r.status == 502);
	CHECK(r.json["error"].text == "no answer from E-Home within 30 s");
	CHECK(f.capture.sink.contains("W hc.ehome: enrollment with https://ehome.example failed: no answer from E-Home within 30 s"));
	CHECK_FALSE(hc::ehome_enrolled(f.fake.settings));
	CHECK(f.client.connects == 0);
}

TEST_CASE("E-Home: the enrollment answer streams in; an oversize answer is refused with the limit") {
	EhomeFixture f(false);
	// An answer longer than kMaxEnrollAnswer (padding in a member E-Home does not send today).
	std::string padded = std::string(kEnrollAnswer);
	padded.insert(1, "\"pad\":\"" + std::string(hc::EhomeLink::kMaxEnrollAnswer, ' ') + "\",");
	auto r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"ABCD-EFGH\"}",
		[&] { f.fetcher.answer(200, padded); });
	CHECK(r.status == 502);
	CHECK(r.json["code"].text == "ehome.bad_answer");
	CHECK(r.json["error"].text ==
		"E-Home's answer is not usable: E-Home's answer is larger than 2048 bytes (" + std::to_string(padded.size()) +
		" announced)");
	CHECK_FALSE(hc::ehome_enrolled(f.fake.settings));
	// A refusal document that does not fit still has its status.
	r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"WRONG\"}",
		[&] { f.fetcher.answer(404, "{\"error\":\"" + std::string(3000, 'x') + "\"}"); });
	CHECK(r.status == 404);
	CHECK(r.json["code"].text == "ehome.code_unknown");
	// The regular answer (in 64-byte pieces) still enrolls.
	r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"ABCD-EFGH\"}",
		[&] { f.fetcher.answer(200, kEnrollAnswer); });
	CHECK(r.status == 200);
	CHECK(hc::ehome_enrolled(f.fake.settings));
}

TEST_CASE("E-Home: DELETE /api/ehome erases the settings and closes the link") {
	EhomeFixture f;
	f.handshake();
	auto r = f.del("/api/ehome");
	CHECK(r.status == 200);
	CHECK(f.link.state() == EhomeState::off);
	CHECK(f.client.closes.size() == 1);
	CHECK_FALSE(f.feed.ehome_attached());
	char value[130];
	for (const char* key : {"ehome.url", "ehome.id", "ehome.name", "ehome.secret", "ehome.socket"}) {
		CHECK(f.fake.settings.get_string(key, value, sizeof(value)) == hc::SettingsStatus::not_found);
	}
	bool on = false;
	CHECK(f.fake.settings.get_bool("ehome.enabled", on) == hc::SettingsStatus::not_found);
	CHECK(f.capture.sink.contains("I hc.ehome: disconnected from E-Home at https://ehome.example: the enrollment is erased"));
	CHECK_FALSE(f.status()["enrolled"].boolean);
	f.advance(120000);
	CHECK(f.client.connects == 1);
	CHECK(f.request(HttpMethod::del, "/api/ehome", "", "", "", false).status == 401); // no principal
}

TEST_CASE("E-Home: a missed pong closes the socket and the backoff reconnects it") {
	EhomeFixture f;
	f.handshake();
	f.advance(EhomeLink::kPingMs);
	CHECK(f.client.pings == 1);
	f.client.pong();
	f.advance(EhomeLink::kPingMs);
	CHECK(f.client.pings == 2);
	CHECK(f.link.state() == EhomeState::connected);
	f.advance(EhomeLink::kPongTimeoutMs);
	CHECK(f.link.state() == EhomeState::failed);
	CHECK(f.client.closes.size() == 1);
	CHECK(f.status()["error"].text == "no pong from E-Home within 10 s");
	f.advance(1100);
	CHECK(f.client.connects == 2);
	f.handshake();
	CHECK(f.link.failures() == 0);
}

TEST_CASE("E-Home: the status shows the states and the network wait") {
	EhomeFixture f(true, false);
	CHECK(f.status()["state"].text == "off");
	CHECK(f.status()["installation"].text == "Example House");
	f.controller.connectivity().link_down("test");
	f.put("/api/settings/ehome.enabled", "{\"value\":\"true\"}");
	CHECK(f.status()["state"].text == "connecting");
	CHECK(f.client.connects == 0);
	CHECK(f.capture.sink.contains("I hc.ehome: waiting for the network"));
	f.controller.connectivity().link_up("192.0.2.100");
	f.advance(1000);
	CHECK(f.client.connects == 1);
	// No ready within the handshake's time: failed.
	f.client.open();
	f.advance(EhomeLink::kHandshakeTimeoutMs);
	CHECK(f.status()["state"].text == "failed");
	CHECK(f.status()["error"].text == "no challenge from E-Home within 30 s");
	f.advance(1100);
	f.handshake();
	CHECK(f.status()["state"].text == "connected");
	f.client.drop("transport: ESP_ERR_ESP_TLS_TCP_CLOSED_FIN");
	CHECK(f.status()["state"].text == "failed");
	CHECK(f.status()["error"].text == "connection lost: transport: ESP_ERR_ESP_TLS_TCP_CLOSED_FIN");
}

TEST_CASE("E-Home: a message other than the challenge during the authentication fails the attempt") {
	EhomeFixture f;
	f.client.open();
	f.client.deliver("{\"type\":\"subscribe\"}");
	CHECK(f.link.state() == EhomeState::failed);
	CHECK(f.status()["error"].text == "E-Home sent \"subscribe\" while the controller awaited the challenge");
}

TEST_CASE("E-Home: a body longer than the route takes is refused 413 and its frames dropped") {
	EhomeFixture f;
	f.handshake();
	f.client.deliver("{\"type\":\"request\",\"id\":1,\"method\":\"POST\",\"path\":\"/api/datapoints/0/command\","
		"\"length\":5000}");
	CHECK(f.response(1)["status"].number == 413);
	CHECK(f.response(1)["body"]["code"].text == "api.body_too_large");
	f.client.deliver_binary(std::string(5000, 'x'), 1024);
	CHECK(f.link.state() == EhomeState::connected);
	f.client.deliver("{\"type\":\"request\",\"id\":2,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK(f.response(2)["status"].number == 200);
	// A binary frame nobody announced ends the connection.
	f.client.deliver_binary("xyz", 3);
	CHECK(f.link.state() == EhomeState::failed);
}

TEST_CASE("E-Home: a command over the feed is recorded in the monitor as via ehome") {
	EhomeFixture f;
	f.service.load(image_fixture("example.json").data(), image_fixture("example.json").size(),
		hc::image::Source::api, false);
	f.handshake();
	f.client.deliver("{\"type\":\"monitor\",\"subscribe\":true}");
	uint16_t writable = 0xFFFF;
	for (size_t i = 0; i < f.model.size(); ++i) {
		const hc::Datapoint* dp = f.model.get(static_cast<uint16_t>(i));
		if (std::string(dp->name()) == "away") { // a virtual one: the command needs no bus
			writable = dp->id();
			break;
		}
	}
	REQUIRE(writable != 0xFFFF);
	f.client.deliver("{\"type\":\"command\",\"id\":" + std::to_string(writable) + ",\"value\":true}");
	CHECK(f.client.sent("result").back()["ok"].boolean);
	f.run();
	bool found = false;
	for (const Json& m : f.client.sent("monitor")) {
		for (const Json& e : m["events"].items) {
			if (e["kind"].text == "command") {
				CHECK(e["via"].text == "ehome");
				found = true;
			}
		}
	}
	CHECK(found);
}

// ---- Probe (the orchestrator's, composed with the settings and the tokens) ---------------------

TEST_CASE("E-Home probe: the principal may change a setting a token may, not the tokens nor its own switch") {
	EhomeFixture f;
	f.handshake();

	// A token's right: a setting change goes through and is stored.
	f.client.deliver("{\"type\":\"request\",\"id\":\"tz\",\"method\":\"PUT\",\"path\":\"/api/settings/ntp.server\","
		"\"body\":{\"value\":\"ntp.example\"}}");
	Json tz;
	for (const Json& x : f.client.sent("response")) {
		if (x["id"].text == "tz") {
			tz = x;
		}
	}
	REQUIRE(tz.has("status"));
	CHECK(tz["status"].number == 200);
	char stored[64] = {};
	CHECK(f.controller.settings().get_string("ntp.server", stored, sizeof(stored)) == hc::SettingsStatus::ok);
	CHECK(std::string(stored) == "ntp.example");

	// Not a token's right over this channel: the tokens themselves and the connection's own settings.
	f.client.deliver("{\"type\":\"request\",\"id\":11,\"method\":\"POST\",\"path\":\"/api/tokens\",\"body\":{\"name\":\"x\"}}");
	Json tokens = f.response(11);
	CHECK(tokens["status"].number == 403);
	CHECK(tokens["body"]["code"].text == "ehome.forbidden");
	f.client.deliver("{\"type\":\"request\",\"id\":12,\"method\":\"PUT\",\"path\":\"/api/settings/ehome.enabled\","
		"\"body\":{\"value\":\"false\"}}");
	CHECK(f.response(12)["status"].number == 403);
	CHECK(f.link.state() == EhomeState::connected);
	f.client.deliver("{\"type\":\"request\",\"id\":13,\"method\":\"GET\",\"path\":\"/api/tokens\"}");
	CHECK(f.response(13)["status"].number == 403);

	// The owner's switch over HTTP, meanwhile, works and closes the socket at once.
	auto off = f.put("/api/settings/ehome.enabled", "{\"value\":\"false\"}");
	REQUIRE_MESSAGE(off.status == 200, off.body);
	CHECK(f.link.state() == EhomeState::off);
	CHECK_FALSE(f.client.session);
	CHECK(f.status()["state"].text == "off");
	CHECK(f.status()["enrolled"].boolean);
	// Any request after the close is not answered (there is no socket to answer on).
	size_t responses = f.client.sent("response").size();
	f.client.deliver("{\"type\":\"request\",\"id\":14,\"method\":\"GET\",\"path\":\"/api/status\"}");
	f.run();
	CHECK(f.client.sent("response").size() == responses);
}

// ---- The installation key (issue #35, spec/api.md "Signed runtime images") ---------------------

namespace {

// TEST KEYS ONLY (spec/fixtures/signatures/vectors.json).
constexpr const char* kKeySeed = "48613bb792f76c2201cb3521eaab920dad55d68982490274331eff6dc73a2fb3";
constexpr const char* kKey = "13833fbfa81080cdb4c1210131a5bb3e98f17675236e66124aed84cc490bccb1";
constexpr const char* kKeyFingerprint = "ce7d 4a41 a476 9ee3";
constexpr const char* kNewSeed = "2bb9dbea0c0c160975dc5e23749d1891e42869b0b87f30c173d3155af920d817";
constexpr const char* kNewKey = "ed592207e3c3f843383c53d6d0b402b2b047323ca0eca31673655d6a940d96cd";
constexpr const char* kNewFingerprint = "90ae 3702 f3a9 63ab";

std::string signature_of(const char* seed_hex, const std::string& image) {
	uint8_t seed[32];
	REQUIRE(hc::from_hex(seed_hex, seed, sizeof(seed)));
	uint8_t sig[64];
	hc::sign_image(seed, image.data(), image.size(), sig);
	char hex[129];
	hc::to_hex(sig, sizeof(sig), hex);
	return hex;
}

std::string enroll_answer_with_key(const std::string& key) {
	std::string answer = kEnrollAnswer;
	answer.insert(answer.size() - 1, ", \"public_key\": \"" + key + "\"");
	return answer;
}

/// PUT /api/image over the socket: `length`, optionally `signature`, then the binary frames.
Json socket_put_image(EhomeFixture& f, int id, const std::string& image, const std::string& signature) {
	std::string request = "{\"type\":\"request\",\"id\":" + std::to_string(id) +
		",\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" + std::to_string(image.size());
	if (!signature.empty()) {
		request += ",\"signature\":\"" + signature + "\"";
	}
	f.client.deliver(request + "}");
	f.client.deliver_binary(image, 1024);
	return f.response(id);
}

std::string stored_key(EhomeFixture& f) {
	char value[130] = {};
	return f.fake.settings.get_string("ehome.key", value, sizeof(value)) == hc::SettingsStatus::ok ? value : "";
}

} // namespace

TEST_CASE("E-Home key: the enrollment answer's public_key is stored, its fingerprint shown; without one no key") {
	{
		EhomeFixture f(false);
		auto r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"ABCD-EFGH\"}",
			[&] { f.fetcher.answer(200, enroll_answer_with_key(kKey)); });
		REQUIRE(r.status == 200);
		CHECK(stored_key(f) == kKey);
		CHECK(f.status()["key"].text == kKeyFingerprint);
		CHECK_FALSE(f.status().has("pending_key"));
		CHECK(f.capture.sink.contains("I hc.ehome: installation key ce7d 4a41 a476 9ee3 stored: runtime images need "
			"its signature from now on"));
		// The auth message names the key the device holds.
		f.client.open();
		f.client.deliver(std::string("{\"type\":\"challenge\",\"nonce\":\"") + kNonce + "\"}");
		REQUIRE(f.client.sent("auth").size() == 1);
		CHECK(f.client.sent("auth")[0]["key"].text == kKeyFingerprint);
		// The key is never listed by the API.
		CHECK(f.get("/api/settings", f.cookie).body.find(kKey) == std::string::npos);
		// Disconnect erases the enrollment, not the key (the next test).
		REQUIRE(f.del("/api/ehome").status == 200);
		CHECK(stored_key(f) == kKey);
		CHECK(f.status()["key"].text == kKeyFingerprint);
	}
	{
		EhomeFixture f(false);
		auto r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"ABCD-EFGH\"}",
			[&] { f.fetcher.answer(200, kEnrollAnswer); });
		REQUIRE(r.status == 200);
		CHECK(stored_key(f).empty());
		CHECK_FALSE(f.status().has("key"));
		CHECK(f.capture.sink.contains("I hc.ehome: E-Home sent no installation key: runtime images are not signed"));
		// An unsigned image goes in over the socket, as before.
		f.handshake();
		CHECK(f.client.sent("auth")[0].has("key") == false);
		CHECK(socket_put_image(f, 3, image_fixture("minimal.json"), "")["status"].number == 200);
	}
	{
		EhomeFixture f(false);
		auto r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"ABCD-EFGH\"}",
			[&] { f.fetcher.answer(200, enroll_answer_with_key("1234")); });
		CHECK(r.status == 502);
		CHECK(r.json["code"].text == "ehome.bad_answer");
		CHECK(r.json["error"].text ==
			"E-Home's answer is not usable: public_key (not an Ed25519 public key of 64 hex digits)");
		CHECK_FALSE(hc::ehome_enrolled(f.fake.settings));
		CHECK(stored_key(f).empty());
	}
}

TEST_CASE("E-Home key: over the socket a signed image is accepted, an unsigned or mis-signed one refused") {
	EhomeFixture f;
	REQUIRE(f.fake.settings.set_string("ehome.key", kKey) == hc::SettingsStatus::ok);
	f.handshake();
	std::string example = image_fixture("example.json");
	Json r = socket_put_image(f, 4, example, "");
	CHECK(r["status"].number == 403);
	CHECK(r["body"]["code"].text == "image.unsigned");
	CHECK(r["body"]["refusals"].items[0]["reason"].text.find(kKeyFingerprint) != std::string::npos);
	CHECK_FALSE(f.service.loaded());
	r = socket_put_image(f, 5, example, signature_of(kNewSeed, example));
	CHECK(r["status"].number == 403);
	CHECK(r["body"]["code"].text == "image.bad_signature");
	CHECK_FALSE(f.service.loaded());
	r = socket_put_image(f, 6, example, "abc");
	CHECK(r["status"].number == 400);
	CHECK(r["body"]["error"].text == "\"signature\" is the image's Ed25519 signature in 128 hex digits");
	r = socket_put_image(f, 7, example, signature_of(kKeySeed, example));
	CHECK(r["status"].number == 200);
	CHECK(f.service.info().revision == 7);
	// The signature belongs to its request: the next one without it is unsigned again.
	r = socket_put_image(f, 8, example, "");
	CHECK(r["body"]["code"].text == "image.unsigned");
	CHECK(f.link.state() == EhomeState::connected);
}

TEST_CASE("E-Home key: a rotation stays pending until the owner accepts it; then the old key is refused") {
	EhomeFixture f;
	REQUIRE(f.fake.settings.set_string("ehome.key", kKey) == hc::SettingsStatus::ok);
	f.handshake();
	std::string minimal = image_fixture("minimal.json");
	std::string example = image_fixture("example.json");
	f.client.deliver(std::string("{\"type\":\"key\",\"public_key\":\"") + kNewKey + "\",\"fingerprint\":\"" +
		kNewFingerprint + "\"}");
	auto pending = f.client.sent("key_pending");
	REQUIRE(pending.size() == 1);
	CHECK(pending[0]["fingerprint"].text == kNewFingerprint);
	CHECK(f.status()["key"].text == kKeyFingerprint);
	CHECK(f.status()["pending_key"].text == kNewFingerprint);
	CHECK(f.capture.sink.contains("W hc.ehome: E-Home offers a new installation key 90ae 3702 f3a9 63ab: it takes "
		"effect when the owner accepts it"));
	CHECK(stored_key(f) == kKey);
	// Until accepted: signed with the new key refused, the old key in force.
	CHECK(socket_put_image(f, 10, minimal, signature_of(kNewSeed, minimal))["body"]["code"].text == "image.bad_signature");
	CHECK(socket_put_image(f, 11, minimal, signature_of(kKeySeed, minimal))["status"].number == 200);
	// E-Home cannot accept its own key.
	f.client.deliver("{\"type\":\"request\",\"id\":12,\"method\":\"POST\",\"path\":\"/api/ehome/key/accept\","
		"\"body\":{\"fingerprint\":\"90ae 3702 f3a9 63ab\"}}");
	CHECK(f.response(12)["status"].number == 403);
	CHECK(f.response(12)["body"]["code"].text == "ehome.forbidden");
	// The owner accepts another fingerprint than the pending one: refused.
	auto r = f.request(HttpMethod::post, "/api/ehome/key/accept", "{\"fingerprint\":\"0000 0000 0000 0000\"}", f.cookie);
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "ehome.key_changed");
	r = f.request(HttpMethod::post, "/api/ehome/key/accept", "{}", f.cookie);
	CHECK(r.status == 400);
	// The owner accepts the key shown.
	r = f.request(HttpMethod::post, "/api/ehome/key/accept",
		std::string("{\"fingerprint\":\"") + kNewFingerprint + "\"}", f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.json["key"].text == kNewFingerprint);
	CHECK(stored_key(f) == kNewKey);
	auto accepted = f.client.sent("key_accepted");
	REQUIRE(accepted.size() == 1);
	CHECK(accepted[0]["fingerprint"].text == kNewFingerprint);
	CHECK(f.status()["key"].text == kNewFingerprint);
	CHECK_FALSE(f.status().has("pending_key"));
	CHECK(f.capture.sink.contains("W hc.ehome: installation key 90ae 3702 f3a9 63ab accepted by the owner"));
	// Now the old key is refused and the new one accepted.
	CHECK(socket_put_image(f, 13, example, signature_of(kKeySeed, example))["body"]["code"].text == "image.bad_signature");
	CHECK(socket_put_image(f, 14, example, signature_of(kNewSeed, example))["status"].number == 200);
	// Nothing is pending any more.
	r = f.request(HttpMethod::post, "/api/ehome/key/accept",
		std::string("{\"fingerprint\":\"") + kNewFingerprint + "\"}", f.cookie);
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "ehome.no_pending_key");
	// The key in force offered again: accepted at once, nothing pending.
	f.client.deliver(std::string("{\"type\":\"key\",\"public_key\":\"") + kNewKey + "\"}");
	CHECK(f.client.sent("key_accepted").size() == 2);
	CHECK_FALSE(f.status().has("pending_key"));
}

TEST_CASE("E-Home key: a disconnect drops the pending key; a device without a key gets one only by acceptance") {
	EhomeFixture f; // enrolled before the key pairing: no key
	f.handshake();
	f.client.deliver(std::string("{\"type\":\"key\",\"public_key\":\"") + kKey + "\"}");
	CHECK(f.status()["pending_key"].text == kKeyFingerprint);
	CHECK_FALSE(f.status().has("key"));
	// Unsigned images still go in: the pending key is not in force.
	CHECK(socket_put_image(f, 20, image_fixture("minimal.json"), "")["status"].number == 200);
	f.client.drop("connection reset");
	CHECK_FALSE(f.status().has("pending_key"));
	CHECK(f.capture.sink.contains("I hc.ehome: the offered installation key ce7d 4a41 a476 9ee3 was not accepted and "
		"is dropped: the connection ended"));
	const char* code = nullptr;
	const char* error = nullptr;
	CHECK_FALSE(f.link.accept_key(nullptr, &code, &error));
	CHECK(std::string(code) == "ehome.no_pending_key");
	CHECK(stored_key(f).empty());
	// Offered again after the reconnect, accepted on the console (no fingerprint: the console showed it).
	f.advance(1100);
	f.handshake();
	f.client.deliver(std::string("{\"type\":\"key\",\"public_key\":\"") + kKey + "\"}");
	CHECK(f.link.accept_key(nullptr, &code, &error));
	CHECK(stored_key(f) == kKey);
	CHECK(socket_put_image(f, 21, image_fixture("minimal.json"), "")["body"]["code"].text == "image.unsigned");
}

TEST_CASE("E-Home key: an unusable key message is answered ehome.bad_key and changes nothing") {
	EhomeFixture f;
	REQUIRE(f.fake.settings.set_string("ehome.key", kKey) == hc::SettingsStatus::ok);
	f.handshake();
	f.client.deliver("{\"type\":\"key\",\"public_key\":\"abcd\"}");
	auto errors = f.client.sent("error");
	REQUIRE(errors.size() == 1);
	CHECK(errors[0]["code"].text == "ehome.bad_key");
	f.client.deliver(std::string("{\"type\":\"key\",\"public_key\":\"") + kNewKey +
		"\",\"fingerprint\":\"ce7d 4a41 a476 9ee3\"}"); // the fingerprint of another key
	errors = f.client.sent("error");
	REQUIRE(errors.size() == 2);
	CHECK(errors[1]["code"].text == "ehome.bad_key");
	CHECK_FALSE(f.status().has("pending_key"));
	CHECK(stored_key(f) == kKey);
	CHECK(f.link.state() == EhomeState::connected);
}

TEST_CASE("E-Home key: forgetting E-Home keeps the key in force; only a new enrollment's answer replaces it") {
	EhomeFixture f(false);
	REQUIRE(f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"ABCD-EFGH\"}",
		[&] { f.fetcher.answer(200, enroll_answer_with_key(kKey)); }).status == 200);
	f.handshake();
	// A rotation is pending when the owner forgets E-Home: the pending key goes, the stored one stays.
	f.client.deliver(std::string("{\"type\":\"key\",\"public_key\":\"") + kNewKey + "\"}");
	REQUIRE(f.status()["pending_key"].text == kNewFingerprint);
	REQUIRE(f.del("/api/ehome").status == 200);
	CHECK_FALSE(hc::ehome_enrolled(f.fake.settings));
	char value[130];
	for (const char* key : {"ehome.id", "ehome.secret", "ehome.socket"}) {
		CHECK(f.fake.settings.get_string(key, value, sizeof(value)) == hc::SettingsStatus::not_found);
	}
	CHECK_FALSE(f.status().has("pending_key"));
	CHECK(f.status()["key"].text == kKeyFingerprint);
	CHECK(f.capture.sink.contains("I hc.ehome: the installation key ce7d 4a41 a476 9ee3 stays in force"));
	// With a token alone, forgetting E-Home does not open the door to unsigned images.
	std::string minimal = image_fixture("minimal.json");
	auto r = f.put("/api/image", minimal);
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "image.unsigned");
	CHECK(r.json["refusals"].items[0]["reason"].text.find(kKeyFingerprint) != std::string::npos);
	f.image_signature = signature_of(kNewSeed, minimal); // the never-accepted rotation's key
	r = f.put("/api/image", minimal);
	f.image_signature.clear();
	CHECK(r.json["code"].text == "image.bad_signature");
	CHECK_FALSE(f.service.loaded());
	// A failed enrollment keeps the key.
	r = f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"WRONG\"}", [&] { f.fetcher.answer(404, ""); });
	CHECK(r.status == 404);
	CHECK(stored_key(f) == kKey);
	// A new enrollment with another key: the new key is in force, the old one refused.
	REQUIRE(f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"ABCD-EFGH\"}",
		[&] { f.fetcher.answer(200, enroll_answer_with_key(kNewKey)); }).status == 200);
	CHECK(stored_key(f) == kNewKey);
	CHECK(f.status()["key"].text == kNewFingerprint);
	f.image_signature = signature_of(kKeySeed, minimal);
	CHECK(f.put("/api/image", minimal).json["code"].text == "image.bad_signature");
	f.image_signature = signature_of(kNewSeed, minimal);
	CHECK(f.put("/api/image", minimal).status == 200);
	f.image_signature.clear();
	// A new enrollment without a key leaves the device without one (the authenticated answer decides).
	REQUIRE(f.del("/api/ehome").status == 200);
	REQUIRE(f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"ABCD-EFGH\"}",
		[&] { f.fetcher.answer(200, kEnrollAnswer); }).status == 200);
	CHECK(stored_key(f).empty());
	CHECK(f.put("/api/image", minimal).status == 200);
}

// ---- Probe: the signature on the socket's text-body form, and a key offer that changes nothing ------

TEST_CASE("E-Home key probe: a small image sent as the request's text body honours the signature too") {
	EhomeFixture f;
	REQUIRE(f.fake.settings.set_string("ehome.key", kKey) == hc::SettingsStatus::ok);
	f.handshake();
	std::string minimal = image_fixture("minimal.json");
	REQUIRE(minimal.size() < 4096);
	// Below the text limit E-Home may send the body inline (no length, no binary frames): the
	// signature must count there exactly as with the frames.
	auto text_put = [&](int id, const std::string& signature) {
		std::string request = "{\"type\":\"request\",\"id\":" + std::to_string(id) +
			",\"method\":\"PUT\",\"path\":\"/api/image\",\"body\":" + minimal;
		if (!signature.empty()) {
			request += ",\"signature\":\"" + signature + "\"";
		}
		f.client.deliver(request + "}");
		return f.response(id);
	};
	Json r = text_put(20, "");
	CHECK(r["status"].number == 403);
	CHECK(r["body"]["code"].text == "image.unsigned");
	CHECK_FALSE(f.service.loaded());
	// Any signature on the inline form is refused up front, whichever key made it (the device cannot
	// tell a wrong key from bytes it does not have).
	r = text_put(21, signature_of(kNewSeed, minimal));
	CHECK(r["status"].number == 400);
	CHECK(r["body"]["code"].text == "image.bad_request");
	// The inline form cannot carry the exact bytes a signature covers (the body is parsed JSON):
	// a signed image goes as binary frames, and the device says so instead of failing the check.
	r = text_put(22, signature_of(kKeySeed, minimal));
	CHECK(r["status"].number == 400);
	CHECK(r["body"]["code"].text == "image.bad_request");
	CHECK(r["body"]["error"].text == "a signed image is sent with length and binary frames: the signature covers its exact bytes");
	CHECK_FALSE(f.service.loaded());
	CHECK(socket_put_image(f, 22, minimal, signature_of(kKeySeed, minimal))["status"].number == 200);
	CHECK(f.service.loaded());

	// E-Home offers the key the device already holds: nothing becomes pending, the answer says so,
	// and images keep working with it.
	f.client.deliver(std::string("{\"type\":\"key\",\"public_key\":\"") + kKey + "\",\"fingerprint\":\"" +
		kKeyFingerprint + "\"}");
	CHECK_FALSE(f.status().has("pending_key"));
	CHECK(f.status()["key"].text == kKeyFingerprint);
	CHECK(f.client.sent("key_pending").empty());
	CHECK(f.client.sent("key_accepted").size() + f.client.sent("error").size() >= 1);
	CHECK(socket_put_image(f, 23, image_fixture("example.json"), signature_of(kKeySeed, image_fixture("example.json")))["status"].number == 200);
	CHECK(f.link.state() == EhomeState::connected);
}
