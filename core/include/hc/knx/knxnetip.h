/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_KNXNETIP_H
#define HC_KNX_KNXNETIP_H

#include <cstddef>
#include <cstdint>

#include "hc/ipv4.h"
#include "hc/knx/address.h"
#include "hc/knx/cemi.h"

namespace hc {
namespace knx {

/// KNXnet/IP frames (KNX 03.08.02 core, 03.08.04 tunnelling): the header, the host protocol address
/// information (HPAI), the tunnel connection's CRI/CRD and the services of a tunnelling client, plus
/// the decoding of a search response (gateway discovery). Builders write into a FrameBuffer, parsers
/// take the octets of one UDP datagram.
namespace ip {

/// The KNXnet/IP port.
constexpr uint16_t kPort = 3671;

/// Header length and protocol version 1.0 (the first two octets of every frame).
constexpr uint8_t kHeaderLength = 0x06;
constexpr uint8_t kVersion = 0x10;

/// Service type identifiers.
enum class Service : uint16_t {
	search_request = 0x0201,
	search_response = 0x0202,
	description_request = 0x0203,
	description_response = 0x0204,
	connect_request = 0x0205,
	connect_response = 0x0206,
	connectionstate_request = 0x0207,
	connectionstate_response = 0x0208,
	disconnect_request = 0x0209,
	disconnect_response = 0x020A,
	tunneling_request = 0x0420,
	tunneling_ack = 0x0421,
	routing_indication = 0x0530,
};

/// "CONNECT_REQUEST", ...; "unknown" for another value.
const char* service_name(uint16_t service);

/// Status codes of the responses and acknowledgements.
namespace status {
constexpr uint8_t kNoError = 0x00;
constexpr uint8_t kHostProtocolType = 0x01;
constexpr uint8_t kVersionNotSupported = 0x02;
constexpr uint8_t kSequenceNumber = 0x04;
constexpr uint8_t kConnectionId = 0x21;
constexpr uint8_t kConnectionType = 0x22;
constexpr uint8_t kConnectionOption = 0x23;
constexpr uint8_t kNoMoreConnections = 0x24;
constexpr uint8_t kDataConnection = 0x26;
constexpr uint8_t kKnxConnection = 0x27;
constexpr uint8_t kTunnellingLayer = 0x29;
} // namespace status

/// "E_NO_ERROR", "E_NO_MORE_CONNECTIONS", ...; "unknown status" for another value.
const char* status_name(uint8_t code);

/// Service families of the supported services DIB.
namespace family {
constexpr uint8_t kCore = 0x02;
constexpr uint8_t kDeviceManagement = 0x03;
constexpr uint8_t kTunnelling = 0x04;
constexpr uint8_t kRouting = 0x05;
constexpr uint8_t kRemoteLogging = 0x06;
constexpr uint8_t kRemoteConfiguration = 0x07;
constexpr uint8_t kObjectServer = 0x08;
} // namespace family

/// An IPv4 UDP endpoint (0.0.0.0:0 is the NAT mode endpoint).
using Endpoint = Ipv4Endpoint;

/// The octets of one frame to send.
struct FrameBuffer {
	/// The longest frame a tunnelling client builds: a TUNNELING_REQUEST with the longest cEMI frame.
	static constexpr size_t kCapacity = 10 + LData::kMaxFrame;

	uint8_t data[kCapacity] = {};
	size_t length = 0;
};

/// CONNECT_REQUEST for a tunnel connection on the link layer: control and data endpoint (the same
/// for a client with one socket; 0.0.0.0:0 in NAT mode), CRI 04 04 02 00. 26 octets.
void build_connect_request(FrameBuffer& out, const Endpoint& control, const Endpoint& data);

/// CONNECTIONSTATE_REQUEST (the heartbeat). 16 octets.
void build_connectionstate_request(FrameBuffer& out, uint8_t channel, const Endpoint& control);

/// DISCONNECT_REQUEST. 16 octets.
void build_disconnect_request(FrameBuffer& out, uint8_t channel, const Endpoint& control);

/// DISCONNECT_RESPONSE (the answer to the gateway's DISCONNECT_REQUEST). 8 octets.
void build_disconnect_response(FrameBuffer& out, uint8_t channel, uint8_t status);

/// TUNNELING_REQUEST carrying `frame` (cEMI). False with the cEMI refusal in `result`.
bool build_tunneling_request(FrameBuffer& out, uint8_t channel, uint8_t sequence, const LData& frame,
	CemiResult* result = nullptr);

/// The 10 octets in front of a cEMI frame of `cemi_length` octets that make it a TUNNELING_REQUEST
/// (header and connection header), for a frame sent as it is (KnxTunnel::send_raw()).
constexpr size_t kTunnelingHeader = 10;
void write_tunneling_header(uint8_t* out, uint8_t channel, uint8_t sequence, size_t cemi_length);

/// TUNNELING_ACK. 10 octets.
void build_tunneling_ack(FrameBuffer& out, uint8_t channel, uint8_t sequence, uint8_t status);

/// SEARCH_REQUEST with the discovery endpoint the responses go to. 14 octets.
void build_search_request(FrameBuffer& out, const Endpoint& discovery);

/// Outcome of a parser. Everything except `ok` is a refusal with a reason text.
enum class ParseStatus : uint8_t {
	ok,
	wrong_header,     ///< Not a KNXnet/IP 1.0 frame (header length, version).
	wrong_length,     ///< The total length disagrees with the datagram, or a structure is cut short.
	wrong_service,    ///< The frame is another service than the parser reads.
	invalid,          ///< A structure with a wrong length or type (HPAI, CRD, connection header, DIB).
	cemi,             ///< The cEMI frame of a TUNNELING_REQUEST was refused (see the cEMI result).
};

struct ParseResult {
	ParseStatus status = ParseStatus::ok;
	const char* reason = "";

