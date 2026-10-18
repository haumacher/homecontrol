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
