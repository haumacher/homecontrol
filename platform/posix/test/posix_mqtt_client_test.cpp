/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "hc/mqtt_codec.h"
#include "hc/posix/mqtt_client.h"

// No broker is installed on the development machine (`which mosquitto` finds nothing), so these tests
// run the client against an in-process broker that speaks the core's codec over a real TCP socket.

namespace {

using hc::mqtt::PacketType;

struct Packet {
	hc::mqtt::Header header{};
	std::vector<uint8_t> body;
};

/// One listening socket on 127.0.0.1 and the connection it accepts; blocking calls with timeouts.
class TestBroker {
public:
	TestBroker() {
		listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
		REQUIRE(listen_fd_ >= 0);
		int one = 1;
		::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
		REQUIRE(::listen(listen_fd_, 1) == 0);
		socklen_t len = sizeof(addr);
		::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
		port = ntohs(addr.sin_port);
	}
	~TestBroker() {
		close_connection();
		::close(listen_fd_);
	}

	std::string url() const { return "mqtt://127.0.0.1:" + std::to_string(port); }

	bool accept(int timeout_ms = 3000) {
		pollfd p{listen_fd_, POLLIN, 0};
		if (::poll(&p, 1, timeout_ms) != 1) {
			return false;
		}
		fd_ = ::accept(listen_fd_, nullptr, nullptr);
		return fd_ >= 0;
	}

	bool read(Packet& out, int timeout_ms = 3000) {
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
		while (true) {
			hc::mqtt::Header h;
			if (hc::mqtt::decode_header(rx_.data(), rx_.size(), h) == hc::mqtt::DecodeStatus::ok &&
				rx_.size() >= h.header_length + h.remaining_length) {
				out.header = h;
				out.body.assign(rx_.begin() + static_cast<long>(h.header_length),
					rx_.begin() + static_cast<long>(h.header_length + h.remaining_length));
				rx_.erase(rx_.begin(), rx_.begin() + static_cast<long>(h.header_length + h.remaining_length));
				return true;
			}
			int left = static_cast<int>(
				std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
			if (left <= 0) {
				return false;
			}
			pollfd p{fd_, POLLIN, 0};
			if (::poll(&p, 1, left) != 1) {
				return false;
			}
			uint8_t chunk[2048];
			ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
			if (n <= 0) {
				return false;
			}
			rx_.insert(rx_.end(), chunk, chunk + n);
		}
	}

	void write(const std::vector<uint8_t>& bytes) { REQUIRE(::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size())); }

	void close_connection() {
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
	}

	uint16_t port = 0;

private:
	int listen_fd_ = -1;
	int fd_ = -1;
	std::vector<uint8_t> rx_;
};

/// Records the client's events (on the loop thread: the test thread).
struct Recorder : hc::MqttClientListener {
	void mqtt_connected() override { ++connected; }
	void mqtt_disconnected(const char* reason) override { reasons.push_back(reason); }
	void mqtt_message(const hc::MqttMessage& m) override {
		messages.push_back(std::string(m.topic) + "=" + std::string(m.payload, m.length) + (m.retained ? " (retained)" : ""));
	}
	int connected = 0;
	std::vector<std::string> reasons;
	std::vector<std::string> messages;
};

/// Runs the loop side (the executor's jobs) until `done` or the timeout.
bool pump(hc::posix::LoopExecutor& executor, const std::function<bool()>& done, int timeout_ms = 5000) {
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (!done()) {
		if (std::chrono::steady_clock::now() > deadline) {
			return false;
		}
		executor.wait(10);
	}
	return true;
}

hc::MqttConnectOptions options(const std::string& url) {
	static std::string keep;
	keep = url;
	hc::MqttConnectOptions o;
	o.broker = keep.c_str();
	o.client_id = "homecontrol-test";
	o.username = "user";
	o.password = "pw";
	o.will_topic = "hc/status";
	o.will_payload = "offline";
	o.will_retain = true;
	o.will_qos = 1;
	o.keepalive_s = 60;
	return o;
}

std::string text(const std::vector<uint8_t>& bytes) { return std::string(bytes.begin(), bytes.end()); }

} // namespace

