/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_TEST_SUPPORT_H
#define HC_KNX_TEST_SUPPORT_H

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/knx/knxnetip.h"
#include "hc/knx/telegram_log.h"
#include "hc/knx/tunnel.h"
#include "hc/platform/udp_socket.h"
#include "test_support.h"

namespace hc_test {

/// The gateway of the tests (documentation range, RFC 5737) and the controller's local endpoint.
constexpr uint32_t kGatewayIp = 0xC0000201;     // 192.0.2.1
constexpr uint32_t kLocalIp = 0xC0000264;       // 192.0.2.100
constexpr uint16_t kLocalPort = 50100;           // 0xC3B4
const hc::Ipv4Endpoint kGateway{kGatewayIp, 3671};

using Bytes = std::vector<uint8_t>;

/// Hex dump "06 10 02 05" of octets, for readable failure messages.
inline std::string hex(const Bytes& bytes) {
	std::string result;
	char text[4];
	for (size_t i = 0; i < bytes.size(); ++i) {
		std::snprintf(text, sizeof(text), i == 0 ? "%02X" : " %02X", bytes[i]);
		result += text;
	}
	return result;
}

/// Reads "06 10 02 05" into octets.
inline Bytes bytes(const char* text) {
	Bytes result;
	unsigned value = 0;
	int digits = 0;
	for (const char* p = text;; ++p) {
		char c = *p;
		int nibble = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
		if (nibble >= 0) {
			value = value * 16 + static_cast<unsigned>(nibble);
			++digits;
			continue;
		}
		if (digits > 0) {
			result.push_back(static_cast<uint8_t>(value));
			value = 0;
			digits = 0;
		}
		if (c == '\0') {
			return result;
		}
	}
}

inline std::string hex(const hc::knx::ip::FrameBuffer& frame) {
	return hex(Bytes(frame.data, frame.data + frame.length));
}

/// Records the datagrams sent, lets the test inject the gateway's answers and the outcome of a
/// resolution.
class FakeUdpSocket : public hc::UdpSocket {
public:
	struct Datagram {
		hc::Ipv4Endpoint to;
		Bytes data;
		uint64_t at;
	};

	explicit FakeUdpSocket(const FakeClock& clock) : clock_(clock) {}

	bool open(hc::UdpReceiver& receiver_, const char** error) override {
		if (fail_open) {
			*error = "no sockets left";
			return false;
		}
		receiver = &receiver_;
		opened = true;
		++opens;
		return true;
	}

	void close() override {
		if (opened) {
			++closes;
		}
		opened = false;
	}

	bool is_open() const override { return opened; }

	bool send_to(const hc::Ipv4Endpoint& to, const uint8_t* data, size_t length, const char** error) override {
		if (!opened) {
			*error = "socket closed";
			return false;
		}
		sent.push_back(Datagram{to, Bytes(data, data + length), clock_.now_ms});
		return true;
	}

	bool local_endpoint(const hc::Ipv4Endpoint&, hc::Ipv4Endpoint& local, const char** error) override {
		if (!opened) {
			*error = "socket closed";
			return false;
		}
		local = hc::Ipv4Endpoint{kLocalIp, kLocalPort};
		return true;
	}

	bool resolve(const char* host, hc::UdpReceiver& receiver_, const char**) override {
		resolves.push_back(host);
		resolver = &receiver_;
		return true;
	}

	/// The gateway sends `data`.
	void inject(const Bytes& data, hc::Ipv4Endpoint from = kGateway) {
		REQUIRE(opened);
		receiver->udp_received(data.data(), data.size(), from);
	}

	/// The service type of datagram `index`.
	uint16_t service(size_t index) const {
		const Bytes& d = sent.at(index).data;
		return static_cast<uint16_t>((d[2] << 8) | d[3]);
	}

	/// The service types of all datagrams sent, as names.
	std::vector<std::string> services() const {
		std::vector<std::string> result;
		for (size_t i = 0; i < sent.size(); ++i) {
			result.push_back(hc::knx::ip::service_name(service(i)));
		}
		return result;
	}

	/// Number of datagrams with `service`.
	size_t count(hc::knx::ip::Service service_) const {
		size_t result = 0;
		for (size_t i = 0; i < sent.size(); ++i) {
			result += service(i) == static_cast<uint16_t>(service_) ? 1 : 0;
		}
		return result;
	}

	const Datagram& last() const { return sent.back(); }

	bool fail_open = false;
	bool opened = false;
	int opens = 0;
	int closes = 0;
	hc::UdpReceiver* receiver = nullptr;
	hc::UdpReceiver* resolver = nullptr;
	std::vector<std::string> resolves;
	std::vector<Datagram> sent;

private:
	const FakeClock& clock_;
};

/// Creates FakeUdpSockets for a GatewaySearch and keeps every one (released ones too) for the test
/// to inspect; counts what was created and released, so that a test can see a leak.
class FakeUdpSocketFactory : public hc::UdpSocketFactory {
public:
	explicit FakeUdpSocketFactory(const FakeClock& clock) : clock_(clock) {}

	hc::UdpSocket* create(const char** error) override {
		if (fail_create) {
			*error = "not enough memory";
			return nullptr;
		}
		sockets.emplace_back(new FakeUdpSocket(clock_));
		sockets.back()->fail_open = fail_open;
		++created;
		return sockets.back().get();
	}

	void release(hc::UdpSocket* socket) override {
		if (socket == nullptr) {
			return;
		}
		socket->close();
		++released;
		for (auto& s : sockets) {
			if (s.get() == socket) {
				return;
			}
		}
		FAIL("released a socket this factory did not create");
	}

