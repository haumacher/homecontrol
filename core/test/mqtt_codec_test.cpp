/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/mqtt_codec.h"

using namespace hc::mqtt;

namespace {

std::vector<uint8_t> bytes(std::initializer_list<int> list) {
	std::vector<uint8_t> out;
	for (int b : list) {
		out.push_back(static_cast<uint8_t>(b));
	}
	return out;
}

std::vector<uint8_t> take(const uint8_t* data, size_t length) { return std::vector<uint8_t>(data, data + length); }

} // namespace

// The expected bytes are worked out from the MQTT 3.1.1 standard (sections 2 and 3), byte by byte.

TEST_CASE("MqttCodecConnectWithWillUserAndPassword") {
	hc::MqttConnectOptions o;
	o.client_id = "hc";
	o.username = "u";
	o.password = "p";
	o.will_topic = "w";
	o.will_payload = "x";
	o.will_retain = true;
	o.will_qos = 1;
	o.keepalive_s = 60;
	uint8_t out[64];
	size_t n = encode_connect(out, sizeof(out), o);
	CHECK(take(out, n) == bytes({0x10, 0x1A, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0xEE, 0x00, 0x3C, 0x00, 0x02, 'h',
								  'c', 0x00, 0x01, 'w', 0x00, 0x01, 'x', 0x00, 0x01, 'u', 0x00, 0x01, 'p'}));
	// Too small a buffer: 0, nothing promised.
	CHECK(encode_connect(out, 20, o) == 0);
	// Minimal: clean session only; a password without a user name is not sent.
	hc::MqttConnectOptions m;
	m.client_id = "c";
	m.password = "p";
	m.keepalive_s = 10;
	n = encode_connect(out, sizeof(out), m);
	CHECK(take(out, n) == bytes({0x10, 0x0D, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, 0x00, 0x0A, 0x00, 0x01, 'c'}));
}

TEST_CASE("MqttCodecPublishSubscribeAndSmallPackets") {
	uint8_t out[300];
	const uint8_t hi[] = {'h', 'i'};
	size_t n = encode_publish(out, sizeof(out), "a/b", hi, 2, 1, true, 10);
	CHECK(take(out, n) == bytes({0x33, 0x09, 0x00, 0x03, 'a', '/', 'b', 0x00, 0x0A, 'h', 'i'}));
	n = encode_publish(out, sizeof(out), "a/b", hi, 2, 0, false, 0);
	CHECK(take(out, n) == bytes({0x30, 0x07, 0x00, 0x03, 'a', '/', 'b', 'h', 'i'}));
	CHECK(encode_publish(out, sizeof(out), "a/b", hi, 2, 1, false, 0) == 0); // QoS 1 needs an id
	CHECK(encode_publish(out, sizeof(out), "a/b", hi, 2, 2, false, 1) == 0); // no QoS 2
	CHECK(publish_size("a/b", 2, 1) == 11);
	// An empty retained payload (a removal).
	n = encode_publish(out, sizeof(out), "t", nullptr, 0, 1, true, 3);
	CHECK(take(out, n) == bytes({0x33, 0x05, 0x00, 0x01, 't', 0x00, 0x03}));
	// Two length bytes: 203 = 0xCB 0x01.
	std::vector<uint8_t> big(200, 'z');
	n = encode_publish(out, sizeof(out), "t", big.data(), big.size(), 0, false, 0);
	REQUIRE(n == 206);
	CHECK(out[0] == 0x30);
	CHECK(out[1] == 0xCB);
	CHECK(out[2] == 0x01);
	n = encode_subscribe(out, sizeof(out), 1, "a/+", 1);
	CHECK(take(out, n) == bytes({0x82, 0x08, 0x00, 0x01, 0x00, 0x03, 'a', '/', '+', 0x01}));
	n = encode_unsubscribe(out, sizeof(out), 2, "a/+");
	CHECK(take(out, n) == bytes({0xA2, 0x07, 0x00, 0x02, 0x00, 0x03, 'a', '/', '+'}));
	n = encode_puback(out, sizeof(out), 0x1234);
	CHECK(take(out, n) == bytes({0x40, 0x02, 0x12, 0x34}));
	n = encode_pingreq(out, sizeof(out));
	CHECK(take(out, n) == bytes({0xC0, 0x00}));
	n = encode_disconnect(out, sizeof(out));
	CHECK(take(out, n) == bytes({0xE0, 0x00}));
}

