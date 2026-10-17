/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/knx/knxnetip.h"

#include <cstring>

namespace hc {
namespace knx {
namespace ip {

namespace {

constexpr uint8_t kHpaiLength = 8;
constexpr uint8_t kProtocolUdp = 0x01;
constexpr uint8_t kTunnelConnection = 0x04;
constexpr uint8_t kTunnelLinkLayer = 0x02;
constexpr uint8_t kDibDeviceInfo = 0x01;
constexpr uint8_t kDibSupportedServices = 0x02;
constexpr uint8_t kDeviceInfoLength = 54;

ParseResult fail(ParseStatus status, const char* reason) {
	return ParseResult{status, reason};
}

void put16(uint8_t* p, uint16_t value) {
	p[0] = static_cast<uint8_t>(value >> 8);
	p[1] = static_cast<uint8_t>(value);
}

uint16_t get16(const uint8_t* p) {
	return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t get32(const uint8_t* p) {
	return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
		(static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void header(FrameBuffer& out, Service service, size_t total) {
	out.data[0] = kHeaderLength;
	out.data[1] = kVersion;
	put16(out.data + 2, static_cast<uint16_t>(service));
	put16(out.data + 4, static_cast<uint16_t>(total));
	out.length = total;
}

void hpai(uint8_t* p, const Endpoint& endpoint) {
	p[0] = kHpaiLength;
	p[1] = kProtocolUdp;
	p[2] = static_cast<uint8_t>(endpoint.address >> 24);
	p[3] = static_cast<uint8_t>(endpoint.address >> 16);
	p[4] = static_cast<uint8_t>(endpoint.address >> 8);
	p[5] = static_cast<uint8_t>(endpoint.address);
	put16(p + 6, endpoint.port);
}

ParseResult read_hpai(const uint8_t* p, size_t available, Endpoint& out) {
	if (available < kHpaiLength) {
		return fail(ParseStatus::wrong_length, "HPAI cut short");
	}
	if (p[0] != kHpaiLength) {
		return fail(ParseStatus::invalid, "HPAI length is not 8");
	}
	if (p[1] != kProtocolUdp) {
		return fail(ParseStatus::invalid, "HPAI is not UDP over IPv4");
	}
	out.address = get32(p + 2);
	out.port = get16(p + 6);
	return ParseResult{};
}

/// Checks the header for `expected` and a body of at least `minimum` octets after it.
ParseResult expect(const uint8_t* frame, size_t length, Service expected, size_t minimum) {
	uint16_t service = 0;
	ParseResult result = parse_header(frame, length, service);
	if (!result) {
		return result;
	}
	if (service != static_cast<uint16_t>(expected)) {
		return fail(ParseStatus::wrong_service, "another service type");
	}
	if (length < kHeaderLength + minimum) {
		return fail(ParseStatus::wrong_length, "frame shorter than its fields");
	}
	return ParseResult{};
}

ParseResult channel_status(const uint8_t* frame, size_t length, Service service, ChannelStatus& out) {
	ParseResult result = expect(frame, length, service, 2);
	if (!result) {
		return result;
	}
	out.channel = frame[6];
	out.status = frame[7];
	return ParseResult{};
}

/// The connection header of a tunnelling request or acknowledgement: length 4, channel, sequence,
/// status (reserved in a request).
ParseResult connection_header(const uint8_t* frame, size_t length, Service service, uint8_t& channel,
	uint8_t& sequence, uint8_t& status) {
	ParseResult result = expect(frame, length, service, 4);
	if (!result) {
		return result;
	}
	if (frame[6] != 4) {
		return fail(ParseStatus::invalid, "connection header length is not 4");
	}
	channel = frame[7];
	sequence = frame[8];
	status = frame[9];
	return ParseResult{};
}

} // namespace

const char* service_name(uint16_t service) {
	switch (static_cast<Service>(service)) {
	case Service::search_request:
		return "SEARCH_REQUEST";
	case Service::search_response:
		return "SEARCH_RESPONSE";
	case Service::description_request:
		return "DESCRIPTION_REQUEST";
	case Service::description_response:
		return "DESCRIPTION_RESPONSE";
	case Service::connect_request:
		return "CONNECT_REQUEST";
	case Service::connect_response:
		return "CONNECT_RESPONSE";
	case Service::connectionstate_request:
		return "CONNECTIONSTATE_REQUEST";
	case Service::connectionstate_response:
		return "CONNECTIONSTATE_RESPONSE";
	case Service::disconnect_request:
		return "DISCONNECT_REQUEST";
	case Service::disconnect_response:
		return "DISCONNECT_RESPONSE";
	case Service::tunneling_request:
		return "TUNNELING_REQUEST";
	case Service::tunneling_ack:
		return "TUNNELING_ACK";
	case Service::routing_indication:
		return "ROUTING_INDICATION";
	}
	return "unknown";
}

const char* status_name(uint8_t code) {
	switch (code) {
	case status::kNoError:
		return "E_NO_ERROR";
	case status::kHostProtocolType:
		return "E_HOST_PROTOCOL_TYPE";
	case status::kVersionNotSupported:
		return "E_VERSION_NOT_SUPPORTED";
	case status::kSequenceNumber:
		return "E_SEQUENCE_NUMBER";
	case status::kConnectionId:
		return "E_CONNECTION_ID";
	case status::kConnectionType:
		return "E_CONNECTION_TYPE";
	case status::kConnectionOption:
		return "E_CONNECTION_OPTION";
	case status::kNoMoreConnections:
		return "E_NO_MORE_CONNECTIONS";
	case status::kDataConnection:
		return "E_DATA_CONNECTION";
	case status::kKnxConnection:
		return "E_KNX_CONNECTION";
	case status::kTunnellingLayer:
		return "E_TUNNELLING_LAYER";
	default:
		return "unknown status";
	}
}

void build_connect_request(FrameBuffer& out, const Endpoint& control, const Endpoint& data) {
	header(out, Service::connect_request, 26);
	hpai(out.data + 6, control);
	hpai(out.data + 14, data);
	out.data[22] = 4;
	out.data[23] = kTunnelConnection;
	out.data[24] = kTunnelLinkLayer;
	out.data[25] = 0;
}

void build_connectionstate_request(FrameBuffer& out, uint8_t channel, const Endpoint& control) {
	header(out, Service::connectionstate_request, 16);
	out.data[6] = channel;
	out.data[7] = 0;
	hpai(out.data + 8, control);
}

void build_disconnect_request(FrameBuffer& out, uint8_t channel, const Endpoint& control) {
	header(out, Service::disconnect_request, 16);
	out.data[6] = channel;
	out.data[7] = 0;
	hpai(out.data + 8, control);
}

void build_disconnect_response(FrameBuffer& out, uint8_t channel, uint8_t status) {
	header(out, Service::disconnect_response, 8);
	out.data[6] = channel;
	out.data[7] = status;
}

bool build_tunneling_request(FrameBuffer& out, uint8_t channel, uint8_t sequence, const LData& frame,
	CemiResult* result) {
	size_t cemi_length = 0;
	CemiResult status = encode(frame, out.data + 10, FrameBuffer::kCapacity - 10, cemi_length);
	if (result != nullptr) {
		*result = status;
	}
	if (!status) {
		out.length = 0;
		return false;
	}
	header(out, Service::tunneling_request, 10 + cemi_length);
	out.data[6] = 4;
	out.data[7] = channel;
	out.data[8] = sequence;
	out.data[9] = 0;
	return true;
}

void build_tunneling_ack(FrameBuffer& out, uint8_t channel, uint8_t sequence, uint8_t status) {
	header(out, Service::tunneling_ack, 10);
	out.data[6] = 4;
	out.data[7] = channel;
	out.data[8] = sequence;
	out.data[9] = status;
}

void build_search_request(FrameBuffer& out, const Endpoint& discovery) {
	header(out, Service::search_request, 14);
	hpai(out.data + 6, discovery);
}

ParseResult parse_header(const uint8_t* data, size_t length, uint16_t& service) {
	if (length < kHeaderLength) {
		return fail(ParseStatus::wrong_length, "datagram shorter than the KNXnet/IP header");
	}
	if (data[0] != kHeaderLength || data[1] != kVersion) {
		return fail(ParseStatus::wrong_header, "not a KNXnet/IP 1.0 frame");
	}
	if (get16(data + 4) != length) {
		return fail(ParseStatus::wrong_length, "total length disagrees with the datagram");
	}
	service = get16(data + 2);
	return ParseResult{};
}

ParseResult parse_connect_response(const uint8_t* frame, size_t length, ConnectResponse& out) {
	ParseResult result = expect(frame, length, Service::connect_response, 2);
	if (!result) {
		return result;
	}
	out = ConnectResponse{};
	out.channel = frame[6];
	out.status = frame[7];
	if (out.status != status::kNoError) {
		return ParseResult{}; // an error response may end after the status
	}
	result = read_hpai(frame + 8, length - 8, out.data);
	if (!result) {
		return result;
	}
	const uint8_t* crd = frame + 16;
	size_t rest = length - 16;
	if (rest < 4) {
		return fail(ParseStatus::wrong_length, "CRD cut short");
	}
	if (crd[0] != 4 || crd[1] != kTunnelConnection) {
		return fail(ParseStatus::invalid, "CRD is not a tunnel connection's (length 4, type 4)");
	}
	out.address.raw = get16(crd + 2);
	return ParseResult{};
}

ParseResult parse_connectionstate_response(const uint8_t* frame, size_t length, ChannelStatus& out) {
	return channel_status(frame, length, Service::connectionstate_response, out);
}

ParseResult parse_disconnect_response(const uint8_t* frame, size_t length, ChannelStatus& out) {
	return channel_status(frame, length, Service::disconnect_response, out);
}

ParseResult parse_disconnect_request(const uint8_t* frame, size_t length, DisconnectRequest& out) {
	ParseResult result = expect(frame, length, Service::disconnect_request, 2);
	if (!result) {
		return result;
	}
	out.channel = frame[6];
	return read_hpai(frame + 8, length - 8, out.control);
}

ParseResult parse_tunneling_request(const uint8_t* frame, size_t length, TunnelingRequest& out) {
	uint8_t reserved = 0;
	ParseResult result =
		connection_header(frame, length, Service::tunneling_request, out.channel, out.sequence, reserved);
	if (!result) {
		return result;
	}
	out.cemi = decode(frame + 10, length - 10, out.frame);
	if (!out.cemi) {
		return fail(ParseStatus::cemi, out.cemi.reason);
	}
	return ParseResult{};
}

ParseResult parse_tunneling_ack(const uint8_t* frame, size_t length, TunnelingAck& out) {
	return connection_header(frame, length, Service::tunneling_ack, out.channel, out.sequence, out.status);
}

uint8_t ServiceFamilies::version(uint8_t family) const {
	for (uint8_t i = 0; i < count; ++i) {
		if (entries[i].family == family) {
			return entries[i].version;
		}
	}
	return 0;
}

ParseResult parse_search_response(const uint8_t* frame, size_t length, SearchResponse& out) {
	ParseResult result = expect(frame, length, Service::search_response, kHpaiLength);
	if (!result) {
		return result;
	}
	out = SearchResponse{};
	result = read_hpai(frame + 6, length - 6, out.control);
	if (!result) {
		return result;
	}
	bool device = false;
	bool services = false;
	size_t offset = 14;
	while (offset < length) {
		if (length - offset < 2) {
			return fail(ParseStatus::wrong_length, "DIB cut short");
		}
		const uint8_t* dib = frame + offset;
		size_t dib_length = dib[0];
		if (dib_length < 2 || dib_length > length - offset) {
			return fail(ParseStatus::invalid, "DIB length outside the frame");
		}
		if (dib[1] == kDibDeviceInfo) {
			if (dib_length != kDeviceInfoLength) {
				return fail(ParseStatus::invalid, "device information DIB is not 54 octets");
			}
			DeviceInfo& info = out.device;
			info.medium = dib[2];
			info.device_status = dib[3];
			info.address.raw = get16(dib + 4);
			info.installation = get16(dib + 6);
			std::memcpy(info.serial, dib + 8, 6);
			info.multicast = get32(dib + 14);
			std::memcpy(info.mac, dib + 18, 6);
			std::memcpy(info.name, dib + 24, DeviceInfo::kMaxName);
			info.name[DeviceInfo::kMaxName] = '\0';
			device = true;
		} else if (dib[1] == kDibSupportedServices) {
			if (dib_length % 2 != 0) {
				return fail(ParseStatus::invalid, "supported services DIB has an odd length");
			}
			for (size_t i = 2; i + 1 < dib_length; i += 2) {
				if (out.families.count < ServiceFamilies::kCapacity) {
					out.families.entries[out.families.count++] = ServiceFamilies::Entry{dib[i], dib[i + 1]};
				}
			}
			services = true;
		}
		offset += dib_length;
	}
	if (!device || !services) {
		return fail(ParseStatus::invalid, "search response without device information or supported services");
	}
	return ParseResult{};
}

} // namespace ip
} // namespace knx
} // namespace hc
