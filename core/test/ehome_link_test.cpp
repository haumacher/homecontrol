/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The E-Home connection (issue #41, spec/api.md "E-Home connection", doc/ehome.md): the enrollment
// through a fake HTTP fetcher, the socket against a fake E-Home server behind a fake WebSocket
// client, the E-Home principal, the status; the binary runtime image over the socket (issue #87:
// `length` and binary frames written into the image slot as they arrive, its signature inside).

#include <functional>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_worker.h"
#include "hc/image/binary.h"
#include "hc/image/image.h"
#include "fake_blob_store.h"
#include "fake_fetch.h"
#include "fake_name_service.h"
#include "hc/ehome_link.h"
#include "hc/image/image_service.h"
#include "hc/image/memory_image_slot.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "hc/live_feed.h"
#include "hc/network_name.h"
#include "hc/sha.h"
#include "hc/connectivity.h"
#include "hc/diagnostics.h"
#include "hc/improv_protocol.h"
#include "hc/status_led.h"
#include "hc/status_text.h"
#include "hc/signature.h"
#include "hc/websocket.h"
#include "image_support.h"
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

/// The source `json` compiled into the binary image, signed with the seed `seed_hex` (nullptr:
/// unsigned), as the bytes E-Home sends.
std::string compiled_text(const std::string& json, const char* seed_hex = nullptr) {
	hc::test::CompileOptions options;
	uint8_t seed[32];
	if (seed_hex != nullptr) {
		REQUIRE(hc::from_hex(seed_hex, seed, sizeof(seed)));
		options.sign_seed = seed;
	}
	std::vector<uint8_t> bytes = hc_test::compiled(json, options);
	return std::string(bytes.begin(), bytes.end());
}

/// The fixture `name` (spec/fixtures/images) compiled, signed with `seed_hex` (nullptr: unsigned).
std::string compiled_fixture(const std::string& name, const char* seed_hex = nullptr) {
	return compiled_text(image_fixture(name), seed_hex);
}

struct EhomeFixture : WebFixture {
	hc_test::ScopedCapture capture{hc::LogLevel::debug};
	hc_test::FakeTransport transport;
	hc::LiveFeed feed{api, controller.loop(), transport};
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::MemoryImageSlot slot;
	hc::image::ImageService service{controller, model, blobs, slot, adapter, &tunnel};
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
		// The newest image format it reads and its slot (issue #87), what runs.
		CHECK(auth.back()["format"].text == hc::image::bin::format_text());
		CHECK(auth.back()["slot"].number == hc::image::MemoryImageSlot::kDefaultSize);
		CHECK_FALSE(auth.back().has("schema"));
		CHECK(auth.back()["image"]["loaded"].boolean == service.loaded());
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

	// PUT /api/image with `length` and binary frames, written into the slot as they arrive.
	std::string image = compiled_fixture("example.json");
	REQUIRE(image.size() > hc::WebSocketClient::kMaxText);
	f.client.deliver("{\"type\":\"request\",\"id\":8,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(image.size()) + "}");
	CHECK_FALSE(f.response(8).has("status")); // not before the last byte
	f.client.deliver("{\"type\":\"ping\"}"); // the feed keeps working between the frames
	CHECK(f.client.sent("pong").size() == 1);
	f.client.deliver_binary(image, 1024);
	Json loaded = f.response(8);
	CHECK(loaded["status"].number == 200);
	CHECK(loaded["body"]["image"]["revision"].number == 7);
	CHECK(f.service.loaded());
	CHECK(f.capture.sink.contains("PUT /api/image from E-Home"));
	CHECK(f.capture.sink.contains("receiving " + std::to_string(image.size()) + " bytes into the image slot"));
	CHECK(std::memcmp(f.slot.bytes(), image.data(), image.size()) == 0);

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
	std::string image = compiled_fixture("example.json");
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

TEST_CASE("E-Home: an image larger than the slot is refused 413 image.too_large naming the slot, its frames dropped") {
	EhomeFixture f;
	f.handshake();
	f.client.deliver("{\"type\":\"request\",\"id\":1,\"method\":\"PUT\",\"path\":\"/api/image\","
		"\"length\":600000}");
	CHECK(f.response(1)["status"].number == 413);
	CHECK(f.response(1)["body"]["code"].text == "image.too_large");
	CHECK(f.response(1)["body"]["error"].text == "the image has 600000 bytes; the device holds at most 524288 (its image slot)");
	CHECK(f.capture.sink.contains("the image has 600000 bytes; the device holds at most 524288 (its image slot)"));
	CHECK(f.slot.erased_sectors() == 0); // nothing was written
	f.client.deliver_binary(std::string(600000, 'x'), 4096);
	CHECK(f.link.state() == EhomeState::connected);
	f.client.deliver("{\"type\":\"request\",\"id\":2,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK(f.response(2)["status"].number == 200);
	CHECK(f.response(2)["body"]["image"]["slot"].number == 524288);
	CHECK_FALSE(f.response(2)["body"]["image"].has("storable"));
}

TEST_CASE("E-Home: a refusal from the image's header is answered at once, the rest of the body dropped, the running image stays") {
	EhomeFixture f;
	f.handshake();
	std::string minimal = compiled_fixture("minimal.json");
	f.client.deliver("{\"type\":\"request\",\"id\":1,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(minimal.size()) + "}");
	f.client.deliver_binary(minimal, 1024);
	REQUIRE(f.response(1)["status"].number == 200);
	// A JSON text (the source form) instead of the binary image: refused when its first 32 bytes are
	// in, before the slot is touched; the frames after it are dropped.
	std::string json = image_fixture("example.json");
	REQUIRE(json.size() > 3000);
	f.client.deliver("{\"type\":\"request\",\"id\":2,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(json.size()) + "}");
	f.client.deliver_binary(json.substr(0, 1000), 1000);
	Json r = f.response(2);
	CHECK(r["status"].number == 400); // answered before the last byte
	CHECK(r["body"]["code"].text == "image.format");
	CHECK(r["body"]["refusals"].items[0]["path"].text == "header.magic");
	CHECK(r["body"]["refusals"].items[0]["reason"].text.find("a JSON text (E-Home compiles the image; send the binary form)") !=
		std::string::npos);
	CHECK(r["body"]["loaded"].boolean);
	f.client.deliver_binary(json.substr(1000), 1024); // dropped
	CHECK(f.link.state() == EhomeState::connected);
	CHECK(f.service.loaded());
	CHECK(std::string(f.service.info().project_id) == "minimal");
	CHECK(std::memcmp(f.slot.bytes(), minimal.data(), minimal.size()) == 0);
	// A header in two frames (the first shorter than the header): the refusal waits for all 32 bytes.
	std::vector<uint8_t> major(minimal.begin(), minimal.end());
	major[4] = 2;
	std::string newer(major.begin(), major.end());
	f.client.deliver("{\"type\":\"request\",\"id\":3,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(newer.size()) + "}");
	f.client.deliver_binary(newer.substr(0, 10), 10);
	CHECK_FALSE(f.response(3).has("status"));
	f.client.deliver_binary(newer.substr(10), 1024);
	CHECK(f.response(3)["status"].number == 422);
	CHECK(f.response(3)["body"]["code"].text == "image.version");
	CHECK(f.service.loaded());
	f.client.deliver("{\"type\":\"request\",\"id\":4,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK(f.response(4)["status"].number == 200);
}

TEST_CASE("E-Home: PUT /api/image with an inline body or the former signature member is refused 400 image.bad_request") {
	EhomeFixture f;
	f.handshake();
	const char* text = "the image is binary: it is sent with length and binary frames, its signature inside";
	// Inline: the body would be parsed JSON, not the image's bytes.
	f.client.deliver("{\"type\":\"request\",\"id\":1,\"method\":\"PUT\",\"path\":\"/api/image\",\"body\":" +
		image_fixture("minimal.json") + "}");
	CHECK(f.response(1)["status"].number == 400);
	CHECK(f.response(1)["body"]["code"].text == "image.bad_request");
	CHECK(f.response(1)["body"]["error"].text == text);
	CHECK_FALSE(f.service.loaded());
	// `signature` beside `length`: refused, the announced frames dropped, the link stays.
	std::string minimal = compiled_fixture("minimal.json");
	f.client.deliver("{\"type\":\"request\",\"id\":2,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(minimal.size()) + ",\"signature\":\"" + std::string(128, 'a') + "\"}");
	CHECK(f.response(2)["status"].number == 400);
	CHECK(f.response(2)["body"]["code"].text == "image.bad_request");
	CHECK(f.response(2)["body"]["error"].text == text);
	f.client.deliver_binary(minimal, 1024);
	CHECK(f.link.state() == EhomeState::connected);
	CHECK_FALSE(f.service.loaded());
	CHECK(f.slot.erased_sectors() == 0);
	// The same image with `length` alone goes in.
	f.client.deliver("{\"type\":\"request\",\"id\":3,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(minimal.size()) + "}");
	f.client.deliver_binary(minimal, 1024);
	CHECK(f.response(3)["status"].number == 200);
	CHECK(f.service.loaded());
}

TEST_CASE("E-Home: a body other than the image above 4 KB arrives whole (a timeline PUT)") {
	EhomeFixture f;
	REQUIRE_MESSAGE(hc_test::deploy(f.service, image_fixture("heating-timeline.json")), hc_test::describe(f.service.report()));
	f.handshake();
	std::string body = R"({"timeline": {"plans": [{"id": "every", "label": "Every day", "points": [)"
		R"({"time": {"hour": 6, "minute": 0, "second": 0}, "state": "comfort"},)" + std::string(5000, ' ') +
		R"({"time": {"hour": 21, "minute": 30, "second": 0}, "state": "night"}]}], "assignments": [{"plan": "every"}]}})";
	REQUIRE(body.size() > hc::WebSocketClient::kMaxText);
	f.client.deliver("{\"type\":\"request\",\"id\":1,\"method\":\"PUT\",\"path\":\"/api/rules/heating/timeline\","
		"\"length\":" + std::to_string(body.size()) + "}");
	CHECK(f.capture.sink.contains("receiving " + std::to_string(body.size()) + " bytes"));
	CHECK_FALSE(f.capture.sink.contains("bytes into the image slot"));
	f.client.deliver_binary(body, 1024);
	Json r = f.response(1);
	REQUIRE_MESSAGE(r["status"].number == 200, r["body"]["error"].text);
	auto timeline = f.get("/api/rules/heating/timeline", f.cookie);
	REQUIRE(timeline.status == 200);
	CHECK(timeline.json["overridden"].boolean);
	REQUIRE(timeline.json["plans"].items.size() == 1);
	REQUIRE(timeline.json["plans"].items[0]["points"].items.size() == 2);
	CHECK(timeline.json["plans"].items[0]["points"].items[1]["time"]["hour"].number == 21);
	CHECK(timeline.json["plans"].items[0]["points"].items[1]["state"].text == "night");
	CHECK(f.service.loaded()); // the image was not touched
}

TEST_CASE("E-Home: a body whose frames stop is reported once with the bytes received") {
	EhomeFixture f;
	f.handshake();
	std::string image = compiled_fixture("example.json");
	std::string length = std::to_string(image.size());
	f.client.deliver("{\"type\":\"request\",\"id\":1,\"method\":\"PUT\",\"path\":\"/api/image\","
		"\"length\":" + length + "}");
	REQUIRE(f.capture.sink.contains("receiving " + length + " bytes"));
	f.client.deliver_binary(image.substr(0, 1000), 1000);
	f.advance(EhomeLink::kBodyStallMs - 1000);
	CHECK_FALSE(f.capture.sink.contains("the body stalls"));
	f.advance(2000);
	CHECK(f.capture.sink.contains("request 1 /api/image: the body stalls at 1000 of " + length + " bytes"));
	f.advance(2000);
	CHECK(f.capture.sink.count("the body stalls") == 1);
}

TEST_CASE("E-Home: a connection lost mid-body leaves no image; the next session deploys again") {
	EhomeFixture f;
	f.handshake();
	std::string minimal = compiled_fixture("minimal.json");
	f.client.deliver("{\"type\":\"request\",\"id\":1,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(minimal.size()) + "}");
	f.client.deliver_binary(minimal, 1024);
	REQUIRE(f.response(1)["status"].number == 200);
	// The next image breaks off after 4096 bytes: the slot was erased for it, so no image afterwards
	// (spec/runtime-image.md I9), reported as image.format with the bytes received.
	std::string image = compiled_fixture("example.json");
	REQUIRE(image.size() > 4096);
	f.client.deliver("{\"type\":\"request\",\"id\":2,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(image.size()) + "}");
	f.client.deliver_binary(image.substr(0, 4096), 1024);
	CHECK(f.service.receiving());
	f.client.drop("connection reset");
	CHECK_FALSE(f.service.receiving());
	CHECK_FALSE(f.service.loaded());
	CHECK(f.service.report().kind == hc::image::Report::Kind::format);
	REQUIRE(f.service.report().count == 1);
	CHECK(std::string(f.service.report().refusals[0].reason) ==
		"the header says " + std::to_string(image.size()) + " bytes, the body has 4096");
	CHECK(f.service.swap_kept()); // the values wait for the deployment E-Home sends again
	auto status = f.get("/api/status", f.cookie);
	CHECK_FALSE(status.json["image"]["loaded"].boolean);
	CHECK(status.json["image"]["refused"].text == "image.format");
	CHECK(status.json["image"]["reason"].text == "header.length: the header says " + std::to_string(image.size()) +
		" bytes, the body has 4096");
	// The next session: `auth` tells E-Home that nothing runs and why; it deploys again.
	f.advance(1100);
	REQUIRE(f.client.session);
	f.client.open();
	f.client.deliver(std::string("{\"type\":\"challenge\",\"nonce\":\"") + kNonce + "\"}");
	Json auth = f.client.sent("auth").back();
	CHECK_FALSE(auth["image"]["loaded"].boolean);
	CHECK(auth["image"]["refused"].text == "image.format");
	CHECK(auth["image"]["reason"].text.find("header.length: the header says") == 0);
	f.client.deliver("{\"type\":\"ready\",\"installation\":\"Example House\"}");
	REQUIRE(f.link.state() == EhomeState::connected);
	f.client.deliver("{\"type\":\"request\",\"id\":3,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(image.size()) + "}");
	f.client.deliver_binary(image, 1024);
	CHECK(f.response(3)["status"].number == 200);
	CHECK(f.service.loaded());
	CHECK_FALSE(f.service.swap_kept());
}

TEST_CASE("E-Home: the auth message names what runs: project, revision and CRC") {
	EhomeFixture f;
	std::vector<uint8_t> bytes = hc_test::compiled(image_fixture("example.json"));
	REQUIRE(hc_test::deploy_bytes(f.service, bytes));
	f.handshake();
	Json auth = f.client.sent("auth").back();
	CHECK(auth["format"].text == "1.1");
	CHECK(auth["slot"].number == 524288);
	CHECK(auth["image"]["loaded"].boolean);
	CHECK(auth["image"]["project"].text == "example-house");
	CHECK(auth["image"]["revision"].number == 7);
	char crc[12];
	std::snprintf(crc, sizeof(crc), "%08x", static_cast<unsigned>(hc::image::bin::image_crc(bytes.data(), bytes.size())));
	CHECK(auth["image"]["crc"].text == crc);
	CHECK(auth["image"]["crc"].text == f.get("/api/status", f.cookie).json["image"]["crc"].text);
	CHECK_FALSE(auth["image"].has("refused"));
}

TEST_CASE("E-Home: a command over the feed is recorded in the monitor as via ehome") {
	EhomeFixture f;
	REQUIRE(hc_test::deploy(f.service, image_fixture("example.json")));
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


std::string enroll_answer_with_key(const std::string& key) {
	std::string answer = kEnrollAnswer;
	answer.insert(answer.size() - 1, ", \"public_key\": \"" + key + "\"");
	return answer;
}

/// PUT /api/image over the socket: `length`, then the binary frames (the signature is in the image).
Json socket_put_image(EhomeFixture& f, int id, const std::string& image) {
	f.client.deliver("{\"type\":\"request\",\"id\":" + std::to_string(id) +
		",\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" + std::to_string(image.size()) + "}");
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
		CHECK(socket_put_image(f, 3, compiled_fixture("minimal.json"))["status"].number == 200);
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
	std::string example = compiled_fixture("example.json");
	Json r = socket_put_image(f, 4, example);
	CHECK(r["status"].number == 403);
	CHECK(r["body"]["code"].text == "image.unsigned");
	CHECK(r["body"]["refusals"].items[0]["reason"].text.find(kKeyFingerprint) != std::string::npos);
	CHECK_FALSE(f.service.loaded());
	CHECK(f.slot.erased_sectors() == 0); // refused from the header: nothing written
	r = socket_put_image(f, 5, compiled_fixture("example.json", kNewSeed));
	CHECK(r["status"].number == 403);
	CHECK(r["body"]["code"].text == "image.bad_signature");
	CHECK(r["body"]["refusals"].items[0]["reason"].text.find(kKeyFingerprint) != std::string::npos);
	CHECK_FALSE(f.service.loaded());
	// The former detached signature beside the frames: refused, whatever it holds.
	f.client.deliver("{\"type\":\"request\",\"id\":6,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(example.size()) + ",\"signature\":\"abc\"}");
	f.client.deliver_binary(example, 1024);
	r = f.response(6);
	CHECK(r["status"].number == 400);
	CHECK(r["body"]["code"].text == "image.bad_request");
	std::string good = compiled_fixture("example.json", kKeySeed);
	r = socket_put_image(f, 7, good);
	CHECK(r["status"].number == 200);
	CHECK(r["body"]["image"]["signed"].boolean);
	CHECK(f.service.info().revision == 7);
	// The unsigned one again: refused from its header, the signed image keeps running.
	r = socket_put_image(f, 8, example);
	CHECK(r["body"]["code"].text == "image.unsigned");
	CHECK(r["body"]["loaded"].boolean);
	CHECK(f.service.loaded());
	CHECK(f.link.state() == EhomeState::connected);
}

TEST_CASE("E-Home key: a rotation stays pending until the owner accepts it; then the old key is refused") {
	EhomeFixture f;
	REQUIRE(f.fake.settings.set_string("ehome.key", kKey) == hc::SettingsStatus::ok);
	f.handshake();
	std::string minimal = image_fixture("minimal.json");
	std::string example = image_fixture("example.json");
	auto signed_by = [](const std::string& json, const char* seed) { return compiled_text(json, seed); };
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
	CHECK(socket_put_image(f, 10, signed_by(minimal, kNewSeed))["body"]["code"].text == "image.bad_signature");
	CHECK(socket_put_image(f, 11, signed_by(minimal, kKeySeed))["status"].number == 200);
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
	CHECK(socket_put_image(f, 13, signed_by(example, kKeySeed))["body"]["code"].text == "image.bad_signature");
	CHECK(socket_put_image(f, 14, signed_by(example, kNewSeed))["status"].number == 200);
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
	CHECK(socket_put_image(f, 20, compiled_fixture("minimal.json"))["status"].number == 200);
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
	CHECK(socket_put_image(f, 21, compiled_fixture("minimal.json"))["body"]["code"].text == "image.unsigned");
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
	std::string minimal = compiled_fixture("minimal.json");
	auto r = f.put("/api/image", minimal);
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "image.unsigned");
	CHECK(r.json["refusals"].items[0]["reason"].text.find(kKeyFingerprint) != std::string::npos);
	r = f.put("/api/image", compiled_fixture("minimal.json", kNewSeed)); // the never-accepted rotation's key
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
	CHECK(f.put("/api/image", compiled_fixture("minimal.json", kKeySeed)).json["code"].text == "image.bad_signature");
	CHECK(f.put("/api/image", compiled_fixture("minimal.json", kNewSeed)).status == 200);
	// A new enrollment without a key leaves the device without one (the authenticated answer decides).
	REQUIRE(f.del("/api/ehome").status == 200);
	REQUIRE(f.enroll("{\"url\":\"https://ehome.example\",\"code\":\"ABCD-EFGH\"}",
		[&] { f.fetcher.answer(200, kEnrollAnswer); }).status == 200);
	CHECK(stored_key(f).empty());
	CHECK(f.put("/api/image", minimal).status == 200);
}

// ---- Probe: the socket's inline form of an image, and a key offer that changes nothing ------------

TEST_CASE("E-Home key probe: an image sent as the request's inline body is refused whatever its signature") {
	EhomeFixture f;
	REQUIRE(f.fake.settings.set_string("ehome.key", kKey) == hc::SettingsStatus::ok);
	f.handshake();
	std::string minimal = image_fixture("minimal.json");
	REQUIRE(minimal.size() < 4096);
	// Below the text limit E-Home could put a body inline (no length, no binary frames): the image is
	// binary and carries its signature, so neither form is taken, with or without `signature`.
	auto text_put = [&](int id, const std::string& signature) {
		std::string request = "{\"type\":\"request\",\"id\":" + std::to_string(id) +
			",\"method\":\"PUT\",\"path\":\"/api/image\",\"body\":" + minimal;
		if (!signature.empty()) {
			request += ",\"signature\":\"" + signature + "\"";
		}
		f.client.deliver(request + "}");
		return f.response(id);
	};
	for (const std::string& signature : {std::string(), std::string(128, 'a'), std::string("abc")}) {
		CAPTURE(signature);
		Json r = text_put(20, signature);
		CHECK(r["status"].number == 400);
		CHECK(r["body"]["code"].text == "image.bad_request");
		CHECK(r["body"]["error"].text ==
			"the image is binary: it is sent with length and binary frames, its signature inside");
		CHECK_FALSE(f.service.loaded());
	}
	CHECK(socket_put_image(f, 22, compiled_text(minimal, kKeySeed))["status"].number == 200);
	CHECK(f.service.loaded());

	// E-Home offers the key the device already holds: nothing becomes pending, the answer says so,
	// and images keep working with it.
	f.client.deliver(std::string("{\"type\":\"key\",\"public_key\":\"") + kKey + "\",\"fingerprint\":\"" +
		kKeyFingerprint + "\"}");
	CHECK_FALSE(f.status().has("pending_key"));
	CHECK(f.status()["key"].text == kKeyFingerprint);
	CHECK(f.client.sent("key_pending").empty());
	CHECK(f.client.sent("key_accepted").size() + f.client.sent("error").size() >= 1);
	CHECK(socket_put_image(f, 23, compiled_fixture("example.json", kKeySeed))["status"].number == 200);
	CHECK(f.link.state() == EhomeState::connected);
}

// ---- The bounded send queue (issue #55) --------------------------------------------------------

namespace {

/// Loads the example image over the socket (with an unbounded queue): 15 datapoints, a snapshot of
/// several chunks.
void load_example(EhomeFixture& f) {
	std::string image = compiled_fixture("example.json");
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
	std::string image = compiled_fixture("example.json");
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

TEST_CASE("E-Home image slot: a signed image is written into the slot as its frames arrive, verified and run in place") {
	// Issue #87: a deploy needs no buffer of the image's size; the slot is erased sector by sector.
	EhomeFixture f;
	REQUIRE(f.fake.settings.set_string("ehome.key", kKey) == hc::SettingsStatus::ok);
	f.handshake();
	std::string example = compiled_fixture("example.json", kKeySeed);
	f.client.deliver("{\"type\":\"request\",\"id\":4,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(example.size()) + "}");
	f.client.deliver_binary(example.substr(0, 2048), 1024);
	// The frames are in the slot as they came (the old image is gone at the first byte, I3).
	CHECK(f.service.receiving());
	CHECK(std::memcmp(f.slot.bytes(), example.data(), 2048) == 0);
	CHECK_FALSE(f.response(4).has("status"));
	f.client.deliver_binary(example.substr(2048), 1024);
	Json r = f.response(4);
	CHECK(r["status"].number == 200);
	CHECK(r["body"]["image"]["signed"].boolean);
	CHECK(r["body"]["image"]["bytes"].number == static_cast<double>(example.size()));
	CHECK(f.service.loaded());
	CHECK_FALSE(f.service.receiving());
	CHECK(f.slot.erased_sectors() == (example.size() + 4095) / 4096);
	CHECK(std::memcmp(f.slot.bytes(), example.data(), example.size()) == 0);
	CHECK(f.service.image().bytes() == f.slot.bytes());
	for (const char* path : {"/api/image", "/api/rules", "/api/datapoints", "/api/schedules", "/api/pages", "/api/model"}) {
		auto answer = f.get(path, f.cookie);
		INFO(path);
		CHECK(answer.status == 200);
	}
	// A mis-signed image is refused after the slot was written: no image.
	r = socket_put_image(f, 5, compiled_fixture("example.json", kNewSeed));
	CHECK(r["status"].number == 403);
	CHECK(r["body"]["code"].text == "image.bad_signature");
	CHECK_FALSE(r["body"]["loaded"].boolean);
	CHECK_FALSE(f.service.loaded());
	// Another request's body never goes there.
	size_t sectors = f.slot.erased_sectors();
	f.client.deliver("{\"type\":\"request\",\"id\":6,\"method\":\"PUT\",\"path\":\"/api/ehome\",\"length\":2}");
	f.client.deliver_binary("{}", 1024);
	CHECK(f.response(6).has("status"));
	CHECK(f.slot.erased_sectors() == sectors);
}

TEST_CASE("E-Home image slot: the flash refusing a write mid-body leaves no image; the connection stays") {
	EhomeFixture f;
	f.handshake();
	std::string example = compiled_fixture("example.json");
	f.slot.fail_write_after(2);
	Json r = socket_put_image(f, 4, example);
	CHECK(r["status"].number >= 500);
	CHECK_FALSE(r["body"]["loaded"].boolean);
	CHECK(r["body"]["refusals"].items[0]["reason"].text.find("the flash refused a write (test)") != std::string::npos);
	CHECK_FALSE(f.service.loaded());
	CHECK(f.link.state() == EhomeState::connected);
	r = socket_put_image(f, 5, example);
	CHECK(r["status"].number == 200);
	CHECK(f.service.loaded());
}

TEST_CASE("E-Home image slot: a deploy while an HTTP deploy is being received is refused 503 image.busy, its frames dropped") {
	EhomeFixture f;
	f.handshake();
	// An HTTP receiver that writes into the slot as the body arrives (the ESP's server) has begun.
	std::string example = compiled_fixture("example.json");
	hc::WebRequest req;
	req.method = HttpMethod::put;
	req.path = "/api/image";
	req.query = "";
	req.cookie = f.cookie.c_str();
	req.requested_with = "HomeControl";
	req.source = "192.0.2.7";
	req.body = example.data();
	req.body_length = 32;
	char out[4096];
	hc::WebResponse http(out, sizeof(out));
	REQUIRE(f.api.image_begin(req, example.size(), http));
	REQUIRE(f.service.receive_write(example.data(), 3000));
	CHECK(f.api.body_refusal(HttpMethod::put, "/api/image") != nullptr);

	f.client.deliver("{\"type\":\"request\",\"id\":3,\"method\":\"PUT\",\"path\":\"/api/image\",\"length\":" +
		std::to_string(example.size()) + "}");
	Json r = f.response(3);
	CHECK(r["status"].number == 503);
	CHECK(r["body"]["code"].text == "image.busy");
	f.client.deliver_binary(example, 1024); // dropped, the link stays up
	CHECK(f.link.state() == EhomeState::connected);
	f.client.deliver("{\"type\":\"request\",\"id\":4,\"method\":\"GET\",\"path\":\"/api/status\"}");
	CHECK(f.response(4)["status"].number == 200);

	// The HTTP deploy ends; then E-Home's goes in.
	REQUIRE(f.service.receive_write(example.data() + 3000, example.size() - 3000));
	req.image_streamed = true;
	req.body = nullptr;
	req.body_length = 0;
	hc::WebResponse done(out, sizeof(out));
	f.api.handle(req, done);
	CHECK(done.status == 200);
	CHECK(f.service.loaded());
	CHECK(socket_put_image(f, 5, example)["status"].number == 200);
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
	for (uint8_t octet : cemi) { // not a range insert: GCC 12's -Warray-bounds false positive (Bookworm)
		result.push_back(octet);
	}
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

TEST_CASE("E-Home: a network name change is answered when its lookup is over (homecontrol #89)") {
	EhomeFixture f;
	hc_test::FakeNameService names{f.fake.clock};
	names.taken = {"printer"};
	REQUIRE(f.fake.settings.set_string("net.hostname", "homecontrol") == hc::SettingsStatus::ok);
	hc::NetworkName name{f.controller.loop(), f.fake.clock, f.fake.settings, names};
	REQUIRE(name.start(f.controller.connectivity()));
	f.api.set_network_name(&name);
	f.handshake();

	f.client.deliver("{\"type\":\"request\",\"id\":21,\"method\":\"PUT\",\"path\":\"/api/settings/net.hostname\","
		"\"body\":{\"value\":\"Kitchen\"}}");
	CHECK_FALSE(f.response(21).has("status")); // the lookup runs
	f.advance(500);
	Json done = f.response(21);
	REQUIRE(done.has("status"));
	CHECK(done["status"].number == 200);
	CHECK(f.fake.settings.entries["net.hostname"].text == "kitchen");

	f.client.deliver("{\"type\":\"request\",\"id\":22,\"method\":\"PUT\",\"path\":\"/api/settings/net.hostname\","
		"\"body\":{\"value\":\"printer\"}}");
	f.advance(500);
	Json taken = f.response(22);
	CHECK(taken["status"].number == 422);
	CHECK(taken["body"]["code"].text == "settings.name_taken");
	CHECK(f.fake.settings.entries["net.hostname"].text == "kitchen");
	f.api.set_network_name(nullptr);
}


// ---- The status fits the live feed's message by construction (homecontrol #92) -------------------

namespace {

/// The ESP's update state (platform/esp/components/hc_web/web_server.cpp) with the texts of an
/// ordinary run: an update channel, a check done, the release key.
void esp_update_state(hc::JsonWriter& json) {
	json.member("available", true)
		.member("partition", "ota_0")
		.member("confirmed", true)
		.member("checking", false)
		.member("source", "https://haumacher.github.io/homecontrol/stable")
		.member("last", "up to date: 1.3.1 is the newest release")
		.member("release_key", "3f9a0c1d2b7e4f5a6c8d")
		.member("last_ago_s", static_cast<uint32_t>(5321))
		.member("next_check_s", static_cast<uint32_t>(80879));
}

/// The ESP's boot record (platform/esp/components/hc_platform/esp_diagnostics.cpp) of a board that
/// has run for a while, without a core dump.
class EspBootFacts : public hc::Diagnostics {
public:
	hc::ResetReason reset_reason() const override { return hc::ResetReason::software; }
	void visit_boot(hc::DiagnosticsVisitor& visitor) const override {
		visitor.number("count", 187);
		visitor.number("brownouts", 0);
		visitor.number("previous_uptime_s", 412377);
		visitor.flag("coredump", false);
	}
};

} // namespace

TEST_CASE("Status: the live status reaches E-Home with an image, a bus session open and the ESP's update state (homecontrol #92)") {
	// homecontrol-f8d198, a Compare over the controller: "E hc.ws status message larger than 1400
	// bytes: not sent" every few seconds, through the session and the reconnects after it.
	EspBootFacts boot;
	hc_test::ScopedPlatformDiagnostics diagnostics(boot);
	BusFixture f;
	f.host.update_writer = esp_update_state;
	REQUIRE_MESSAGE(hc_test::deploy(f.service, image_fixture("example.json")), hc_test::describe(f.service.report()));
	f.client.deliver("{\"type\":\"subscribe\"}");
	f.run();
	size_t before = f.client.sent("status").size();
	f.open();
	f.advance(hc::LiveFeed::kTickMs); // the status check sends the changed status
	auto status = f.client.sent("status");
	REQUIRE(status.size() > before);
	const Json& last = status.back();
	CHECK(last["boot"]["count"].number == 187);
	CHECK(last["image"]["loaded"].boolean);
	CHECK(last["update"]["source"].text == "https://haumacher.github.io/homecontrol/stable");
	CHECK(last["ehome"]["bus"]["open"].boolean);
	CHECK(last["ehome"]["bus"]["address"].text == "1.1.250");
	CHECK_FALSE(f.capture.sink.contains("status message larger than"));
}

TEST_CASE("Status: the largest status, every optional member present and every text at its limit, fits kMessageSize") {
	EhomeFixture f;
	// Through the bound counter: every text at its limit, every number at its widest, every
	// optional member present (an image loaded or refused, whichever is larger; Improv active; an
	// E-Home link with its key, a pending key, an error and an open bus session; the platform's
	// update state and boot facts at their budgets).
	hc::JsonWriter bound = hc::JsonWriter::bound();
	f.feed.write_status_message(bound, false);
	MESSAGE("the largest status: " << bound.length() << " bytes of " << hc::LiveFeed::kMessageSize);
	CHECK(bound.unbounded() == 0); // every text of the status has its limit
	// The writer needs one byte more than the text (its NUL).
	CHECK(bound.length() < hc::LiveFeed::kMessageSize);

	hc::JsonWriter anonymous = hc::JsonWriter::bound();
	f.feed.write_status_message(anonymous, true);
	CHECK(anonymous.unbounded() == 0);
	CHECK(anonymous.length() < bound.length());

	// The status as it is now is within the bound.
	char buffer[hc::LiveFeed::kMessageSize];
	hc::JsonWriter json(buffer, sizeof(buffer));
	f.feed.write_status_message(json, false);
	CHECK_FALSE(json.overflowed());
	CHECK(json.length() <= bound.length());
}

TEST_CASE("Status: no name of the tables the status writes as a code is cut by its limit") {
	using hc::status_text::kCode;
	auto fits = [](const char* name) {
		INFO(std::string(name));
		CHECK(std::strlen(name) + 2 <= kCode);
	};
	for (int i = 0; i <= static_cast<int>(hc::LinkState::up); ++i) {
		fits(hc::link_state_name(static_cast<hc::LinkState>(i)));
	}
	for (int i = 0; i <= static_cast<int>(hc::NetworkMode::failsafe_connecting); ++i) {
		fits(hc::network_mode_name(static_cast<hc::NetworkMode>(i)));
	}
	for (int i = static_cast<int>(hc::improv::State::authorization_required);
		 i <= static_cast<int>(hc::improv::State::provisioned); ++i) {
		fits(hc::improv::state_name(static_cast<hc::improv::State>(i)));
	}
	for (int i = 0; i <= static_cast<int>(hc::LedStatus::ready); ++i) {
		fits(hc::led_status_name(static_cast<hc::LedStatus>(i)));
	}
	for (int i = 0; i <= static_cast<int>(hc::image::Source::file); ++i) {
		fits(hc::image::source_name(static_cast<hc::image::Source>(i)));
	}
	for (int i = 0; i <= static_cast<int>(hc::image::Report::Kind::busy); ++i) {
		fits(hc::image::report_kind_code(static_cast<hc::image::Report::Kind>(i)));
	}
	for (int i = 0; i <= static_cast<int>(EhomeState::failed); ++i) {
		fits(hc::ehome_state_name(static_cast<EhomeState>(i)));
	}
	for (int i = 0; i <= static_cast<int>(hc::ResetReason::cpu_lockup); ++i) {
		fits(hc::reset_reason_name(static_cast<hc::ResetReason>(i)));
	}
}

TEST_CASE("Status: a platform's member over its budget is replaced by its error, logged once, and the status still goes out") {
	EhomeFixture f;
	f.host.update_writer = [](hc::JsonWriter& json) {
		json.member("available", true);
		for (int i = 0; i < 40; ++i) {
			json.member(("member_" + std::to_string(i)).c_str(), "a text the core does not know the length of");
		}
	};
	f.handshake();
	f.client.deliver("{\"type\":\"subscribe\"}");
	f.run();
	f.advance(2 * hc::LiveFeed::kHeartbeatMs);
	auto status = f.client.sent("status");
	REQUIRE(status.size() >= 2);
	for (const Json& s : status) {
		CHECK(s["update"]["error"].text == "the platform's update status is larger than 380 bytes");
		CHECK_FALSE(s["update"].has("available"));
		CHECK(s["ehome"]["state"].text == "connected"); // the members after it are there
	}
	CHECK(f.capture.sink.count("E hc.web: the platform's update status is larger than 380 bytes: left out of the status") == 1);
	CHECK_FALSE(f.capture.sink.contains("status message larger than"));
}

TEST_CASE("Status: texts longer than their limits, escaped at their widest, are cut and the status goes out whole") {
	EhomeFixture f;
	// The update source as the ESP writes it (bounded), of control characters (6 bytes each in JSON).
	f.host.update_writer = [](hc::JsonWriter& json) {
		std::string control(300, '\x01');
		json.member("available", true).member("source", control.c_str(), hc::status_text::kUrl);
	};
	f.handshake();
	f.client.deliver("{\"type\":\"subscribe\"}");
	f.run();
	REQUIRE(f.client.sent("status").size() >= 1);
	std::string raw;
	for (const std::string& t : f.client.texts) {
		if (t.find("\"type\":\"status\"") != std::string::npos) {
			raw = t;
		}
	}
	// The source: as many escaped control characters as its limit holds, then the ellipsis.
	size_t at = raw.find("\"source\":\"");
	REQUIRE(at != std::string::npos);
	size_t end = raw.find('"', at + 10);
	std::string form = raw.substr(at + 9, end - at - 9 + 1);
	CHECK(form.size() <= hc::status_text::kUrl);
	CHECK(form.size() > hc::status_text::kUrl - 6);
	CHECK(form.compare(form.size() - 4, 4, "\xE2\x80\xA6\"") == 0);
	CHECK(form.compare(1, 6, "\\u0001") == 0);
	CHECK_FALSE(f.capture.sink.contains("status message larger than"));
}