TEST_CASE("PosixMqttClientSpeaksMqtt311WithABroker") {
	TestBroker broker;
	std::atomic<bool> broker_ok{false};
	std::vector<uint8_t> connect_body, publish_body, qos0_body;
	// The broker holds the PUBACK until the test has seen the publish in flight: the client's
	// reader thread would otherwise race the check (seen on CI).
	std::atomic<bool> release_puback{false};
	std::thread script([&] {
		Packet p;
		REQUIRE(broker.accept());
		REQUIRE(broker.read(p));
		REQUIRE(p.header.type == PacketType::connect);
		connect_body = p.body;
		broker.write({0x20, 0x02, 0x00, 0x00});
		REQUIRE(broker.read(p));
		REQUIRE(p.header.type == PacketType::subscribe);
		CHECK(p.header.flags == 0x02);
		uint16_t sub_id = static_cast<uint16_t>((p.body[0] << 8) | p.body[1]);
		broker.write({0x90, 0x03, static_cast<uint8_t>(sub_id >> 8), static_cast<uint8_t>(sub_id), 0x01});
		// A retained QoS 1 message: the client must acknowledge it.
		broker.write({0x33, 0x0C, 0x00, 0x03, 't', '/', 'a', 0x00, 0x05, 'h', 'e', 'l', 'l', 'o'});
		REQUIRE(broker.read(p));
		REQUIRE(p.header.type == PacketType::puback);
		CHECK(p.body == std::vector<uint8_t>({0x00, 0x05}));
		REQUIRE(broker.read(p));
		REQUIRE(p.header.type == PacketType::publish);
		CHECK(p.header.flags == 0x03); // QoS 1, retained
		publish_body = p.body;
		hc::mqtt::Publish pub;
		REQUIRE(hc::mqtt::decode_publish(p.header, p.body.data(), p.body.size(), pub));
		while (!release_puback.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		broker.write({0x40, 0x02, static_cast<uint8_t>(pub.packet_id >> 8), static_cast<uint8_t>(pub.packet_id)});
		REQUIRE(broker.read(p));
		REQUIRE(p.header.type == PacketType::publish);
		CHECK(p.header.flags == 0x00);
		qos0_body = p.body;
		broker.close_connection();
		broker_ok = true;
	});

	hc::posix::LoopExecutor executor;
	Recorder rec;
	{
		hc::posix::PosixMqttClient client(executor);
		const char* error = nullptr;
		REQUIRE(client.connect(options(broker.url()), rec, &error));
		CHECK_FALSE(client.publish("x", "y", 1, false, 0, &error)); // before the CONNACK
		REQUIRE(pump(executor, [&] { return rec.connected == 1; }));
		REQUIRE(client.subscribe("t/+", 1, &error));
		REQUIRE(pump(executor, [&] { return !rec.messages.empty(); }));
		CHECK(rec.messages[0] == "t/a=hello (retained)");
		REQUIRE(client.publish("x/y", "v", 1, true, 1, &error));
		CHECK(client.pending() == 1); // the broker has not acknowledged yet
		release_puback = true;
		REQUIRE(pump(executor, [&] { return client.pending() == 0; }));
		REQUIRE(client.publish("x/z", "w", 1, false, 0, &error));
		REQUIRE(pump(executor, [&] { return !rec.reasons.empty(); }));
		CHECK(rec.reasons[0] == "connection closed by the broker");
		CHECK_FALSE(client.publish("x", "y", 1, false, 0, &error));
		CHECK(std::string(error) == "not connected");
	}
	script.join();
	CHECK(broker_ok);
	// CONNECT: protocol "MQTT" level 4, flags user|password|will retain|will QoS 1|will|clean, keepalive 60.
	REQUIRE(connect_body.size() > 10);
	CHECK(text(std::vector<uint8_t>(connect_body.begin(), connect_body.begin() + 7)) == std::string("\0\x04MQTT\x04", 7));
	CHECK(connect_body[7] == 0xEE);
	CHECK(connect_body[8] == 0x00);
	CHECK(connect_body[9] == 60);
	std::string rest = text(std::vector<uint8_t>(connect_body.begin() + 10, connect_body.end()));
	CHECK(rest == std::string("\0\x10homecontrol-test\0\x09hc/status\0\x07offline\0\x04user\0\x02pw", 2 + 16 + 2 + 9 + 2 + 7 + 2 + 4 + 2 + 2));
	CHECK(text(publish_body).substr(0, 5) == std::string("\0\x03x/y", 5));
	CHECK(text(publish_body).substr(7) == "v");
	CHECK(text(qos0_body) == std::string("\0\x03x/zw", 6));
}

TEST_CASE("PosixMqttClientReportsARefusedConnection") {
	TestBroker broker;
	std::thread script([&] {
		Packet p;
		REQUIRE(broker.accept());
		REQUIRE(broker.read(p));
		broker.write({0x20, 0x02, 0x00, 0x05});
		broker.read(p, 1000); // until the client closes
	});
	hc::posix::LoopExecutor executor;
	Recorder rec;
	{
		hc::posix::PosixMqttClient client(executor);
		const char* error = nullptr;
		REQUIRE(client.connect(options(broker.url()), rec, &error));
		REQUIRE(pump(executor, [&] { return !rec.reasons.empty(); }));
		CHECK(rec.reasons[0] == "connection refused: not authorized");
		CHECK(rec.connected == 0);
	}
	script.join();
}

TEST_CASE("PosixMqttClientRefusesTlsAndReportsAClosedPort") {
	hc::posix::LoopExecutor executor;
	Recorder rec;
	hc::posix::PosixMqttClient client(executor);
	const char* error = nullptr;
	CHECK_FALSE(client.connect(options("mqtts://127.0.0.1"), rec, &error));
	CHECK(std::string(error).find("needs TLS") != std::string::npos);
	uint16_t port;
	{
		TestBroker closed; // a port that was free a moment ago
		port = closed.port;
	}
	REQUIRE(client.connect(options("mqtt://127.0.0.1:" + std::to_string(port)), rec, &error));
	REQUIRE(pump(executor, [&] { return !rec.reasons.empty(); }));
	CHECK(rec.reasons[0].find("cannot connect to 127.0.0.1:") == 0);
}

TEST_CASE("PosixMqttClientPingsAndDropsASilentBroker") {
	TestBroker broker;
	std::atomic<bool> pinged{false};
	std::thread script([&] {
		Packet p;
		REQUIRE(broker.accept());
		REQUIRE(broker.read(p));
		broker.write({0x20, 0x02, 0x00, 0x00});
		if (broker.read(p, 3000) && p.header.type == PacketType::pingreq) {
			pinged = true;
		}
		broker.read(p, 3000); // silent until the client gives up
	});
	hc::posix::LoopExecutor executor;
	Recorder rec;
	{
		hc::posix::PosixMqttClient client(executor);
		hc::MqttConnectOptions o = options(broker.url());
		o.keepalive_s = 1;
		const char* error = nullptr;
		REQUIRE(client.connect(o, rec, &error));
		REQUIRE(pump(executor, [&] { return !rec.reasons.empty(); }, 6000));
		CHECK(rec.reasons[0] == "keepalive timeout: the broker did not answer");
	}
	script.join();
	CHECK(pinged);
}

TEST_CASE("PosixMqttClientDisconnectsCleanly") {
	TestBroker broker;
	std::atomic<bool> got_disconnect{false};
	std::thread script([&] {
		Packet p;
		REQUIRE(broker.accept());
		REQUIRE(broker.read(p));
		broker.write({0x20, 0x02, 0x00, 0x00});
		if (broker.read(p, 3000) && p.header.type == PacketType::disconnect) {
			got_disconnect = true;
		}
	});
	hc::posix::LoopExecutor executor;
	Recorder rec;
	{
		hc::posix::PosixMqttClient client(executor);
		const char* error = nullptr;
		REQUIRE(client.connect(options(broker.url()), rec, &error));
		REQUIRE(pump(executor, [&] { return rec.connected == 1; }));
		client.disconnect();
		pump(executor, [] { return false; }, 300);
		CHECK(rec.reasons.empty()); // no callback after disconnect()
		CHECK(client.pending() == 0);
	}
	script.join();
	CHECK(got_disconnect);
}
