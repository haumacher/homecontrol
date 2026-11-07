/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/knx/gateway_search.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "hc/log.h"

namespace hc {
namespace knx {

namespace {

constexpr const char* kTag = "hc.knx";

/// The friendly name (ISO 8859-1, NUL-padded) as UTF-8 into `out` (FoundGateway::kMaxName); control
/// characters become '?'.
void latin1_to_utf8(const char* name, char* out) {
	size_t length = 0;
	for (size_t i = 0; i < ip::DeviceInfo::kMaxName && name[i] != '\0'; ++i) {
		uint8_t c = static_cast<uint8_t>(name[i]);
		if (c < 0x20 || c == 0x7F) {
			out[length++] = '?';
		} else if (c < 0x80) {
			out[length++] = static_cast<char>(c);
		} else {
			out[length++] = static_cast<char>(0xC0 | (c >> 6));
			out[length++] = static_cast<char>(0x80 | (c & 0x3F));
		}
	}
	// Trailing blanks of a name padded with spaces instead of NULs.
	while (length > 0 && out[length - 1] == ' ') {
		--length;
	}
	out[length] = '\0';
}

} // namespace

const char* search_state_name(SearchState state) {
	switch (state) {
		case SearchState::idle: return "idle";
		case SearchState::running: return "running";
		case SearchState::done: return "done";
	}
	return "unknown";
}

GatewaySearch::GatewaySearch(EventLoop& loop, const Clock& clock, Connectivity& connectivity,
	UdpSocketFactory& sockets, Ipv4Endpoint target)
	: loop_(loop), clock_(clock), connectivity_(connectivity), sockets_(sockets), target_(target) {}

GatewaySearch::~GatewaySearch() {
	loop_.cancel(repeat_timer_);
	loop_.cancel(window_timer_);
	release_socket();
	free_table();
}

void GatewaySearch::release_socket() {
	if (socket_ != nullptr) {
		socket_->close();
		sockets_.release(socket_);
		socket_ = nullptr;
	}
}

void GatewaySearch::free_table() {
	delete[] gateways_;
	gateways_ = nullptr;
}

bool GatewaySearch::refuse(const char* reason, const char** error) {
	release_socket();
	*error = reason;
	Log::warn(kTag, "gateway search refused: %s", reason);
	return false;
}

bool GatewaySearch::start(const char** error, bool* started) {
	if (started != nullptr) {
		*started = false;
	}
	if (state_ == SearchState::running) {
		return true;
	}
	if (!connectivity_.is_up()) {
		return refuse("the network is not up", error);
	}
	// The previous result goes; its table first, so that the new one can take its place.
	free_table();
	count_ = 0;
	overflow_ = 0;
	ignored_ = 0;
	state_ = SearchState::idle;
	started_ms_ = 0;
	gateways_ = new (std::nothrow) FoundGateway[kCapacity];
	if (gateways_ == nullptr) {
		return refuse("not enough memory for the search's table", error);
	}
	const char* reason = nullptr;
	socket_ = sockets_.create(&reason);
	if (socket_ == nullptr) {
		free_table();
		std::snprintf(error_, sizeof(error_), "no socket for the search: %s", reason);
		return refuse(error_, error);
	}
	if (!socket_->open(*this, &reason)) {
		free_table();
		std::snprintf(error_, sizeof(error_), "no socket for the search: %s", reason);
		return refuse(error_, error);
	}
	if (!socket_->local_endpoint(target_, local_, &reason)) {
		free_table();
		std::snprintf(error_, sizeof(error_), "no local address for the search: %s", reason);
		return refuse(error_, error);
	}
	if (!send_request(error)) {
		free_table();
		return refuse(*error, error);
	}
	uint64_t now = clock_.monotonic_ms();
	window_timer_ = loop_.schedule_at(now + kWindowMs, Callback::member<GatewaySearch, &GatewaySearch::window_closed>(this));
	if (!window_timer_.valid()) {
		// Without the timer the window would never close.
		free_table();
		return refuse("no timer left for the search window", error);
	}
	repeat_timer_ = loop_.schedule_at(now + kRepeatMs, Callback::member<GatewaySearch, &GatewaySearch::repeat_due>(this));
	state_ = SearchState::running;
	started_ms_ = now;
	char target[Ipv4Endpoint::kMaxText];
	char local[Ipv4Endpoint::kMaxText];
	Log::info(kTag, "gateway search started: SEARCH_REQUEST to %s, answers to %s for %u ms",
		target_.format(target, sizeof(target)), local_.format(local, sizeof(local)), static_cast<unsigned>(kWindowMs));
	if (started != nullptr) {
		*started = true;
	}
	return true;
}

bool GatewaySearch::send_request(const char** error) {
	ip::FrameBuffer frame;
	ip::build_search_request(frame, local_);
	const char* reason = nullptr;
	if (socket_ == nullptr || !socket_->send_to(target_, frame.data, frame.length, &reason)) {
		std::snprintf(error_, sizeof(error_), "SEARCH_REQUEST not sent: %s", reason != nullptr ? reason : "no socket");
		*error = error_;
		return false;
	}
	return true;
}

void GatewaySearch::repeat_due() {
	repeat_timer_ = TimerHandle();
	if (state_ != SearchState::running) {
		return;
	}
	const char* error = nullptr;
	if (!send_request(&error)) {
		Log::warn(kTag, "gateway search: repeated %s", error);
	}
}

void GatewaySearch::window_closed() {
	window_timer_ = TimerHandle();
	loop_.cancel(repeat_timer_);
	release_socket();
	state_ = SearchState::done;
	// Keep only what was found (nothing for none); the full table if the smaller one is not there.
	if (count_ == 0) {
		free_table();
	} else if (count_ < kCapacity) {
		FoundGateway* found = new (std::nothrow) FoundGateway[count_];
		if (found != nullptr) {
			for (size_t i = 0; i < count_; ++i) {
				found[i] = gateways_[i];
			}
			free_table();
			gateways_ = found;
		}
	}
	Log::info(kTag, "gateway search done: %u gateway(s) answered%s, %u datagram(s) ignored",
		static_cast<unsigned>(count_ + overflow_), overflow_ > 0 ? " (some beyond the list's capacity)" : "",
		static_cast<unsigned>(ignored_));
}

void GatewaySearch::udp_received(const uint8_t* data, size_t length, const Ipv4Endpoint& from) {
	if (state_ != SearchState::running) {
		return;
	}
	ip::SearchResponse response;
	ip::ParseResult result = ip::parse_search_response(data, length, response);
	if (!result) {
		++ignored_;
		char sender[Ipv4Endpoint::kMaxText];
		Log::debug(kTag, "gateway search: datagram from %s ignored: %s", from.format(sender, sizeof(sender)),
			result.reason);
		return;
	}
	add(response, from);
}

void GatewaySearch::add(const ip::SearchResponse& response, const Ipv4Endpoint& from) {
	// A gateway behind NAT may name 0.0.0.0:0 ("where this came from"): then the sender is the endpoint.
	ip::Endpoint control = response.control;
	if (control.address == 0) {
		control.address = from.address;
	}
	if (control.port == 0) {
		control.port = from.port;
	}
	for (size_t i = 0; i < count_; ++i) {
		if (gateways_[i].control == control) {
			return; // the answer to the repeated request
		}
	}
	if (count_ == kCapacity) {
		++overflow_;
		return;
	}
	FoundGateway& found = gateways_[count_++];
	found = FoundGateway{};
	found.control = control;
	found.address = response.device.address;
	std::memcpy(found.mac, response.device.mac, sizeof(found.mac));
	std::memcpy(found.serial, response.device.serial, sizeof(found.serial));
	latin1_to_utf8(response.device.name, found.name);
	found.tunnelling = response.families.version(ip::family::kTunnelling) != 0;
	found.routing = response.families.version(ip::family::kRouting) != 0;
	char endpoint[Ipv4Endpoint::kMaxText];
	char address[IndividualAddress::kMaxText];
	Log::info(kTag, "gateway search: \"%s\" at %s (%s)%s%s", found.name, control.format(endpoint, sizeof(endpoint)),
		found.address.format(address, sizeof(address)), found.tunnelling ? ", tunnelling" : ", no tunnelling",
		found.routing ? ", routing" : "");
}

void GatewaySearch::udp_error(const char* reason) {
	Log::warn(kTag, "gateway search: receive error: %s", reason);
}

void GatewaySearch::udp_resolved(bool, uint32_t, const char*) {
	// The search resolves nothing.
}

} // namespace knx
} // namespace hc