	explicit operator bool() const { return status == ParseStatus::ok; }
};

/// Checks the header and returns the service type. Refused if the octets are not a KNXnet/IP 1.0
/// frame whose total length equals `length`.
ParseResult parse_header(const uint8_t* data, size_t length, uint16_t& service);

struct ConnectResponse {
	uint8_t channel = 0;
	uint8_t status = 0;
	/// Only with status E_NO_ERROR: the gateway's data endpoint and the individual address it
	/// assigned to the tunnel (from the CRD).
	Endpoint data;
	IndividualAddress address;
};

ParseResult parse_connect_response(const uint8_t* frame, size_t length, ConnectResponse& out);

/// The channel and status of a CONNECTIONSTATE_RESPONSE or a DISCONNECT_RESPONSE.
struct ChannelStatus {
	uint8_t channel = 0;
	uint8_t status = 0;
};

ParseResult parse_connectionstate_response(const uint8_t* frame, size_t length, ChannelStatus& out);
ParseResult parse_disconnect_response(const uint8_t* frame, size_t length, ChannelStatus& out);

/// A DISCONNECT_REQUEST from the gateway.
struct DisconnectRequest {
	uint8_t channel = 0;
	Endpoint control;
};

ParseResult parse_disconnect_request(const uint8_t* frame, size_t length, DisconnectRequest& out);

/// A TUNNELING_REQUEST. The connection header is always decoded; a cEMI refusal is reported
/// (status cemi, `cemi` holds the reason) with channel and sequence valid, so the request can be
/// acknowledged and then dropped.
struct TunnelingRequest {
	uint8_t channel = 0;
	uint8_t sequence = 0;
	LData frame;
	CemiResult cemi;
};

ParseResult parse_tunneling_request(const uint8_t* frame, size_t length, TunnelingRequest& out);

struct TunnelingAck {
	uint8_t channel = 0;
	uint8_t sequence = 0;
	uint8_t status = 0;
};

ParseResult parse_tunneling_ack(const uint8_t* frame, size_t length, TunnelingAck& out);

/// The device information DIB of a search or description response.
struct DeviceInfo {
	static constexpr size_t kMaxName = 30;

	uint8_t medium = 0;          ///< 0x02 TP1, 0x04 PL110, 0x10 RF, 0x20 KNX IP.
	uint8_t device_status = 0;   ///< Bit 0: programming mode.
	IndividualAddress address;
	uint16_t installation = 0;   ///< Project-installation identifier.
	uint8_t serial[6] = {};
	uint32_t multicast = 0;      ///< Routing multicast address (host byte order), 0 if none.
	uint8_t mac[6] = {};
	char name[kMaxName + 1] = {}; ///< Friendly name, NUL-terminated (ISO 8859-1 as sent).
};

/// The supported service families DIB.
struct ServiceFamilies {
	static constexpr size_t kCapacity = 8;

	struct Entry {
		uint8_t family;
		uint8_t version;
	};

	Entry entries[kCapacity] = {};
	uint8_t count = 0;

	/// The version of `family`, 0 if it is not supported.
	uint8_t version(uint8_t family) const;
};

struct SearchResponse {
	Endpoint control;
	DeviceInfo device;
	ServiceFamilies families;
};

/// Decodes a SEARCH_RESPONSE: the control endpoint, the device information and the supported
/// service families (more than ServiceFamilies::kCapacity are ignored; further DIBs are skipped).
ParseResult parse_search_response(const uint8_t* frame, size_t length, SearchResponse& out);

} // namespace ip
} // namespace knx
} // namespace hc

#endif // HC_KNX_KNXNETIP_H