TEST_CASE("MqttCodecHeaders") {
	Header h;
	auto a = bytes({0x30, 0xC1, 0x02});
	REQUIRE(decode_header(a.data(), a.size(), h) == DecodeStatus::ok);
	CHECK(h.type == PacketType::publish);
	CHECK(h.remaining_length == 321);
	CHECK(h.header_length == 3);
	CHECK(decode_header(a.data(), 2, h) == DecodeStatus::need_more);
	CHECK(decode_header(a.data(), 1, h) == DecodeStatus::need_more);
	auto max = bytes({0x30, 0xFF, 0xFF, 0xFF, 0x7F});
	REQUIRE(decode_header(max.data(), max.size(), h) == DecodeStatus::ok);
	CHECK(h.remaining_length == kMaxRemainingLength);
	auto five = bytes({0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x01});
	CHECK(decode_header(five.data(), five.size(), h) == DecodeStatus::invalid);
	auto reserved = bytes({0xF0, 0x00});
	CHECK(decode_header(reserved.data(), reserved.size(), h) == DecodeStatus::invalid);
}

TEST_CASE("MqttCodecReceivedPackets") {
	bool present = true;
	uint8_t code = 9;
	auto connack = bytes({0x00, 0x05});
	REQUIRE(decode_connack(connack.data(), connack.size(), present, code));
	CHECK_FALSE(present);
	CHECK(code == 5);
	CHECK(std::string(connack_reason(code)) == "not authorized");
	auto bad = bytes({0x02, 0x00});
	CHECK_FALSE(decode_connack(bad.data(), bad.size(), present, code));

	// A retained QoS 1 PUBLISH from the broker.
	auto pub = bytes({0x33, 0x0A, 0x00, 0x03, 'a', '/', 'b', 0x00, 0x07, 'x', 'y', 'z'});
	Header h;
	REQUIRE(decode_header(pub.data(), pub.size(), h) == DecodeStatus::ok);
	Publish p;
	REQUIRE(decode_publish(h, pub.data() + h.header_length, h.remaining_length, p));
	CHECK(std::string(p.topic, p.topic_length) == "a/b");
	CHECK(std::string(reinterpret_cast<const char*>(p.payload), p.payload_length) == "xyz");
	CHECK(p.packet_id == 7);
	CHECK(p.qos == 1);
	CHECK(p.retain);
	// A topic longer than the body is malformed.
	auto broken = bytes({0x00, 0x09, 'a'});
	h.flags = 0;
	CHECK_FALSE(decode_publish(h, broken.data(), broken.size(), p));

	uint16_t id = 0;
	uint8_t granted = 0;
	auto suback = bytes({0x00, 0x01, 0x80});
	REQUIRE(decode_suback(suback.data(), suback.size(), id, granted));
	CHECK(id == 1);
	CHECK(granted == 0x80);
	auto puback = bytes({0x12, 0x34});
	REQUIRE(decode_packet_id(puback.data(), puback.size(), id));
	CHECK(id == 0x1234);
}

TEST_CASE("MqttCodecBrokerUrls") {
	Broker b;
	REQUIRE(parse_broker("mqtt://broker.example", b));
	CHECK_FALSE(b.tls);
	CHECK(std::string(b.host) == "broker.example");
	CHECK(b.port == 1883);
	REQUIRE(parse_broker("mqtts://192.0.2.5:8884/", b));
	CHECK(b.tls);
	CHECK(std::string(b.host) == "192.0.2.5");
	CHECK(b.port == 8884);
	REQUIRE(parse_broker("mqtts://h", b));
	CHECK(b.port == 8883);
	const char* error = nullptr;
	CHECK_FALSE(parse_broker("http://h", b, &error));
	CHECK_FALSE(parse_broker("mqtt://user:pw@h", b, &error));
	CHECK_FALSE(parse_broker("mqtt://h:70000", b, &error));
	CHECK_FALSE(parse_broker("mqtt://h:", b, &error));
	CHECK_FALSE(parse_broker("mqtt://-h", b, &error));
	CHECK_FALSE(parse_broker("mqtt://h/x", b, &error));
	CHECK(std::string(error).find("no path") != std::string::npos);
}

TEST_CASE("MqttCodecTopics") {
	CHECK(topic_valid("a/b c"));
	CHECK_FALSE(topic_valid(""));
	CHECK_FALSE(topic_valid("a/+"));
	CHECK_FALSE(topic_valid("a/#"));
	CHECK_FALSE(topic_valid("a\nb"));
	CHECK(topic_matches("a/+/c", "a/b/c"));
	CHECK(topic_matches("a/+/c", "a//c"));
	CHECK_FALSE(topic_matches("a/+/c", "a/b/d"));
	CHECK_FALSE(topic_matches("a/+", "a/b/c"));
	CHECK(topic_matches("a/#", "a/b/c"));
	CHECK(topic_matches("a/#", "a"));
	CHECK(topic_matches("#", "x/y"));
	CHECK(topic_matches("ha/+/node/+/config", "ha/light/node/f1/config"));
	CHECK_FALSE(topic_matches("ha/+/node/+/config", "ha/light/other/f1/config"));
	CHECK_FALSE(topic_matches("a/b", "a/bc"));
	CHECK_FALSE(topic_matches("a/bc", "a/b"));
}
