/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_MQTT_TEST_SUPPORT_H
#define HC_MQTT_TEST_SUPPORT_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/mqtt_codec.h"
#include "hc/platform/mqtt_client.h"

namespace hc_test {

/// A scripted MqttClient: records what the core asks for; the test answers (accept, drop, deliver).
/// Keeps a broker-like retained store so a sweep test can deliver what a subscription would bring.
class FakeMqttClient : public hc::MqttClient {
public:
	struct Publication {
		std::string topic;
		std::string payload;
		bool retain;
		uint8_t qos;
	};

	bool connect(const hc::MqttConnectOptions& options, hc::MqttClientListener& listener, const char** error) override {
		if (refuse_connect != nullptr) {
			*error = refuse_connect;
			return false;
		}
		++connects;
		broker = options.broker;
		client_id = options.client_id;
		username = options.username != nullptr ? options.username : "";
		password = options.password != nullptr ? options.password : "";
		will_topic = options.will_topic != nullptr ? options.will_topic : "";
		will_payload = options.will_payload != nullptr ? options.will_payload : "";
		will_retain = options.will_retain;
		will_qos = options.will_qos;
		listener_ = &listener;
		session = true;
		connected = false;
		subscriptions.clear();
		return true;
	}

	void disconnect() override {
		++disconnects;
		session = false;
		connected = false;
		queued = 0;
		queued_bytes = 0;
		subscriptions.clear();
	}

	bool publish(const char* topic, const char* payload, size_t length, bool retain, uint8_t qos,
		const char** error) override {
		if (!connected) {
			*error = "not connected";
			return false;
		}
		if (queued >= capacity || (byte_capacity != SIZE_MAX && queued_bytes + length > byte_capacity)) {
			*error = "outbox full";
			++refused;
			return false;
		}
		REQUIRE(hc::mqtt::topic_valid(topic));
		++queued;
		queued_bytes += length;
		published.push_back(Publication{topic, std::string(payload, length), retain, qos});
		if (retain) {
			if (length == 0) {
				retained.erase(topic);
			} else {
				retained[topic] = std::string(payload, length);
			}
		}
		return true;
	}

	bool subscribe(const char* filter, uint8_t qos, const char** error) override {
		if (!connected) {
			*error = "not connected";
			return false;
		}
		REQUIRE(qos <= 1);
		subscriptions.push_back(filter);
		subscribe_log.push_back(filter);
		return true;
	}

	bool unsubscribe(const char* filter, const char** error) override {
		if (!connected) {
			*error = "not connected";
			return false;
		}
		for (size_t i = 0; i < subscriptions.size(); ++i) {
			if (subscriptions[i] == filter) {
				subscriptions.erase(subscriptions.begin() + static_cast<long>(i));
				break;
			}
		}
		unsubscribe_log.push_back(filter);
		return true;
	}

	size_t pending() const override { return queued; }

	// The test's side.

	void accept() {
		REQUIRE(session);
		connected = true;
		listener_->mqtt_connected();
	}

	void drop(const char* reason) {
		REQUIRE(session);
		session = false;
		connected = false;
		queued = 0;
		subscriptions.clear();
		listener_->mqtt_disconnected(reason);
	}

	void deliver(const std::string& topic, const std::string& payload, bool retained_flag = false,
		bool truncated = false) {
		hc::MqttMessage message{topic.c_str(), payload.data(), payload.size(), retained_flag, truncated};
		listener_->mqtt_message(message);
	}

	/// Delivers the retained messages matching the current subscriptions (what a broker sends on
	/// SUBSCRIBE), with the retained flag.
	void deliver_retained() {
		std::vector<std::pair<std::string, std::string>> due;
		for (const auto& entry : retained) {
			for (const std::string& filter : subscriptions) {
				if (hc::mqtt::topic_matches(filter.c_str(), entry.first.c_str())) {
					due.push_back(entry);
					break;
				}
			}
		}
		for (const auto& entry : due) {
			deliver(entry.first, entry.second, true);
		}
	}

	bool subscribed(const std::string& filter) const {
		for (const std::string& s : subscriptions) {
			if (s == filter) {
				return true;
			}
		}
		return false;
	}

	/// Publications to `topic` (in order).
	std::vector<Publication> to(const std::string& topic) const {
		std::vector<Publication> result;
		for (const Publication& p : published) {
			if (p.topic == topic) {
				result.push_back(p);
			}
		}
		return result;
	}

	/// Publications whose topic starts with `prefix`.
	std::vector<Publication> under(const std::string& prefix) const {
		std::vector<Publication> result;
		for (const Publication& p : published) {
			if (p.topic.compare(0, prefix.size(), prefix) == 0) {
				result.push_back(p);
			}
		}
		return result;
	}

	const char* refuse_connect = nullptr;
	size_t capacity = SIZE_MAX;
	size_t queued = 0;
	/// A limit on the payload bytes queued, like esp-mqtt's outbox limit: a publication that would
	/// exceed it is refused.
	size_t byte_capacity = SIZE_MAX;
	size_t queued_bytes = 0;
	/// Publications refused because the queue was full.
	size_t refused = 0;
	bool session = false;
	bool connected = false;
	int connects = 0;
	int disconnects = 0;
	std::string broker, client_id, username, password, will_topic, will_payload;
	bool will_retain = false;
	uint8_t will_qos = 0;
	std::vector<Publication> published;
	std::vector<std::string> subscriptions;
	std::vector<std::string> subscribe_log;
	std::vector<std::string> unsubscribe_log;
	std::map<std::string, std::string> retained;

private:
	hc::MqttClientListener* listener_ = nullptr;
};

} // namespace hc_test

#endif // HC_MQTT_TEST_SUPPORT_H
