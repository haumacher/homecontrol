/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_MQTT_CLIENT_H
#define HC_PLATFORM_MQTT_CLIENT_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// What MqttClient::connect() needs. The client copies everything it keeps; the pointers need to
/// live only for the call.
struct MqttConnectOptions {
	/// `mqtt://host[:port]` (default port 1883) or `mqtts://host[:port]` (8883, TLS with the
	/// platform's certificate store). Checked by the core (mqtt_broker_valid()).
	const char* broker = nullptr;
	const char* client_id = nullptr;
	/// nullptr or "" for none. A password without a user name is not sent (MQTT 3.1.1).
	const char* username = nullptr;
	const char* password = nullptr;
	/// The last will: published by the broker when the connection is lost without DISCONNECT.
	const char* will_topic = nullptr;
	const char* will_payload = nullptr;
	bool will_retain = false;
	uint8_t will_qos = 0;
	/// Seconds; the client pings when idle and drops the connection when the broker stays silent.
	uint16_t keepalive_s = 60;
};

/// One message the broker delivered.
struct MqttMessage {
	const char* topic;   ///< NUL-terminated.
	const char* payload; ///< Not NUL-terminated; valid during the call only.
	size_t length;       ///< Bytes in `payload`.
	/// Delivered because of a new subscription (a retained message), not forwarded live.
	bool retained;
	/// The platform cut the payload at its inbound buffer (`length` is what it kept).
	bool truncated;
};

/// The events of a session, always on the event loop's thread, never from within a MqttClient call.
class MqttClientListener {
public:
	/// The broker accepted the connection (CONNACK 0).
	virtual void mqtt_connected() = 0;
	/// The connection attempt failed or an established connection was lost; `reason` says why
	/// ("connection refused: not authorized", "keepalive timeout", "TLS handshake failed"). After
	/// it the client is idle until the next connect().
	virtual void mqtt_disconnected(const char* reason) = 0;
	/// A message on a subscribed topic.
	virtual void mqtt_message(const MqttMessage& message) = 0;

protected:
	~MqttClientListener() = default;
};

/// An MQTT 3.1.1 client of the platform (doc/mqtt.md): esp-mqtt on the ESP, a small own client
/// over a TCP socket on POSIX, a scripted fake in the tests. The core decides when to connect and
/// reconnect (the client never reconnects by itself), what to publish and subscribe; the platform
/// moves the bytes off the event loop and reports back on the loop's thread.
///
/// A session: connect() starts it; exactly one mqtt_connected() or mqtt_disconnected() follows;
/// after mqtt_connected() a mqtt_disconnected() follows when the connection ends, unless the core
/// ended it with disconnect(). Calls of a session that ended deliver nothing. Session state is
/// clean (clean session 1): subscriptions are made again after every connect.
///
/// Outgoing messages go through a bounded queue (esp-mqtt's outbox, the POSIX client's in-flight
/// window): publish() refuses when it is full, and the core tries again later (backpressure, never
/// loss of the latest state: the core keeps what is unpublished).
///
/// Every call is made on the event loop's thread.
class MqttClient {
public:
	/// Longest topic in bytes without the NUL.
	static constexpr size_t kMaxTopic = 255;

	/// Starts a session (an existing one is ended first, without a callback). False with `error`
	/// (static text) when it cannot start: the scheme is not supported by the platform (POSIX has
	/// no TLS), no memory, a URL the platform refuses.
	virtual bool connect(const MqttConnectOptions& options, MqttClientListener& listener, const char** error) = 0;

	/// Ends the session with DISCONNECT (the broker then does not publish the last will) and drops
	/// what is queued. No callback follows.
	virtual void disconnect() = 0;

	/// Queues a message. False with `error` when not connected or when the outgoing queue is full
	/// (try again later). QoS 0 or 1.
	virtual bool publish(const char* topic, const char* payload, size_t length, bool retain, uint8_t qos,
		const char** error) = 0;

	/// Subscribes a topic filter with QoS 0 or 1 (wildcards `+` and `#` allowed). False with `error`
	/// when not connected or the request cannot be queued. A broker's refusal is logged by the
	/// platform.
	virtual bool subscribe(const char* filter, uint8_t qos, const char** error) = 0;

	/// Ends a subscription.
	virtual bool unsubscribe(const char* filter, const char** error) = 0;

	/// How much of the outgoing queue is taken, in the platform's unit (POSIX: QoS 1 messages awaiting
	/// their PUBACK; ESP: esp-mqtt's outbox in bytes). 0 means everything is sent (and acknowledged):
	/// the core waits for it before an orderly disconnect.
	virtual size_t pending() const = 0;

protected:
	~MqttClient() = default;
};

} // namespace hc

#endif // HC_PLATFORM_MQTT_CLIENT_H
