/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_MQTT_CODEC_H
#define HC_MQTT_CODEC_H

#include <cstddef>
#include <cstdint>

#include "hc/platform/mqtt_client.h"

namespace hc {
namespace mqtt {

/// The MQTT 3.1.1 wire format (OASIS standard, 2014) as far as a client needs it: the packets a
/// client sends (CONNECT with a last will, PUBLISH QoS 0/1, PUBACK, SUBSCRIBE, UNSUBSCRIBE,
/// PINGREQ, DISCONNECT) and those it receives (CONNACK, PUBLISH, PUBACK, SUBACK, UNSUBACK,
/// PINGRESP). Pure functions over caller buffers; the POSIX client uses them, the ESP build has
/// esp-mqtt. QoS 2 is not supported (the core never asks for it).

/// Control packet types (the high nibble of the first byte).
enum class PacketType : uint8_t {
	connect = 1,
	connack = 2,
	publish = 3,
	puback = 4,
	pubrec = 5,
	pubrel = 6,
	pubcomp = 7,
	subscribe = 8,
	suback = 9,
	unsubscribe = 10,
	unsuback = 11,
	pingreq = 12,
	pingresp = 13,
	disconnect = 14,
};

/// Largest remaining length the format can express (four length bytes).
constexpr size_t kMaxRemainingLength = 268435455;

/// The fixed header of a packet.
struct Header {
	PacketType type;
	uint8_t flags;            ///< The low nibble of the first byte.
	size_t remaining_length;  ///< Bytes after the fixed header.
	size_t header_length;     ///< 2..5.
};

enum class DecodeStatus : uint8_t {
	ok,
	need_more, ///< The bytes so far are a valid prefix.
	invalid,   ///< Malformed (a length of more than four bytes, a reserved type).
};

/// Reads the fixed header from the first `length` bytes.
DecodeStatus decode_header(const uint8_t* data, size_t length, Header& out);

/// The encoders write one complete packet into `out` and return its length, or 0 when it does not
/// fit `size` or an argument is invalid (a string longer than 65535 bytes, QoS above 1, packet id
/// 0 where one is needed).

/// CONNECT with clean session; the will, user name and password from `options` (each optional).
size_t encode_connect(uint8_t* out, size_t size, const MqttConnectOptions& options);
/// PUBLISH; `packet_id` is used (and must not be 0) for QoS 1 only.
size_t encode_publish(uint8_t* out, size_t size, const char* topic, const uint8_t* payload, size_t length,
	uint8_t qos, bool retain, uint16_t packet_id, bool dup = false);
size_t encode_puback(uint8_t* out, size_t size, uint16_t packet_id);
/// SUBSCRIBE of one filter.
size_t encode_subscribe(uint8_t* out, size_t size, uint16_t packet_id, const char* filter, uint8_t qos);
/// UNSUBSCRIBE of one filter.
size_t encode_unsubscribe(uint8_t* out, size_t size, uint16_t packet_id, const char* filter);
size_t encode_pingreq(uint8_t* out, size_t size);
size_t encode_disconnect(uint8_t* out, size_t size);

/// The bytes a PUBLISH needs (to size a buffer); 0 if it cannot be encoded.
size_t publish_size(const char* topic, size_t length, uint8_t qos);

/// A received PUBLISH: the views point into the packet's body.
struct Publish {
	const char* topic;
	size_t topic_length;
	const uint8_t* payload;
	size_t payload_length;
	uint16_t packet_id; ///< 0 for QoS 0.
	uint8_t qos;
	bool retain;
	bool dup;
};

/// The body decoders take the packet after its fixed header (`header.remaining_length` bytes) and
/// return false when it is malformed.

/// CONNACK: `session_present` and the return code (0: accepted; see connack_reason()).
bool decode_connack(const uint8_t* body, size_t length, bool& session_present, uint8_t& code);
bool decode_publish(const Header& header, const uint8_t* body, size_t length, Publish& out);
/// PUBACK and UNSUBACK: the packet id.
bool decode_packet_id(const uint8_t* body, size_t length, uint16_t& packet_id);
/// SUBACK of one filter: the packet id and the granted QoS (0x80: refused).
bool decode_suback(const uint8_t* body, size_t length, uint16_t& packet_id, uint8_t& granted);

/// The text of a CONNACK return code ("unacceptable protocol version", "not authorized", ...).
const char* connack_reason(uint8_t code);

/// A broker URL, split.
struct Broker {
	bool tls = false;
	char host[128] = {};
	uint16_t port = 0;
};

/// Splits `mqtt://host[:port]` or `mqtts://host[:port]` (an optional trailing "/"); the host is a
/// name of [A-Za-z0-9.-] or an IPv4 literal, at most 127 bytes; the port 1..65535 (default 1883,
/// 8883 with TLS). False with `error` naming the problem.
bool parse_broker(const char* url, Broker& out, const char** error = nullptr);

/// Whether `topic` is a valid topic name for PUBLISH: 1..MqttClient::kMaxTopic bytes, no `+`, no
/// `#`, no NUL, no control characters.
bool topic_valid(const char* topic);

/// Whether `topic` matches the subscription `filter` (`+` one level, `#` the rest).
bool topic_matches(const char* filter, const char* topic);

} // namespace mqtt
} // namespace hc

#endif // HC_MQTT_CODEC_H
