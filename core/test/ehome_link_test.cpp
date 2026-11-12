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
#include "fake_image_staging.h"
#include "fake_worker.h"
#include "hc/image/image.h"
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
		if (!session || fail_sends || length > room) {
			return false;
		}
		take(length);
		texts.push_back(std::string(text, length));
		order.push_back(texts.back());
		return true;
	}
	bool send_binary(const uint8_t* data, size_t length) override {
		if (!session || length > room) {
			return false;
		}
		take(length);
		binary.append(reinterpret_cast<const char*>(data), length);
		++binary_messages;
		order.push_back("<binary>");
		return true;
	}
	bool send_ping() override {
		if (!session || room == 0) {
			return false;
		}
		++pings;
		return true;
	}
	size_t send_room() override { return session ? room : 0; }
	void want_writable() override { ++writable_wanted; }
	std::string claim;
	char* claim_text(size_t max) override {
		if (!session || fail_sends || max > room) {
			return nullptr;
		}
		claim.assign(max, '\0');
		return &claim[0];
	}
	void commit_text(size_t length) override { send_text(claim.data(), length); }
	void abandon_text() override {}
	void close(uint16_t code, const char* reason) override {
		closes.push_back({code, reason});
		session = false;
	}

	// ---- A bounded send queue (room == SIZE_MAX: unbounded, as on POSIX) ----
	void take(size_t length) {
		if (room != SIZE_MAX) {
			room -= length;
			queued += length;
		}
	}
	/// The queue sent everything: its room is `capacity` again, the link hears of it if it asked.
	void drain(size_t capacity) {
		room = capacity;
		queued = 0;
		if (writable_wanted > 0) {
			writable_wanted = 0;
			listener_->ws_client_writable();
		}
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
	size_t room = SIZE_MAX;
	size_t queued = 0;
	int writable_wanted = 0;
	std::vector<std::string> order; ///< Every message sent, in order ("<binary>" for a binary one).
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
		CHECK(auth.back()["schema"].text == hc::image::schema_version_text()); // the newest schema it reads
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
		{"PUT", "/api/settings/update.channel", "{\"value\":\"beta\"}"},
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

TEST_CASE("E-Home: an image larger than the staging area is refused 413 naming the limit, its frames dropped (#52)") {
	EhomeFixture f;
	hc_test::FakeImageStaging staging;
	staging.capacity_bytes = 126976;
	f.service.set_staging(&staging);
	f.handshake();
	f.client.deliver("{\"type\":\"request\",\"id\":1,\"method\":\"PUT\",\"path\":\"/api/image\","
		"\"length\":130000}");
	CHECK(f.response(1)["status"].number == 413);
	CHECK(f.response(1)["body"]["code"].text == "api.body_too_large");
	CHECK(f.response(1)["body"]["error"].text ==
		"the image has 130000 bytes; the device receives at most 126975 (its staging area)");
	CHECK(f.capture.sink.contains("the image has 130000 bytes; the device receives at most 126975 (its staging area)"));
	CHECK(staging.begins == 0); // nothing was staged
	f.client.deliver_binary(std::string(130000, 'x'), 4096);
	CHECK(f.link.state() == EhomeState::connected);
	f.client.deliver("{\"type\":\"request\",\"id\":2,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK(f.response(2)["status"].number == 200);
	CHECK(f.response(2)["body"]["image"]["storable"].number > 0);
	f.service.set_staging(nullptr);
}

TEST_CASE("E-Home: a body whose frames stop is reported once with the bytes received") {
	EhomeFixture f;
	f.handshake();
	f.client.deliver("{\"type\":\"request\",\"id\":1,\"method\":\"PUT\",\"path\":\"/api/image\","
		"\"length\":3000}");
	REQUIRE(f.capture.sink.contains("receiving 3000 bytes"));
	f.client.deliver_binary(std::string(1000, ' '), 1000);
	f.advance(EhomeLink::kBodyStallMs - 1000);
	CHECK_FALSE(f.capture.sink.contains("the body stalls"));
	f.advance(2000);
	CHECK(f.capture.sink.contains("request 1 /api/image: the body stalls at 1000 of 3000 bytes"));
	f.advance(2000);
	CHECK(f.capture.sink.count("the body stalls") == 1);
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

// ---- The bounded send queue (issue #55) --------------------------------------------------------

namespace {

/// Loads the example image over the socket (with an unbounded queue): 15 datapoints, a snapshot of
/// several chunks.
void load_example(EhomeFixture& f) {
	std::string image = image_fixture("example.json");
	f.client.deliver("{\"type\":\"request\",\"id\":100,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(image.size()) + "}");
	f.client.deliver_binary(image, 1024);
	REQUIRE(f.response(100)["status"].number == 200);
}

} // namespace

TEST_CASE("E-Home queue: the snapshot takes the send queue only beside the reserve, resumes on writable, whole and in order") {
	EhomeFixture f;
	f.handshake();
	load_example(f);
	f.run();
	// A queue with room for one chunk beside the reserve (as the ESP client's).
	const size_t capacity = EhomeLink::kBulkReserve + hc::LiveFeed::kMessageSize + 200;
	f.client.drain(capacity);
	f.client.texts.clear();
	f.client.deliver("{\"type\":\"subscribe\"}");
	f.run();
	CHECK(f.client.sent("status").size() == 1); // a single message takes the reserve too
	// The status leaves no room for a chunk beside the reserve: the snapshot waits for writable.
	std::vector<Json> chunks = f.client.sent("datapoints");
	CHECK(chunks.empty());
	CHECK(f.client.writable_wanted > 0);
	for (int turn = 0; turn < 20 && (chunks.empty() || !chunks.back()["done"].boolean); ++turn) {
		size_t had = chunks.size();
		f.client.drain(capacity);
		f.run();
		chunks = f.client.sent("datapoints");
		CHECK(chunks.size() <= had + 1); // one chunk per drained queue: the second would eat into the reserve
	}
	REQUIRE(chunks.size() >= 2);
	double next = 0;
	for (const Json& c : chunks) {
		CHECK(c["snapshot"].boolean);
		CHECK(c["from"].number == next); // in order, nothing skipped, nothing twice
		next = c["from"].number + static_cast<double>(c["updates"].items.size());
	}
	CHECK(chunks.back()["done"].boolean);
	CHECK(next == chunks.back()["count"].number);
	CHECK_FALSE(f.capture.sink.contains("full resync follows"));
	CHECK(f.link.state() == EhomeState::connected);
}

TEST_CASE("E-Home queue: an answer without room waits, goes out before the feed's next message, keeps the request slot") {
	EhomeFixture f;
	f.handshake();
	load_example(f);
	f.client.deliver("{\"type\":\"subscribe\"}");
	f.run();
	REQUIRE(f.client.sent("datapoints").back()["done"].boolean);

	f.client.room = 64; // the queue is nearly full
	f.client.deliver("{\"type\":\"request\",\"id\":7,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK_FALSE(f.response(7).has("status"));
	CHECK(f.capture.sink.contains("request 7: the answer"));
	// The slot stays taken: the next request is refused (its short answer fits).
	f.client.room = 1000;
	f.client.deliver("{\"type\":\"request\",\"id\":8,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK(f.response(8)["status"].number == 429);
	// The feed waits behind the held answer: an update is not sent before it.
	size_t before = f.client.order.size();
	f.client.deliver("{\"type\":\"ping\"}");
	CHECK(f.client.order.size() == before); // the feed's pong is held back too
	f.client.drain(SIZE_MAX);
	f.run();
	REQUIRE(f.response(7)["status"].number == 200);
	size_t answer = 0;
	for (size_t i = 0; i < f.client.order.size(); ++i) {
		Json m = parse(f.client.order[i]);
		if (m["type"].text == "response" && m["id"].number == 7) {
			answer = i;
		}
	}
	CHECK(answer == before); // the first message after the queue drained
	// The feed resynchronizes the client that missed a message, after the answer.
	CHECK(f.client.sent("datapoints").back()["done"].boolean);
	CHECK(f.link.state() == EhomeState::connected);
	// The slot is free again.
	f.client.deliver("{\"type\":\"request\",\"id\":9,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK(f.response(9)["status"].number == 200);
}

TEST_CASE("E-Home queue: an answer that finds no room for kHoldMs ends the connection with the reason") {
	EhomeFixture f;
	f.handshake();
	f.client.room = 16;
	f.client.deliver("{\"type\":\"request\",\"id\":7,\"method\":\"GET\",\"path\":\"/api/status\"}");
	f.advance(EhomeLink::kHoldMs - 1000);
	CHECK(f.link.state() == EhomeState::connected);
	CHECK(f.client.closes.empty());
	f.advance(2000);
	CHECK(f.client.closes.size() == 1);
	CHECK(f.capture.sink.contains("the answer to request 7 ("));
	CHECK(f.capture.sink.contains("found no room in the send queue within 10 s"));
}

TEST_CASE("E-Home queue: a streamed answer waits for room piece by piece; a full queue delays the ping, never fails it") {
	EhomeFixture f;
	f.handshake();
	load_example(f);
	std::string image = image_fixture("example.json");
	f.client.binary.clear();
	f.client.room = EhomeLink::kStreamPiece + 100;
	f.client.deliver("{\"type\":\"request\",\"id\":9,\"method\":\"GET\",\"path\":\"/api/image\"}");
	CHECK(f.response(9)["status"].number == 200);
	f.run();
	CHECK(f.client.binary.size() == EhomeLink::kStreamPiece); // the second piece waits
	for (int turn = 0; turn < 20 && f.client.binary.size() < image.size(); ++turn) {
		f.client.drain(EhomeLink::kStreamPiece + 100);
		f.run();
	}
	CHECK(f.client.binary == image);
	CHECK(f.capture.sink.contains("request 9: " + std::to_string(image.size()) + " bytes sent"));

	f.client.room = 0;
	f.advance(EhomeLink::kPingMs);
	CHECK(f.client.pings == 0);
	CHECK(f.link.state() == EhomeState::connected);
	f.client.drain(SIZE_MAX);
	f.advance(1000);
	CHECK(f.client.pings == 1);
	CHECK(f.link.state() == EhomeState::connected);
}

TEST_CASE("E-Home queue probe: a drop mid-snapshot with an answer held; the new session starts clean") {
	// homecontrol #55 review: nothing of the old session (the held answer, the rest of its
	// snapshot) reaches the new one, the request slot is free again, and the new session's
	// snapshot is whole from the first datapoint.
	EhomeFixture f;
	f.handshake();
	load_example(f);
	f.run();
	const size_t capacity = EhomeLink::kBulkReserve + hc::LiveFeed::kMessageSize + 200;
	f.client.drain(capacity);
	f.client.deliver("{\"type\":\"subscribe\"}");
	f.run();
	f.client.drain(capacity);
	f.run();
	REQUIRE_FALSE(f.client.sent("datapoints").empty());
	REQUIRE_FALSE(f.client.sent("datapoints").back()["done"].boolean);
	f.client.room = 16; // an answer finds no room and is held
	f.client.deliver("{\"type\":\"request\",\"id\":7,\"method\":\"GET\",\"path\":\"/api/status\"}");
	REQUIRE_FALSE(f.response(7).has("status"));

	f.client.drop("network gone");
	f.run();
	CHECK(f.link.state() == EhomeState::failed);
	f.client.texts.clear();
	f.client.order.clear();
	f.client.room = SIZE_MAX;
	f.advance(EhomeLink::kHoldMs + 2000); // past the hold limit and the backoff
	CHECK(f.client.connects == 2);
	f.handshake();
	CHECK(f.link.state() == EhomeState::connected);
	f.run();
	for (const std::string& m : f.client.order) {
		Json json = parse(m);
		bool old_answer = json["type"].text == "response" && json["id"].number == 7;
		CHECK_FALSE(old_answer); // never the old answer
		CHECK(json["type"].text != "datapoints"); // no snapshot before the new subscribe
	}
	f.client.deliver("{\"type\":\"subscribe\"}");
	f.run();
	std::vector<Json> chunks = f.client.sent("datapoints");
	REQUIRE_FALSE(chunks.empty());
	CHECK(chunks.front()["from"].number == 0);
	CHECK(chunks.back()["done"].boolean);
	f.client.deliver("{\"type\":\"request\",\"id\":9,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK(f.response(9)["status"].number == 200); // the slot is free
}

TEST_CASE("E-Home image staging: a signed image is received into the staging area, verified and loaded from it") {
	// homecontrol #66: a deploy needs no heap block of the image's size.
	EhomeFixture f;
	hc_test::FakeImageStaging staging;
	f.service.set_staging(&staging);
	REQUIRE(f.fake.settings.set_string("ehome.key", kKey) == hc::SettingsStatus::ok);
	f.handshake();
	std::string example = image_fixture("example.json");
	size_t compiled = f.capture.sink.count("I hc.rule: rules loaded:");
	Json r = socket_put_image(f, 4, example, signature_of(kKeySeed, example));
	CHECK(r["status"].number == 200);
	CHECK(f.service.loaded());
	// The rules were compiled once: the loader's check hands its set to the engine.
	CHECK(f.capture.sink.count("I hc.rule: rules loaded:") == compiled + 1);
	CHECK(staging.begins == 1);
	CHECK(staging.appends >= 3); // a piece per frame, and the NUL
	CHECK(staging.ends == 1);    // released when the request is answered
	CHECK_FALSE(staging.busy);
	CHECK(staging.content.size() == example.size() + 1);
	CHECK(f.capture.sink.contains("receiving " + std::to_string(example.size()) + " bytes into the staging area"));
	CHECK(f.capture.sink.contains("I hc.image:   steps: signature "));
	// The loaded image does not point into the staging area: overwritten after end(), it still serves.
	for (const char* path : {"/api/image", "/api/rules", "/api/datapoints", "/api/schedules", "/api/pages"}) {
		auto answer = f.get(path, f.cookie);
		INFO(path);
		CHECK(answer.status == 200);
		CHECK(answer.body.find("XXXX") == std::string::npos);
	}
	// A mis-signed image is refused from the staging area as from the heap.
	r = socket_put_image(f, 5, example, signature_of(kNewSeed, example));
	CHECK(r["status"].number == 403);
	CHECK(staging.ends == 2);

	// Another request's body never goes there.
	int begins = staging.begins;
	f.client.deliver("{\"type\":\"request\",\"id\":6,\"method\":\"PUT\",\"path\":\"/api/ehome\",\"length\":2}");
	f.client.deliver_binary("{}", 1024);
	CHECK(staging.begins == begins);
}

TEST_CASE("E-Home image staging: when the area declines, the image is received into the heap with the reason logged") {
	EhomeFixture f;
	hc_test::FakeImageStaging staging;
	staging.off = "the updater's image does not verify";
	f.service.set_staging(&staging);
	f.handshake();
	std::string image = image_fixture("minimal.json");
	CHECK(socket_put_image(f, 3, image, "")["status"].number == 200);
	CHECK(f.capture.sink.contains("the body goes to the heap: the updater's image does not verify"));

	// An area that fails while the body arrives: the request is refused, the connection stays.
	staging.off = nullptr;
	staging.fail_after = 1;
	std::string example = image_fixture("example.json");
	Json r = socket_put_image(f, 4, example, "");
	CHECK(r["status"].number == 507);
	CHECK(f.capture.sink.contains("the staging area failed while the body arrived"));
	CHECK_FALSE(staging.busy);
	staging.fail_after = 0;
	r = socket_put_image(f, 5, example, "");
	CHECK(r["status"].number == 200);
}

TEST_CASE("E-Home image store: the answer to PUT /api/image goes out when the image is stored, a failure with its reason") {
	// homecontrol #68: the store runs off the loop; E-Home hears "stored" only when it is.
	EhomeFixture f;
	hc_test::FakeImageStaging staging;
	hc_test::ManualWorker worker;
	f.service.set_staging(&staging);
	f.service.set_worker(&worker);
	f.handshake();
	std::string example = image_fixture("example.json");
	f.client.deliver("{\"type\":\"request\",\"id\":4,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(example.size()) + "}");
	f.client.deliver_binary(example, 1024);
	CHECK(f.service.loaded()); // runs
	CHECK(f.service.store_pending());
	CHECK_FALSE(f.response(4).has("status")); // not answered before it is stored
	CHECK(f.capture.sink.contains("request 4: /api/image runs, its answer follows when it is stored"));
	// The link stays responsive; a further request waits for the running one as always.
	f.client.deliver("{\"type\":\"ping\"}");
	CHECK(f.client.sent("pong").size() == 1);
	f.client.deliver("{\"type\":\"request\",\"id\":5,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK(f.response(5)["status"].number == 429);
	CHECK(staging.busy); // the service reads the body from the staging area

	worker.step();
	Json r = f.response(4);
	CHECK(r["status"].number == 200);
	CHECK(r["body"]["stored"].boolean);
	CHECK(r["body"]["image"]["revision"].number == 7);
	CHECK_FALSE(staging.busy);
	f.client.deliver("{\"type\":\"request\",\"id\":6,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK(f.response(6)["status"].number == 200); // the slot is free again

	// A store the flash refuses: the answer says so, with the reason.
	std::string next = example;
	next.replace(next.find("\"revision\": 7"), 13, "\"revision\": 8");
	f.client.deliver("{\"type\":\"request\",\"id\":7,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(next.size()) + "}");
	f.client.deliver_binary(next, 1024);
	CHECK_FALSE(f.response(7).has("status"));
	f.blobs.fail_writes = true;
	worker.step();
	f.blobs.fail_writes = false;
	r = f.response(7);
	CHECK(r["status"].number == 200);
	CHECK_FALSE(r["body"]["stored"].boolean);
	CHECK(r["body"]["storeError"].text == "the storage refused a write");
	CHECK(r["body"]["image"]["revision"].number == 8);
}

TEST_CASE("E-Home image store: a deploy while an HTTP deploy is being stored is refused 503 image.busy, its frames dropped") {
	EhomeFixture f;
	hc_test::ManualWorker worker;
	f.service.set_worker(&worker);
	f.handshake();
	// An HTTP deploy whose store is pending (the receiver hands its body over).
	std::string example = image_fixture("example.json");
	hc::image::ImageBody body;
	REQUIRE(body.begin(example.size(), nullptr));
	REQUIRE(body.append(example.data(), example.size()));
	hc::WebRequest req;
	req.method = HttpMethod::put;
	req.path = "/api/image";
	req.query = "";
	req.cookie = f.cookie.c_str();
	req.requested_with = "HomeControl";
	req.source = "192.0.2.7";
	req.body = body.finish();
	req.body_length = example.size();
	req.body_holder = &body;
	char out[4096];
	hc::WebResponse http(out, sizeof(out));
	f.api.handle(req, http);
	REQUIRE(http.deferred);
	CHECK_FALSE(body.active()); // taken over

	f.client.deliver("{\"type\":\"request\",\"id\":3,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(example.size()) + "}");
	Json r = f.response(3);
	CHECK(r["status"].number == 503);
	CHECK(r["body"]["code"].text == "image.busy");
	f.client.deliver_binary(example, 1024); // dropped, the link stays up
	CHECK(f.link.state() == EhomeState::connected);
	f.client.deliver("{\"type\":\"request\",\"id\":4,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK(f.response(4)["status"].number == 200);

	worker.step();
	REQUIRE(f.api.poll_deferred(http));
	CHECK(http.status == 200);
	CHECK_FALSE(socket_put_image(f, 5, example, "").has("status")); // now E-Home's turn: deferred
	worker.step();
	CHECK(f.response(5)["body"]["stored"].boolean);
}

TEST_CASE("E-Home image store: the session ends while the image is stored; the store completes, the next session deploys") {
	EhomeFixture f;
	hc_test::ManualWorker worker;
	f.service.set_worker(&worker);
	f.handshake();
	std::string example = image_fixture("example.json");
	socket_put_image(f, 4, example, "");
	CHECK(f.service.store_pending());
	f.client.drop("connection reset");
	CHECK(f.capture.sink.contains("the deferred answer to request 4 is dropped with the session"));
	worker.step(); // the store goes on and ends
	CHECK(f.service.stored_ok());
	CHECK_FALSE(f.response(4).has("status"));
	f.advance(1100);
	f.handshake();
	CHECK_FALSE(socket_put_image(f, 5, example, "").has("status")); // deferred: no answer yet
	worker.step();
	CHECK(f.response(5)["status"].number == 200);
	CHECK(f.response(5)["body"]["stored"].boolean);
}

// ---- The bus session (homecontrol #85, spec/api.md "Bus session") ------------------------------

namespace {

using hc_test::Bytes;
using hc_test::bytes;
using hc_test::hex;

/// The tunnel's address (1.1.250, from connect_response()) and a device's (1.1.5).
/// E-Home's T_Connect to 1.1.5, as Calimero builds it (system priority, hop count 6, source 0.0.0).
constexpr const char* kConnect = "11 00 B0 60 00 00 11 05 00 80";
/// The gateway's L_Data.con of it (the source filled in), and the negative one.
constexpr const char* kConnectCon = "2E 00 B0 60 11 FA 11 05 00 80";
constexpr const char* kConnectNegative = "2E 00 B1 60 11 FA 11 05 00 80";
/// The device's T_ACK to the tunnel's address.
constexpr const char* kAckFromDevice = "29 00 B0 60 11 05 11 FA 00 C2";
/// A_IndividualAddress_Response of 1.1.5, a broadcast.
constexpr const char* kAddressResponse = "29 00 B0 E0 11 05 00 00 01 01 40";
/// A group write to 1/0/1 (on).
constexpr const char* kGroupWrite = "29 00 BC E0 11 05 08 01 01 00 81";
/// A T_Connect between two other devices.
constexpr const char* kOtherConnect = "29 00 B0 60 11 05 11 06 00 80";

std::string compact(const char* spaced) {
	std::string out;
	for (const char* p = spaced; *p != '\0'; ++p) {
		if (*p != ' ') {
			out += *p;
		}
	}
	return out;
}

std::string upper_hex(const Bytes& octets) {
	return compact(hex(octets).c_str());
}

/// A TUNNELING_REQUEST of the gateway with any cEMI length (the support's helper takes one octet).
Bytes gateway_request(uint8_t sequence, const Bytes& cemi) {
	size_t total = 10 + cemi.size();
	Bytes result{0x06, 0x10, 0x04, 0x20, static_cast<uint8_t>(total >> 8), static_cast<uint8_t>(total), 0x04, 7, sequence, 0};
	result.insert(result.end(), cemi.begin(), cemi.end());
	return result;
}

/// An L_Data frame (`code` 11 req, 29 ind, 2E con) of an extended frame with an APDU of `apdu`
/// octets (an A_Memory_Write in a connection), from `source` to `destination` (individual).
Bytes extended(uint8_t code, size_t apdu, uint16_t source, uint16_t destination) {
	Bytes frame{code, 0x00, 0x30, 0x60, static_cast<uint8_t>(source >> 8), static_cast<uint8_t>(source),
		static_cast<uint8_t>(destination >> 8), static_cast<uint8_t>(destination), static_cast<uint8_t>(apdu), 0x42};
	for (size_t i = 0; i < apdu; ++i) {
		frame.push_back(static_cast<uint8_t>(i == 0 ? 0x80 | (apdu - 3) : i));
	}
	return frame;
}

struct BusFixture : EhomeFixture {
	hc_test::TunnelRecorder recorder;
	uint8_t gateway_sequence = 0; ///< The gateway's next TUNNELING_REQUEST.
	size_t heartbeats_answered = 0;

	explicit BusFixture(bool with_tunnel = true) {
		if (with_tunnel) {
			api.set_knx(&tunnel, nullptr);
			tunnel.add_listener(&recorder);
			std::optional<hc::knx::GatewayAddress> gateway = hc::knx::GatewayAddress::parse("192.0.2.1");
			REQUIRE(gateway);
			tunnel.set_gateway(&*gateway, false);
			tunnel.start();
			run();
			socket.inject(hc_test::connect_response(7));
			REQUIRE(tunnel.is_up());
		}
		handshake();
	}

	void bus(const std::string& members) {
		client.deliver("{\"type\":\"bus\"," + members + "}");
		run();
	}

	/// The bus messages of `event` sent so far.
	std::vector<Json> events(const std::string& event) const {
		std::vector<Json> out;
		for (const Json& m : client.sent("bus")) {
			if (m["event"].text == event) {
				out.push_back(m);
			}
		}
		return out;
	}

	/// The bus messages' events in order.
	std::vector<std::string> order() const {
		std::vector<std::string> out;
		for (const Json& m : client.sent("bus")) {
			out.push_back(m["event"].text);
		}
		return out;
	}

	Json last(const std::string& event) const {
		std::vector<Json> all = events(event);
		return all.empty() ? Json() : all.back();
	}

	void open() {
		bus("\"op\":\"open\",\"ref\":1");
		REQUIRE(link.bus().is_open());
	}

	void send(int ref, const std::string& frame) { bus("\"op\":\"send\",\"ref\":" + std::to_string(ref) + ",\"frame\":\"" + frame + "\""); }

	/// The TUNNELING_REQUESTs the tunnel sent, as their cEMI octets.
	std::vector<Bytes> requests() const {
		std::vector<Bytes> out;
		for (const auto& d : socket.sent) {
			if (d.data.size() > 10 && d.data[2] == 0x04 && d.data[3] == 0x20) {
				out.push_back(Bytes(d.data.begin() + 10, d.data.end()));
			}
		}
		return out;
	}

	/// The gateway acknowledges the tunnel's request with `sequence`.
	void ack(uint8_t sequence) {
		socket.inject(hc_test::tunneling_ack(7, sequence));
		run();
	}

	/// The gateway sends a cEMI frame (an indication or a confirmation).
	void gateway(const Bytes& cemi) {
		socket.inject(gateway_request(gateway_sequence++, cemi));
		run();
	}

	/// Waits `ms` in steps of 1 s, playing E-Home's pongs and the gateway's heartbeat answers.
	void wait(uint64_t ms) {
		for (uint64_t waited = 0; waited < ms; waited += 1000) {
			advance(1000);
			client.pong();
			size_t heartbeats = socket.count(hc::knx::ip::Service::connectionstate_request);
			if (heartbeats > heartbeats_answered) {
				heartbeats_answered = heartbeats;
				socket.inject(hc_test::connectionstate_response(7));
			}
		}
	}
};

} // namespace

TEST_CASE("Bus session: the status says the firmware has it; open answers the tunnel's address and the longest frame") {
	BusFixture f;
	Json status = f.status();
	REQUIRE(status.has("bus"));
	CHECK_FALSE(status["bus"]["open"].boolean);
	CHECK_FALSE(f.link.bus().holds_memory()); // nothing taken before a session

	f.open();
	Json opened = f.last("opened");
	CHECK(opened["ref"].number == 1);
	CHECK(opened["address"].text == "1.1.250");
	CHECK(opened["max_frame"].number == 264);
	CHECK(opened["max_apdu"].number == 254);
	CHECK(opened["idle_s"].number == 60);
	CHECK(f.capture.sink.contains("I hc.ehome: bus session opened by E-Home: tunnel address 1.1.250"));
	status = f.status();
	CHECK(status["bus"]["open"].boolean);
	CHECK(status["bus"]["address"].text == "1.1.250");
	CHECK(status["bus"]["sent"].number == 0);
	CHECK(f.link.bus().holds_memory());
	// Not for the live feed: no ws.unknown_type.
	CHECK(f.client.sent("error").empty());
}

TEST_CASE("Bus session: a send is forwarded as it is and answered with each outcome of the tunnel") {
	BusFixture f;
	f.open();

	// Confirmed: the TUNNELING_REQUEST carries E-Home's octets unchanged.
	f.send(10, compact(kConnect));
	REQUIRE(f.requests().size() == 1);
	CHECK(hex(f.requests()[0]) == kConnect);
	CHECK(hex(f.socket.last().data) == std::string("06 10 04 20 00 14 04 07 00 00 ") + kConnect);
	CHECK(f.events("sent").empty()); // the outcome comes with the confirmation
	f.ack(0);
	f.gateway(bytes(kConnectCon));
	Json sent = f.last("sent");
	CHECK(sent["ref"].number == 10);
	CHECK(sent["outcome"].text == "confirmed");

	// Negative.
	f.send(11, compact(kConnect));
	f.ack(1);
	f.gateway(bytes(kConnectNegative));
	CHECK(f.last("sent")["ref"].number == 11);
	CHECK(f.last("sent")["outcome"].text == "negative");

	// No confirmation within 3 s.
	f.send(12, compact(kConnect));
	f.ack(2);
	f.wait(3000);
	CHECK(f.last("sent")["ref"].number == 12);
	CHECK(f.last("sent")["outcome"].text == "no_confirmation");

	// The confirmation that arrives before the gateway's ACK counts as well.
	f.send(13, compact(kConnect));
	f.gateway(bytes(kConnectCon));
	CHECK(f.last("sent")["ref"].number == 12);
	f.ack(3);
	CHECK(f.last("sent")["ref"].number == 13);
	CHECK(f.last("sent")["outcome"].text == "confirmed");
	CHECK(f.status()["bus"]["sent"].number == 4);
	CHECK(f.capture.sink.contains("I hc.ehome: bus session: frame 11 negative confirmation"));
}

TEST_CASE("Bus session: indications to the tunnel's address and broadcasts go to E-Home in order; group telegrams do not") {
	BusFixture f;
	f.open();
	// Three in one loop turn: one message, in the order received.
	f.socket.inject(gateway_request(f.gateway_sequence++, bytes(kAckFromDevice)));
	f.socket.inject(gateway_request(f.gateway_sequence++, bytes(kGroupWrite)));
	f.socket.inject(gateway_request(f.gateway_sequence++, bytes(kAddressResponse)));
	f.socket.inject(gateway_request(f.gateway_sequence++, bytes(kOtherConnect)));
	f.run();
	std::vector<Json> frames = f.events("frames");
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0]["frames"].items.size() == 2);
	CHECK(frames[0]["frames"].items[0].text == compact(kAckFromDevice));
	CHECK(frames[0]["frames"].items[1].text == compact(kAddressResponse));
	CHECK_FALSE(frames[0].has("dropped"));
	// The group telegram took its usual way (the tunnel's listeners: the adapter, the monitor's log).
	bool group_seen = false;
	for (const auto& frame : f.recorder.frames) {
		group_seen = group_seen || (frame.group && frame.destination == 0x0801);
	}
	CHECK(group_seen);
	// Later ones follow in later messages, in order.
	f.gateway(bytes(kAckFromDevice));
	f.gateway(bytes(kAddressResponse));
	frames = f.events("frames");
	REQUIRE(frames.size() == 3);
	CHECK(frames[1]["frames"].items[0].text == compact(kAckFromDevice));
	CHECK(frames[2]["frames"].items[0].text == compact(kAddressResponse));
	CHECK(f.status()["bus"]["received"].number == 4);

	// Without a session nothing is forwarded.
	f.bus("\"op\":\"close\"");
	f.gateway(bytes(kAckFromDevice));
	CHECK(f.events("frames").size() == 3);
}

TEST_CASE("Bus session: one at a time; it ends by close, by the end of the connection and by idling") {
	BusFixture f;
	f.open();
	f.bus("\"op\":\"open\",\"ref\":\"again\"");
	Json refused = f.last("refused");
	CHECK(refused["ref"].text == "again");
	CHECK(refused["op"].text == "open");
	CHECK(refused["code"].text == "bus.busy");
	CHECK(f.link.bus().is_open()); // the first one goes on

	// Close: answered with the counts; the memory is given back.
	f.send(5, compact(kConnect));
	f.ack(0);
	f.gateway(bytes(kConnectCon));
	f.gateway(bytes(kAckFromDevice));
	f.bus("\"op\":\"close\",\"ref\":2");
	Json closed = f.last("closed");
	CHECK(closed["ref"].number == 2);
	CHECK(closed["code"].text == "bus.closed");
	CHECK(closed["sent"].number == 1);
	CHECK(closed["received"].number == 1);
	CHECK(closed["dropped"].number == 0);
	CHECK_FALSE(f.link.bus().is_open());
	CHECK_FALSE(f.link.bus().holds_memory());
	CHECK(f.capture.sink.contains("I hc.ehome: bus session ended (closed by E-Home): 1 frames sent, 1 received, 0 dropped"));

	// The end of the connection ends it without a message.
	f.open();
	size_t messages = f.client.sent("bus").size();
	f.client.drop("connection reset");
	CHECK_FALSE(f.link.bus().is_open());
	CHECK(f.client.sent("bus").size() == messages);
	CHECK(f.capture.sink.contains("I hc.ehome: bus session ended (the connection to E-Home ended)"));
	f.advance(1100);
	f.handshake();
	CHECK_FALSE(f.status()["bus"]["open"].boolean);

	// Idling: frames keep it open, 60 s without one end it.
	f.open();
	f.wait(50000);
	f.gateway(bytes(kAckFromDevice)); // an indication counts
	f.wait(50000);
	CHECK(f.link.bus().is_open());
	f.send(6, compact(kConnect)); // and so does a send
	f.ack(1);
	f.gateway(bytes(kConnectCon));
	f.wait(50000);
	CHECK(f.link.bus().is_open());
	f.wait(11000);
	CHECK_FALSE(f.link.bus().is_open());
	closed = f.last("closed");
	CHECK(closed["code"].text == "bus.idle");
	CHECK(closed["reason"].text == "no frame for 60 s");
	CHECK_FALSE(closed.has("ref"));
	CHECK(f.tunnel.is_up());
}

TEST_CASE("Bus session: the controller's own telegrams share the tunnel's queue with the session's frames") {
	BusFixture f;
	f.open();
	// A telegram of the controller's (a rule, a schedule) is on the wire first ...
	hc::knx::LData own;
	uint8_t on = 1;
	REQUIRE(hc::knx::make_group_request(own, hc::knx::GroupAddress::of(1, 0, 1), hc::knx::GroupService::write, &on, 1, true));
	uint32_t token = f.tunnel.send(own);
	REQUIRE(token != 0);
	// ... E-Home's frame waits behind it, then goes, and the controller's next one behind that.
	f.send(20, compact(kConnect));
	uint32_t token2 = f.tunnel.send(own);
	REQUIRE(token2 != 0);
	CHECK(f.requests().size() == 1);
	CHECK(f.tunnel.queued() == 3);
	f.ack(0);
	f.gateway(bytes("2E 00 BC E0 11 FA 08 01 01 00 81"));
	REQUIRE(f.recorder.sent.size() == 1);
	CHECK(f.recorder.sent[0].token == token);
	CHECK(f.recorder.sent[0].outcome == hc::knx::SendOutcome::confirmed);
	REQUIRE(f.requests().size() == 2);
	CHECK(hex(f.requests()[1]) == kConnect);
	f.ack(1);
	f.gateway(bytes(kConnectCon));
	CHECK(f.last("sent")["outcome"].text == "confirmed");
	CHECK(f.recorder.sent.size() == 1); // the session's outcome is not the controller's
	REQUIRE(f.requests().size() == 3);
	f.ack(2);
	f.gateway(bytes("2E 00 BC E0 11 FA 08 01 01 00 81"));
	REQUIRE(f.recorder.sent.size() == 2);
	CHECK(f.recorder.sent[1].token == token2);
}

TEST_CASE("Bus session: extended frames longer than a decoded group telegram pass both ways") {
	BusFixture f;
	f.open();
	// A memory write with 60 octets of APDU: 70 octets of cEMI, beyond LData's 41.
	Bytes write = extended(0x11, 60, 0x0000, 0x1105);
	REQUIRE(write.size() == 70);
	f.send(30, upper_hex(write));
	REQUIRE(f.requests().size() == 1);
	CHECK(f.requests()[0] == write);
	CHECK(f.socket.last().data.size() == 80);
	f.ack(0);
	f.gateway(extended(0x2E, 60, 0x11FA, 0x1105)); // its confirmation is as long
	CHECK(f.last("sent")["outcome"].text == "confirmed");

	// The longest: an APDU of 254 octets, 264 of cEMI; a TUNNELING_REQUEST of 274.
	Bytes longest = extended(0x11, 254, 0x0000, 0x1105);
	REQUIRE(longest.size() == 264);
	f.send(31, upper_hex(longest));
	REQUIRE(f.requests().size() == 2);
	CHECK(f.requests()[1] == longest);
	CHECK(f.socket.last().data.size() == 274);
	CHECK(hex(Bytes(f.socket.last().data.begin(), f.socket.last().data.begin() + 6)) == "06 10 04 20 01 12");
	f.ack(1);
	f.gateway(extended(0x2E, 254, 0x11FA, 0x1105));
	CHECK(f.last("sent")["outcome"].text == "confirmed");

	// A long indication of the device (a memory response) comes to E-Home as it is, with its additional
	// information.
	Bytes response = extended(0x29, 200, 0x1105, 0x11FA);
	response[1] = 2; // two octets of additional information
	response.insert(response.begin() + 2, {0x03, 0x00});
	f.gateway(response);
	REQUIRE(f.events("frames").size() == 1);
	CHECK(f.last("frames")["frames"].items[0].text == upper_hex(response));
	CHECK(f.capture.sink.contains("not decoded: TPDU longer than 32 octets"));
	CHECK_FALSE(f.capture.sink.contains("W hc.knx: dropping a tunnelled frame"));

	// One octet more than the longest is refused.
	Bytes too_long = extended(0x11, 254, 0x0000, 0x1105);
	too_long.push_back(0);
	f.send(32, upper_hex(too_long));
	CHECK(f.last("refused")["code"].text == "bus.bad_frame");
	CHECK(f.last("refused")["error"].text == "the frame has 265 octets, more than the 264 forwarded");
	CHECK(f.requests().size() == 2);
}

TEST_CASE("Bus session: every refusal comes with its code and text") {
	SUBCASE("no KNX tunnel") {
		BusFixture f(false);
		f.bus("\"op\":\"open\",\"ref\":1");
		CHECK(f.last("refused")["code"].text == "bus.knx_unavailable");
		CHECK(f.last("refused")["error"].text == "this controller has no KNX tunnel");
		CHECK(f.capture.sink.contains("W hc.ehome: bus open 1 refused: this controller has no KNX tunnel (bus.knx_unavailable)"));
	}
	SUBCASE("the tunnel is down") {
		BusFixture f;
		f.tunnel.stop();
		f.bus("\"op\":\"open\",\"ref\":1");
		CHECK(f.last("refused")["code"].text == "bus.tunnel_down");
		CHECK(f.last("refused")["error"].text == "the KNX tunnel is not up (stopped)");
		CHECK_FALSE(f.link.bus().holds_memory());
	}
	SUBCASE("no session") {
		BusFixture f;
		f.send(1, compact(kConnect));
		CHECK(f.last("refused")["code"].text == "bus.no_session");
		CHECK(f.last("refused")["op"].text == "send");
		f.bus("\"op\":\"close\",\"ref\":2");
		CHECK(f.last("refused")["code"].text == "bus.no_session");
		CHECK(f.last("refused")["op"].text == "close");
		CHECK(f.requests().empty());
	}
	SUBCASE("bad frames") {
		BusFixture f;
		f.open();
		const char* frames[][2] = {
			{"11 00 B0 60 00 00 11 05 00 8", "\"frame\" is an even number of hex digits"},
			{"11 00 B0 60 00 00 11 05 00 8G", "\"frame\" holds a character that is no hex digit"},
			{"", "\"frame\" is an even number of hex digits"},
			{"2E 00 B0 60 00 00 11 05 00 80", "the frame is no L_Data.req (message code 11)"},
			{"11 00 B0 60 00 00 11 05 01 80", "cEMI length field disagrees with the frame length"},
			{"10 00 B0 60 00 00 11 05 00 80", "cEMI message code is not L_Data.req/con/ind"},
			{"11 00 BC E0 00 00 08 01 01 00 81",
				"the frame is a group telegram: the session sends to individual addresses and broadcasts (group "
				"telegrams: the monitor's send)"},
		};
		int ref = 100;
		for (const auto& item : frames) {
			f.send(ref, compact(item[0]));
			Json refused = f.last("refused");
			CHECK(refused["ref"].number == ref);
			CHECK(refused["code"].text == "bus.bad_frame");
			CHECK(refused["error"].text == item[1]);
			++ref;
		}
		f.bus("\"op\":\"send\",\"ref\":7,\"frame\":17");
		CHECK(f.last("refused")["error"].text == "\"frame\" is a cEMI L_Data.req in hex digits");
		CHECK(f.requests().empty());
		// A broadcast is a frame the session sends (A_IndividualAddress_Read).
		f.send(8, "1100B0E00000000001 00");
		f.send(9, compact("11 00 B0 E0 00 00 00 00 01 01 00"));
		CHECK(f.requests().size() == 1);
		CHECK(f.link.bus().is_open());
	}
	SUBCASE("the queue is full") {
		BusFixture f;
		f.open();
		for (int i = 0; i < 4; ++i) {
			f.send(i, compact(kConnect));
		}
		f.send(4, compact(kConnect));
		CHECK(f.last("refused")["ref"].number == 4);
		CHECK(f.last("refused")["code"].text == "bus.queue_full");
		CHECK(f.last("refused")["error"].text == "4 frames of the session wait for their outcome: wait for one");
		f.ack(0);
		f.gateway(bytes(kConnectCon));
		f.send(5, compact(kConnect)); // room again
		CHECK(f.last("refused")["ref"].number == 4);
	}
	SUBCASE("the tunnel's queue is full of the controller's own telegrams") {
		BusFixture f;
		f.open();
		hc::knx::LData own;
		uint8_t on = 1;
		REQUIRE(hc::knx::make_group_request(own, hc::knx::GroupAddress::of(1, 0, 1), hc::knx::GroupService::write, &on, 1, true));
		while (f.tunnel.queued() < hc::knx::KnxTunnel::kQueueCapacity) {
			REQUIRE(f.tunnel.send(own) != 0);
		}
		f.send(6, compact(kConnect));
		CHECK(f.last("refused")["code"].text == "bus.queue_full");
		CHECK(f.last("refused")["error"].text == "the KNX tunnel's queue is full: try again");
	}
	SUBCASE("malformed messages") {
		BusFixture f;
		f.bus("\"ref\":1");
		CHECK(f.last("refused")["code"].text == "bus.bad_request");
		CHECK(f.last("refused")["error"].text == "a bus message carries \"op\": open, send or close");
		f.bus("\"op\":\"reset\",\"ref\":1");
		CHECK(f.last("refused")["error"].text == "\"op\" is open, send or close");
		f.bus("\"op\":\"open\",\"ref\":[1]");
		CHECK(f.last("refused")["error"].text == "\"ref\" is a number or a string of at most 37 bytes");
		f.open();
		f.bus("\"op\":\"send\",\"frame\":\"" + compact(kConnect) + "\"");
		CHECK(f.last("refused")["code"].text == "bus.bad_request");
		CHECK(f.last("refused")["error"].text == "a send needs a \"ref\": its outcome is reported under it");
		CHECK(f.requests().empty());
	}
}

TEST_CASE("Bus session: the tunnel going down reports the session's frames lost, then ends the session") {
	BusFixture f;
	f.open();
	f.send(40, compact(kConnect));
	f.send(41, compact(kConnect));
	f.socket.inject(hc_test::disconnect_request(7));
	f.run();
	CHECK_FALSE(f.link.bus().is_open());
	std::vector<std::string> order = f.order();
	REQUIRE(order.size() == 4);
	CHECK(order[0] == "opened");
	CHECK(order[1] == "sent");
	CHECK(order[2] == "sent");
	CHECK(order[3] == "closed");
	CHECK(f.events("sent")[0]["outcome"].text == "lost");
	CHECK(f.events("sent")[1]["ref"].number == 41);
	Json closed = f.last("closed");
	CHECK(closed["code"].text == "bus.tunnel_down");
	CHECK(closed["reason"].text == "the KNX tunnel went down: the gateway closed the tunnel (DISCONNECT_REQUEST)");
	CHECK_FALSE(f.link.bus().holds_memory());
}

TEST_CASE("Bus session: messages wait for room in the send queue and leave in order; an outbox full drops indications, counted") {
	BusFixture f;
	f.open();
	f.client.fail_sends = true;
	f.send(50, compact(kConnect));
	f.ack(0);
	f.gateway(bytes(kConnectCon));
	f.gateway(bytes(kAckFromDevice));
	f.gateway(bytes(kAddressResponse));
	size_t messages = f.client.sent("bus").size();
	f.client.fail_sends = false;
	f.client.drain(SIZE_MAX); // the link hears ws_client_writable()
	std::vector<std::string> order = f.order();
	REQUIRE(order.size() == messages + 2);
	CHECK(order[messages] == "sent");
	CHECK(order[messages + 1] == "frames");
	CHECK(f.last("frames")["frames"].items.size() == 2);

	// The outbox keeps its reserve for answers: indications beyond it are dropped and counted.
	f.client.fail_sends = true;
	Bytes response = extended(0x29, 200, 0x1105, 0x11FA);
	for (int i = 0; i < 8; ++i) {
		f.gateway(response);
	}
	f.client.fail_sends = false;
	f.wait(1000); // the tick sends what waits
	Json frames = f.last("frames");
	// 7 records of 213 octets fit the 2048 - 512 octets indications may take; the 8th is dropped.
	CHECK(frames["frames"].items.size() == 7);
	CHECK(frames["dropped"].number == 1);
	f.bus("\"op\":\"close\"");
	CHECK(f.last("closed")["dropped"].number == 1);
	CHECK(f.last("closed")["received"].number == 9);
}

TEST_CASE("Bus session: frames of an ended session are still sent; their memory is given back after the outcome") {
	BusFixture f;
	f.open();
	f.send(60, compact(kConnect));
	f.client.drop("connection reset");
	CHECK_FALSE(f.link.bus().is_open());
	CHECK(f.link.bus().holds_memory()); // the tunnel still holds the frame
	f.ack(0);
	f.gateway(bytes(kConnectCon));
	CHECK_FALSE(f.link.bus().holds_memory());
	f.advance(1100);
	f.handshake();
	f.open(); // a new session starts clean
	CHECK(f.events("sent").empty());
}

TEST_CASE("Bus session: answers that outrun the connection end the session with bus.overflow, visibly") {
	BusFixture f;
	f.open();
	f.client.fail_sends = true;
	for (int i = 0; i < 20; ++i) {
		f.send(i, "XX"); // each refused: the answers pile up in the outbox
	}
	CHECK_FALSE(f.link.bus().is_open());
	CHECK(f.capture.sink.contains("W hc.ehome: the bus session's messages to E-Home outran the connection"));
	f.client.fail_sends = false;
	f.client.drain(SIZE_MAX);
	f.wait(1000);
	Json closed = f.last("closed");
	CHECK(closed["code"].text == "bus.overflow");
	CHECK(closed["reason"].text == "the messages to E-Home outran the connection");
	CHECK(f.last("refused")["code"].text == "bus.no_session"); // the sends after the end
	CHECK_FALSE(f.link.bus().holds_memory());
}

TEST_CASE("Bus session probe: a long group telegram during a session stays out of the session") {
	BusFixture f;
	f.open();
	// An extended group write of 40 octets of APDU to 1/0/1: a group telegram, however long.
	Bytes group = extended(0x29, 40, 0x1105, 0x0801);
	group[3] = 0xE0; // group destination
	f.gateway(group);
	CHECK(f.events("frames").empty());
	CHECK(f.status()["bus"]["received"].number == 0);
	// A frame for the tunnel right after it still comes through.
	f.gateway(bytes(kAckFromDevice));
	REQUIRE(f.events("frames").size() == 1);
	CHECK(f.last("frames")["frames"].items[0].text == compact(kAckFromDevice));
}
