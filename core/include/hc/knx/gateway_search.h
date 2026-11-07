/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_GATEWAY_SEARCH_H
#define HC_KNX_GATEWAY_SEARCH_H

#include <cstddef>
#include <cstdint>

#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/knx/address.h"
#include "hc/knx/knxnetip.h"
#include "hc/platform/clock.h"
#include "hc/platform/udp_socket.h"

namespace hc {
namespace knx {

/// The observable state of a GatewaySearch.
enum class SearchState : uint8_t {
	idle,    ///< No search since the start.
	running, ///< The window is open: responses are collected.
	done,    ///< The window closed; the gateways found stay until the next search.
};

/// "idle", "running", "done".
const char* search_state_name(SearchState state);

/// One gateway that answered a search.
struct FoundGateway {
	/// Longest friendly name in UTF-8 including the NUL: the 30 octets of ISO 8859-1 the device
	/// information DIB carries take at most two octets each.
	static constexpr size_t kMaxName = 2 * ip::DeviceInfo::kMaxName + 1;

	/// The control endpoint of its HPAI (a NAT gateway's 0.0.0.0:0 replaced by where it answered from).
	ip::Endpoint control;
	IndividualAddress address;
	uint8_t mac[6] = {};
	uint8_t serial[6] = {};
	char name[kMaxName] = {};
	/// The supported service families DIB lists tunnelling (0x04) and routing (0x05).
	bool tunnelling = false;
	bool routing = false;
};

/// The KNXnet/IP gateway search (KNX 03.08.02 core, "discovery"): a SEARCH_REQUEST to the system
/// multicast group 224.0.23.12:3671 with the HPAI of a socket of its own, then the SEARCH_RESPONSEs
/// the gateways send back to that endpoint, collected for kWindowMs. The request is sent twice (at
/// the start and after kRepeatMs), against a datagram lost on the WiFi; the responses are
/// deduplicated by control endpoint. It only offers: a gateway is never chosen from it
/// automatically (doc/knx.md).
///
/// It runs on the event loop like everything else: start() sends and returns, timers close the
/// window. One search at a time: start() while one runs answers with the running one. Datagrams that
/// are not a well-formed SEARCH_RESPONSE are counted and ignored.
///
/// Memory: nothing while idle. A search takes its socket from the platform's UdpSocketFactory (on
/// the ESP with a reader task, on the heap) and gives it back when the window closes; the table of
/// kCapacity gateways is allocated at the start, shrunk to the gateways found when the window
/// closes, and kept until the next search (further gateways are counted in overflow()). An
/// allocation that fails refuses the start with the reason.
///
/// Threading: every method is called on the event loop's thread.
class GatewaySearch : public UdpReceiver {
public:
	static constexpr uint32_t kWindowMs = 3000;
	static constexpr uint32_t kRepeatMs = 1000;
	static constexpr size_t kCapacity = 16;
	/// The KNXnet/IP system setup multicast address 224.0.23.12 (host byte order) and its port.
	static constexpr uint32_t kMulticastAddress = 0xE000170C;
	static constexpr uint16_t kMulticastPort = ip::kPort;

	/// `sockets` gives the search a socket of its own (not the tunnel's) for the time a search runs.
	/// `target` is where the SEARCH_REQUEST goes: the multicast group; a test points it to a gateway
	/// on the loopback interface instead, so that no search leaves the host.
	GatewaySearch(EventLoop& loop, const Clock& clock, Connectivity& connectivity, UdpSocketFactory& sockets,
		Ipv4Endpoint target = Ipv4Endpoint{kMulticastAddress, kMulticastPort});
	~GatewaySearch();

	GatewaySearch(const GatewaySearch&) = delete;
	GatewaySearch& operator=(const GatewaySearch&) = delete;

	/// Starts a search, forgetting the previous result. While one runs it is left as it is and
	/// `*started` is false. False with `error` (logged) when it cannot start: the network is down,
	/// no memory for the table or the socket, the socket cannot be opened, there is no local address,
	/// or the request cannot be sent. The previous result is then gone too.
	bool start(const char** error, bool* started = nullptr);

	SearchState state() const { return state_; }
	bool running() const { return state_ == SearchState::running; }
	/// The gateways found so far (also while running), in the order they answered.
	size_t count() const { return count_; }
	const FoundGateway& gateway(size_t index) const { return gateways_[index]; }
	/// Responses of further gateways once kCapacity were found.
	uint32_t overflow() const { return overflow_; }
	/// Datagrams ignored during the search: no KNXnet/IP frame, another service, a malformed response.
	uint32_t ignored() const { return ignored_; }
	/// When the last search started and when its window closes (monotonic ms; 0 before the first).
	uint64_t started_ms() const { return started_ms_; }
	uint64_t ends_ms() const { return started_ms_ == 0 ? 0 : started_ms_ + kWindowMs; }

	// UdpReceiver
	void udp_received(const uint8_t* data, size_t length, const Ipv4Endpoint& from) override;
	void udp_error(const char* reason) override;
	void udp_resolved(bool ok, uint32_t address, const char* error) override;

private:
	bool send_request(const char** error);
	/// Refuses a start: gives the socket back, logs, sets `error`.
	bool refuse(const char* reason, const char** error);
	void release_socket();
	void free_table();
	void repeat_due();
	void window_closed();
	void add(const ip::SearchResponse& response, const Ipv4Endpoint& from);

	EventLoop& loop_;
	const Clock& clock_;
	Connectivity& connectivity_;
	UdpSocketFactory& sockets_;
	UdpSocket* socket_ = nullptr;
	Ipv4Endpoint target_;
	SearchState state_ = SearchState::idle;
	Ipv4Endpoint local_;
	uint64_t started_ms_ = 0;
	TimerHandle repeat_timer_;
	TimerHandle window_timer_;
	FoundGateway* gateways_ = nullptr; ///< kCapacity while running, count_ after (nullptr for none).
	size_t count_ = 0;
	uint32_t overflow_ = 0;
	uint32_t ignored_ = 0;
	char error_[112] = {};
};

} // namespace knx
} // namespace hc

#endif // HC_KNX_GATEWAY_SEARCH_H