	/// The socket created last.
	FakeUdpSocket& last() {
		REQUIRE(!sockets.empty());
		return *sockets.back();
	}

	int live() const { return created - released; }

	bool fail_create = false;
	bool fail_open = false;
	int created = 0;
	int released = 0;
	std::vector<std::unique_ptr<FakeUdpSocket>> sockets;

private:
	const FakeClock& clock_;
};

// Frames of the gateway.

inline Bytes connect_response(uint8_t channel, uint8_t status = 0, const char* address = "11 FA") {
	if (status != 0) {
		Bytes result = bytes("06 10 02 06 00 08");
		result.push_back(channel);
		result.push_back(status);
		return result;
	}
	// Data endpoint 192.0.2.1:3671, CRD tunnel connection with the assigned address (1.1.250).
	Bytes result = bytes("06 10 02 06 00 14");
	result.push_back(channel);
	result.push_back(0);
	Bytes rest = bytes("08 01 C0 00 02 01 0E 57 04 04");
	result.insert(result.end(), rest.begin(), rest.end());
	Bytes ia = bytes(address);
	result.insert(result.end(), ia.begin(), ia.end());
	return result;
}

inline Bytes connectionstate_response(uint8_t channel, uint8_t status = 0) {
	return Bytes{0x06, 0x10, 0x02, 0x08, 0x00, 0x08, channel, status};
}

inline Bytes tunneling_ack(uint8_t channel, uint8_t sequence, uint8_t status = 0) {
	return Bytes{0x06, 0x10, 0x04, 0x21, 0x00, 0x0A, 0x04, channel, sequence, status};
}

inline Bytes tunneling_request(uint8_t channel, uint8_t sequence, const Bytes& cemi) {
	Bytes result{0x06, 0x10, 0x04, 0x20, 0x00, static_cast<uint8_t>(10 + cemi.size()), 0x04, channel, sequence, 0x00};
	result.insert(result.end(), cemi.begin(), cemi.end());
	return result;
}

inline Bytes disconnect_request(uint8_t channel) {
	Bytes result{0x06, 0x10, 0x02, 0x09, 0x00, 0x10, channel, 0x00};
	Bytes hpai = bytes("08 01 C0 00 02 01 0E 57");
	result.insert(result.end(), hpai.begin(), hpai.end());
	return result;
}

/// Records what a tunnel tells its listeners.
/// The records a TelegramLog hands on, kept for a test (the device keeps them in the monitor ring).
struct TelegramRecorder : hc::knx::TelegramListener {
	std::vector<hc::knx::TelegramRecord> records;
	void telegram_logged(const hc::knx::TelegramRecord& record, uint64_t) override { records.push_back(record); }
	size_t size() const { return records.size(); }
	const hc::knx::TelegramRecord* at(size_t i) const { return i < records.size() ? &records[i] : nullptr; }
};

struct TunnelRecorder : hc::knx::KnxTunnelListener {
	void tunnel_state_changed(const hc::knx::KnxTunnel& tunnel, hc::knx::TunnelState previous) override {
		states.push_back(std::string(hc::knx::tunnel_state_name(previous)) + "->" +
			hc::knx::tunnel_state_name(tunnel.state()));
	}

	void tunnel_frame(const hc::knx::KnxTunnel&, const hc::knx::LData& frame) override { frames.push_back(frame); }

	void tunnel_sent(const hc::knx::KnxTunnel&, uint32_t token, const hc::knx::LData& frame,
		hc::knx::SendOutcome outcome) override {
		sent.push_back(Sent{token, frame, outcome});
	}

	struct Sent {
		uint32_t token;
		hc::knx::LData frame;
		hc::knx::SendOutcome outcome;
	};

	std::vector<std::string> states;
	std::vector<hc::knx::LData> frames;
	std::vector<Sent> sent;
};

/// A tunnel to the test gateway under the fake clock, with the network up.
struct TunnelFixture {
	ScopedCapture capture{hc::LogLevel::debug};
	FakeClock clock;
	hc::EventLoop loop;
	FakeNetworkDriver driver{clock};
	hc::Connectivity connectivity{loop, clock, driver};
	FakeUdpSocket socket{clock};
	hc::knx::KnxTunnel tunnel{loop, clock, connectivity, socket};
	TunnelRecorder recorder;

	TunnelFixture() {
		tunnel.add_listener(&recorder);
		connectivity.start();
		connectivity.link_up("192.0.2.100");
	}

	void set_gateway(const char* text, bool nat = false) {
		std::optional<hc::knx::GatewayAddress> gateway = hc::knx::GatewayAddress::parse(text);
		REQUIRE(gateway);
		tunnel.set_gateway(&*gateway, nat);
	}

	/// Runs the loop, jumping the clock from deadline to deadline, until `until_ms`.
	void run_until(uint64_t until_ms) {
		for (;;) {
			loop.run_once(clock.now_ms);
			uint64_t deadline = loop.next_deadline();
			if (deadline > until_ms) {
				clock.now_ms = until_ms;
				loop.run_once(clock.now_ms);
				return;
			}
			if (deadline > clock.now_ms) {
				clock.now_ms = deadline;
			}
		}
	}

	void run_for(uint64_t ms) { run_until(clock.now_ms + ms); }

	/// Sets the gateway, starts, and answers the CONNECT_REQUEST: the tunnel is up on `channel`.
	void connect(uint8_t channel = 7) {
		set_gateway("192.0.2.1");
		tunnel.start();
		run_for(0);
		REQUIRE(socket.count(hc::knx::ip::Service::connect_request) >= 1);
		socket.inject(connect_response(channel));
		REQUIRE(tunnel.is_up());
	}
};

} // namespace hc_test

#endif // HC_KNX_TEST_SUPPORT_H
