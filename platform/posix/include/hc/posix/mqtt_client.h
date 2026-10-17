/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_MQTT_CLIENT_H
#define HC_POSIX_MQTT_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "hc/platform/mqtt_client.h"
#include "hc/posix/web_server.h"

namespace hc {
namespace posix {

/// The POSIX MqttClient (doc/mqtt.md): a small MQTT 3.1.1 client of its own over a TCP socket, on
/// the core's codec (hc/mqtt_codec.h): CONNECT with the last will, CONNACK, PUBLISH QoS 0 and 1
/// with PUBACK (both directions), SUBSCRIBE/SUBACK, UNSUBSCRIBE/UNSUBACK, PINGREQ/PINGRESP and
/// DISCONNECT. TCP only: `mqtts://` is refused with that reason (no TLS on the host build). Enough
/// to run the controller against a broker on the LAN (mosquitto) and for the SBC fallback.
///
/// A reader thread per session connects, reads and keeps the connection alive (PINGREQ after
/// half the keepalive of silence, the connection dropped when the broker stays silent for the
/// whole keepalive); it hands every event to the event loop through the LoopExecutor the web
/// server uses (blocking until the loop ran it, so the inbound side needs no queue). Writes
/// happen on the loop thread, under a mutex, with a send timeout of kSendTimeoutS. QoS 1
/// messages are not retransmitted after a reconnect (clean session): the core republishes the
/// state anyway.
class PosixMqttClient : public MqttClient {
public:
	static constexpr int kConnectTimeoutS = 10;
	static constexpr int kSendTimeoutS = 5;
	/// QoS 1 messages waiting for their PUBACK; publish() refuses beyond (backpressure).
	static constexpr size_t kMaxInflight = 64;
	/// Largest packet accepted from the broker; a larger one drops the connection.
	static constexpr size_t kMaxInbound = 262144;

	explicit PosixMqttClient(LoopExecutor& executor);
	/// Ends the session without DISCONNECT (the broker publishes the last will) and waits for the
	/// reader thread; call on the loop thread.
	~PosixMqttClient();

	PosixMqttClient(const PosixMqttClient&) = delete;
	PosixMqttClient& operator=(const PosixMqttClient&) = delete;

	bool connect(const MqttConnectOptions& options, MqttClientListener& listener, const char** error) override;
	void disconnect() override;
	bool publish(const char* topic, const char* payload, size_t length, bool retain, uint8_t qos,
		const char** error) override;
	bool subscribe(const char* filter, uint8_t qos, const char** error) override;
	bool unsubscribe(const char* filter, const char** error) override;
	size_t pending() const override;

	// Implementation: the state shared with the reader thread (mqtt_client.cpp).
	struct Session;
	struct Shared;

private:
	static void reader(std::shared_ptr<Session> session, std::shared_ptr<Shared> shared);
	void end_session(bool send_disconnect);
	void reap(bool all);
	bool send_packet(const uint8_t* data, size_t length, const char** error);

	LoopExecutor& executor_;
	std::shared_ptr<Shared> shared_;
	std::shared_ptr<Session> session_;
	struct Worker {
		std::thread thread;
		std::shared_ptr<Session> session;
	};
	std::vector<Worker> workers_;
	uint16_t next_packet_id_ = 1;
};

} // namespace posix
} // namespace hc

#endif // HC_POSIX_MQTT_CLIENT_H
