/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/mqtt_codec.h"

#include <cstring>

namespace hc {
namespace mqtt {

namespace {

constexpr size_t kMaxString = 65535;

/// Sequential writer with overflow tracking.
struct Out {
	uint8_t* data;
	size_t size;
	size_t length = 0;
	bool overflow = false;

	Out(uint8_t* d, size_t s) : data(d), size(s) {}

	void byte(uint8_t b) {
		if (length >= size) {
			overflow = true;
			return;
		}
		data[length++] = b;
	}
	void u16(uint16_t v) {
		byte(static_cast<uint8_t>(v >> 8));
		byte(static_cast<uint8_t>(v & 0xFF));
	}
	void bytes(const uint8_t* b, size_t n) {
		if (n > size - length || length > size) {
			overflow = true;
			return;
		}
		if (n > 0) {
			std::memcpy(data + length, b, n);
		}
		length += n;
	}
	void string(const char* s, size_t n) {
		u16(static_cast<uint16_t>(n));
		bytes(reinterpret_cast<const uint8_t*>(s), n);
	}
	/// The fixed header with the remaining length.
	void header(uint8_t first, size_t remaining) {
		byte(first);
		do {
			uint8_t digit = static_cast<uint8_t>(remaining % 128);
			remaining /= 128;
			if (remaining > 0) {
				digit |= 0x80;
			}
			byte(digit);
		} while (remaining > 0);
	}
	size_t done() const { return overflow ? 0 : length; }
};

size_t length_bytes(size_t remaining) {
	if (remaining < 128) {
		return 1;
	}
	if (remaining < 16384) {
		return 2;
	}
	if (remaining < 2097152) {
		return 3;
	}
	return 4;
}

bool text(const char* s) { return s != nullptr && s[0] != '\0'; }

uint16_t read_u16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

bool fail(const char** error, const char* reason) {
	if (error != nullptr) {
		*error = reason;
	}
	return false;
}

} // namespace

DecodeStatus decode_header(const uint8_t* data, size_t length, Header& out) {
	if (length < 2) {
		return DecodeStatus::need_more;
	}
	uint8_t type = data[0] >> 4;
	if (type == 0 || type == 15) {
		return DecodeStatus::invalid;
	}
	size_t value = 0;
	size_t multiplier = 1;
	for (size_t i = 1; i <= 4; ++i) {
		if (i >= length) {
			return DecodeStatus::need_more;
		}
		uint8_t digit = data[i];
		value += static_cast<size_t>(digit & 0x7F) * multiplier;
		if ((digit & 0x80) == 0) {
			out.type = static_cast<PacketType>(type);
			out.flags = data[0] & 0x0F;
			out.remaining_length = value;
			out.header_length = i + 1;
			return DecodeStatus::ok;
		}
		multiplier *= 128;
	}
	return DecodeStatus::invalid;
}

size_t encode_connect(uint8_t* out, size_t size, const MqttConnectOptions& options) {
	const char* client_id = options.client_id != nullptr ? options.client_id : "";
	size_t id_length = std::strlen(client_id);
	bool will = text(options.will_topic);
	bool user = text(options.username);
	bool password = user && options.password != nullptr && options.password[0] != '\0';
	if (id_length > kMaxString || options.will_qos > 1) {
		return 0;
	}
	size_t remaining = 10 + 2 + id_length;
	size_t will_topic_length = 0;
	size_t will_payload_length = 0;
	if (will) {
		will_topic_length = std::strlen(options.will_topic);
		will_payload_length = options.will_payload != nullptr ? std::strlen(options.will_payload) : 0;
		if (will_topic_length > kMaxString || will_payload_length > kMaxString) {
			return 0;
		}
		remaining += 2 + will_topic_length + 2 + will_payload_length;
	}
	size_t user_length = user ? std::strlen(options.username) : 0;
	size_t password_length = password ? std::strlen(options.password) : 0;
	if (user_length > kMaxString || password_length > kMaxString) {
		return 0;
	}
	if (user) {
		remaining += 2 + user_length;
	}
	if (password) {
		remaining += 2 + password_length;
	}
	uint8_t flags = 0x02; // clean session
	if (will) {
		flags |= 0x04 | static_cast<uint8_t>(options.will_qos << 3);
		if (options.will_retain) {
			flags |= 0x20;
		}
	}
	if (password) {
		flags |= 0x40;
	}
	if (user) {
		flags |= 0x80;
	}
	Out w(out, size);
	w.header(0x10, remaining);
	w.string("MQTT", 4);
	w.byte(4); // protocol level 3.1.1
	w.byte(flags);
	w.u16(options.keepalive_s);
	w.string(client_id, id_length);
	if (will) {
		w.string(options.will_topic, will_topic_length);
		w.string(options.will_payload != nullptr ? options.will_payload : "", will_payload_length);
	}
	if (user) {
		w.string(options.username, user_length);
	}
	if (password) {
		w.string(options.password, password_length);
	}
	return w.done();
}

size_t publish_size(const char* topic, size_t length, uint8_t qos) {
	if (topic == nullptr || qos > 1) {
		return 0;
	}
	size_t topic_length = std::strlen(topic);
	if (topic_length > kMaxString) {
		return 0;
	}
	size_t remaining = 2 + topic_length + (qos > 0 ? 2 : 0) + length;
	if (remaining > kMaxRemainingLength) {
		return 0;
	}
	return 1 + length_bytes(remaining) + remaining;
}

size_t encode_publish(uint8_t* out, size_t size, const char* topic, const uint8_t* payload, size_t length,
	uint8_t qos, bool retain, uint16_t packet_id, bool dup) {
	if (publish_size(topic, length, qos) == 0 || (qos > 0 && packet_id == 0)) {
		return 0;
	}
	size_t topic_length = std::strlen(topic);
	size_t remaining = 2 + topic_length + (qos > 0 ? 2 : 0) + length;
	uint8_t first = static_cast<uint8_t>(0x30 | (qos << 1));
	if (retain) {
		first |= 0x01;
	}
	if (dup && qos > 0) {
		first |= 0x08;
	}
	Out w(out, size);
	w.header(first, remaining);
	w.string(topic, topic_length);
	if (qos > 0) {
		w.u16(packet_id);
	}
	w.bytes(payload, length);
	return w.done();
}

size_t encode_puback(uint8_t* out, size_t size, uint16_t packet_id) {
	if (packet_id == 0) {
		return 0;
	}
	Out w(out, size);
	w.header(0x40, 2);
	w.u16(packet_id);
	return w.done();
}

size_t encode_subscribe(uint8_t* out, size_t size, uint16_t packet_id, const char* filter, uint8_t qos) {
	if (packet_id == 0 || filter == nullptr || filter[0] == '\0' || qos > 1) {
		return 0;
	}
	size_t filter_length = std::strlen(filter);
	if (filter_length > kMaxString) {
		return 0;
	}
	Out w(out, size);
	w.header(0x82, 2 + 2 + filter_length + 1);
	w.u16(packet_id);
	w.string(filter, filter_length);
	w.byte(qos);
	return w.done();
}

size_t encode_unsubscribe(uint8_t* out, size_t size, uint16_t packet_id, const char* filter) {
	if (packet_id == 0 || filter == nullptr || filter[0] == '\0') {
		return 0;
	}
	size_t filter_length = std::strlen(filter);
	if (filter_length > kMaxString) {
		return 0;
	}
	Out w(out, size);
	w.header(0xA2, 2 + 2 + filter_length);
	w.u16(packet_id);
	w.string(filter, filter_length);
	return w.done();
}

size_t encode_pingreq(uint8_t* out, size_t size) {
	Out w(out, size);
	w.header(0xC0, 0);
	return w.done();
}

size_t encode_disconnect(uint8_t* out, size_t size) {
	Out w(out, size);
	w.header(0xE0, 0);
	return w.done();
}

bool decode_connack(const uint8_t* body, size_t length, bool& session_present, uint8_t& code) {
	if (length != 2 || (body[0] & 0xFE) != 0) {
		return false;
	}
	session_present = (body[0] & 0x01) != 0;
	code = body[1];
	return true;
}

bool decode_publish(const Header& header, const uint8_t* body, size_t length, Publish& out) {
	uint8_t qos = (header.flags >> 1) & 0x03;
	if (qos > 2 || length < 2) {
		return false;
	}
	size_t topic_length = read_u16(body);
	size_t position = 2 + topic_length;
	if (position > length || topic_length == 0) {
		return false;
	}
	out.topic = reinterpret_cast<const char*>(body + 2);
	out.topic_length = topic_length;
	if (std::memchr(out.topic, '\0', topic_length) != nullptr) {
		return false;
	}
	out.packet_id = 0;
	if (qos > 0) {
		if (position + 2 > length) {
			return false;
		}
		out.packet_id = read_u16(body + position);
		if (out.packet_id == 0) {
			return false;
		}
		position += 2;
	}
	out.payload = body + position;
	out.payload_length = length - position;
	out.qos = qos;
	out.retain = (header.flags & 0x01) != 0;
	out.dup = (header.flags & 0x08) != 0;
	return true;
}

bool decode_packet_id(const uint8_t* body, size_t length, uint16_t& packet_id) {
	if (length != 2) {
		return false;
	}
	packet_id = read_u16(body);
	return packet_id != 0;
}

bool decode_suback(const uint8_t* body, size_t length, uint16_t& packet_id, uint8_t& granted) {
	if (length != 3) {
		return false;
	}
	packet_id = read_u16(body);
	granted = body[2];
	return packet_id != 0 && (granted <= 2 || granted == 0x80);
}

const char* connack_reason(uint8_t code) {
	switch (code) {
		case 0: return "accepted";
		case 1: return "unacceptable protocol version";
		case 2: return "client identifier rejected";
		case 3: return "server unavailable";
		case 4: return "bad user name or password";
		case 5: return "not authorized";
		default: return "unknown return code";
	}
}

bool parse_broker(const char* url, Broker& out, const char** error) {
	out = Broker();
	if (url == nullptr) {
		return fail(error, "the broker URL is missing");
	}
	const char* rest = nullptr;
	if (std::strncmp(url, "mqtt://", 7) == 0) {
		rest = url + 7;
		out.port = 1883;
	} else if (std::strncmp(url, "mqtts://", 8) == 0) {
		rest = url + 8;
		out.tls = true;
		out.port = 8883;
	} else {
		return fail(error, "the broker URL must start with mqtt:// or mqtts://");
	}
	size_t host_length = 0;
	while (rest[host_length] != '\0' && rest[host_length] != ':' && rest[host_length] != '/') {
		char c = rest[host_length];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
		if (!ok) {
			return fail(error, "the broker host may contain only letters, digits, '.' and '-'");
		}
		++host_length;
	}
	if (host_length == 0) {
		return fail(error, "the broker URL has no host");
	}
	if (host_length >= sizeof(out.host)) {
		return fail(error, "the broker host is longer than 127 bytes");
	}
	if (rest[0] == '.' || rest[0] == '-' || rest[host_length - 1] == '.' || rest[host_length - 1] == '-') {
		return fail(error, "the broker host must not start or end with '.' or '-'");
	}
	std::memcpy(out.host, rest, host_length);
	out.host[host_length] = '\0';
	const char* p = rest + host_length;
	if (*p == ':') {
		++p;
		uint32_t port = 0;
		size_t digits = 0;
		while (*p >= '0' && *p <= '9' && digits < 6) {
			port = port * 10 + static_cast<uint32_t>(*p - '0');
			++p;
			++digits;
		}
		if (digits == 0 || port == 0 || port > 65535) {
			return fail(error, "the broker port must be 1..65535");
		}
		out.port = static_cast<uint16_t>(port);
	}
	if (*p == '/') {
		++p;
	}
	if (*p != '\0') {
		return fail(error, "the broker URL must be mqtt[s]://host[:port] (no path, user or query)");
	}
	return true;
}

bool topic_valid(const char* topic) {
	if (topic == nullptr || topic[0] == '\0') {
		return false;
	}
	size_t length = 0;
	for (const char* p = topic; *p != '\0'; ++p, ++length) {
		unsigned char c = static_cast<unsigned char>(*p);
		if (c == '+' || c == '#' || c < 0x20 || c == 0x7F) {
			return false;
		}
	}
	return length <= MqttClient::kMaxTopic;
}

bool topic_matches(const char* filter, const char* topic) {
	const char* f = filter;
	const char* t = topic;
	while (true) {
		if (*f == '#') {
			return true;
		}
		if (*f == '+') {
			while (*t != '\0' && *t != '/') {
				++t;
			}
			++f;
		} else {
			while (*f != '\0' && *f != '/' && *f == *t) {
				++f;
				++t;
			}
			if ((*f != '\0' && *f != '/') || (*t != '\0' && *t != '/')) {
				return false;
			}
		}
		if (*f == '\0' && *t == '\0') {
			return true;
		}
		if (*f == '/' && *t == '/') {
			++f;
			++t;
			// "a/#" also matches "a" (the parent level); handled when the filter continues with '#'.
			continue;
		}
		if (*f == '/' && *t == '\0' && f[1] == '#' && f[2] == '\0') {
			return true;
		}
		return false;
	}
}

} // namespace mqtt
} // namespace hc
