/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/mqtt_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>

#include "hc/log.h"
#include "hc/mqtt_codec.h"

namespace hc {
namespace posix {

namespace {

constexpr const char* kTag = "hc.mqtt";

int64_t now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

} // namespace

/// What the reader thread and the loop share about the client; touched on the loop thread only
/// (inside executor jobs and the client's methods), except `executor`, which never changes.
struct PosixMqttClient::Shared {
	LoopExecutor* executor = nullptr;
	MqttClientListener* listener = nullptr;
	uint32_t generation = 0;
	bool alive = true;
};

/// One connection.
struct PosixMqttClient::Session {
	uint32_t generation = 0;
	mqtt::Broker broker;
	std::string client_id, username, password, will_topic, will_payload;
	bool will_retain = false;
	uint8_t will_qos = 0;
	uint16_t keepalive_s = 60;

	std::mutex mutex;   ///< Guards fd, closed, connected and the socket's write side.
	int fd = -1;
	bool closed = false;    ///< Ended by the client: no callback.
	bool connected = false; ///< CONNACK 0 received.
	std::atomic<size_t> inflight{0};
	std::atomic<int64_t> last_send{0};
	std::atomic<bool> done{false};

	/// Sends all bytes; the caller holds `mutex`. False (and the socket shut down) on failure.
	bool send_locked(const uint8_t* data, size_t length, const char** error) {
		if (fd < 0 || closed) {
			*error = "not connected";
			return false;
		}
		size_t sent = 0;
		while (sent < length) {
			ssize_t n = ::send(fd, data + sent, length - sent, MSG_NOSIGNAL);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				*error = errno == EAGAIN || errno == EWOULDBLOCK ? "send timed out" : "send failed";
				::shutdown(fd, SHUT_RDWR);
				return false;
			}
			sent += static_cast<size_t>(n);
		}
		last_send.store(now_ms());
		return true;
	}
};

PosixMqttClient::PosixMqttClient(LoopExecutor& executor) : executor_(executor), shared_(std::make_shared<Shared>()) {
	shared_->executor = &executor;
}

PosixMqttClient::~PosixMqttClient() {
	shared_->alive = false;
	end_session(false);
	reap(true);
}

void PosixMqttClient::reap(bool all) {
	for (size_t i = 0; i < workers_.size();) {
		Worker& w = workers_[i];
		if (!all && !w.session->done.load()) {
			++i;
			continue;
		}
		// A reader may wait for the loop (this thread) to run its last event: run the jobs while
		// waiting (they are dropped: the session is over or the client is going away).
		while (!w.session->done.load()) {
			executor_.wait(10);
		}
		w.thread.join();
		workers_.erase(workers_.begin() + static_cast<long>(i));
	}
}

void PosixMqttClient::end_session(bool send_disconnect) {
	if (!session_) {
		return;
	}
	std::shared_ptr<Session> s = session_;
	session_.reset();
	++shared_->generation;
	std::lock_guard<std::mutex> lock(s->mutex);
	if (send_disconnect && s->connected && s->fd >= 0) {
		uint8_t packet[2];
		size_t n = mqtt::encode_disconnect(packet, sizeof(packet));
		const char* error = nullptr;
		s->send_locked(packet, n, &error);
	}
	s->closed = true;
	s->connected = false;
	if (s->fd >= 0) {
		::shutdown(s->fd, SHUT_RDWR);
	}
}

bool PosixMqttClient::connect(const MqttConnectOptions& options, MqttClientListener& listener, const char** error) {
	end_session(false);
	reap(false);
	auto s = std::make_shared<Session>();
	const char* reason = nullptr;
	if (!mqtt::parse_broker(options.broker, s->broker, &reason)) {
		*error = reason;
		return false;
	}
	if (s->broker.tls) {
		*error = "mqtts:// needs TLS, which the POSIX MQTT client does not have (use mqtt://)";
		return false;
	}
	s->client_id = options.client_id != nullptr ? options.client_id : "";
	s->username = options.username != nullptr ? options.username : "";
	s->password = options.password != nullptr ? options.password : "";
	s->will_topic = options.will_topic != nullptr ? options.will_topic : "";
	s->will_payload = options.will_payload != nullptr ? options.will_payload : "";
	s->will_retain = options.will_retain;
	s->will_qos = options.will_qos;
	s->keepalive_s = options.keepalive_s;
	s->generation = ++shared_->generation;
	shared_->listener = &listener;
	session_ = s;
	workers_.push_back(Worker{std::thread(&PosixMqttClient::reader, s, shared_), s});
	return true;
}

void PosixMqttClient::disconnect() { end_session(true); }

bool PosixMqttClient::send_packet(const uint8_t* data, size_t length, const char** error) {
	if (!session_) {
		*error = "not connected";
		return false;
	}
	std::lock_guard<std::mutex> lock(session_->mutex);
	if (!session_->connected) {
		*error = "not connected";
		return false;
	}
	return session_->send_locked(data, length, error);
}

bool PosixMqttClient::publish(const char* topic, const char* payload, size_t length, bool retain, uint8_t qos,
	const char** error) {
	if (!session_ || !session_->connected) {
		*error = "not connected";
		return false;
	}
	if (qos > 0 && session_->inflight.load() >= kMaxInflight) {
		*error = "too many messages waiting for their acknowledgement";
		return false;
	}
	size_t size = mqtt::publish_size(topic, length, qos);
	if (size == 0) {
		*error = "the message cannot be encoded";
		return false;
	}
	std::vector<uint8_t> packet(size);
	uint16_t id = 0;
	if (qos > 0) {
		id = next_packet_id_++;
		if (next_packet_id_ == 0) {
			next_packet_id_ = 1;
		}
	}
	size_t n = mqtt::encode_publish(packet.data(), packet.size(), topic, reinterpret_cast<const uint8_t*>(payload), length,
		qos, retain, id);
	if (n == 0) {
		*error = "the message cannot be encoded";
		return false;
	}
	if (qos > 0) {
		session_->inflight.fetch_add(1);
	}
	if (!send_packet(packet.data(), n, error)) {
		if (qos > 0) {
			session_->inflight.fetch_sub(1);
		}
		return false;
	}
	return true;
}

bool PosixMqttClient::subscribe(const char* filter, uint8_t qos, const char** error) {
	uint8_t packet[MqttClient::kMaxTopic + 16];
	uint16_t id = next_packet_id_++;
	if (next_packet_id_ == 0) {
		next_packet_id_ = 1;
	}
	size_t n = mqtt::encode_subscribe(packet, sizeof(packet), id, filter, qos);
	if (n == 0) {
		*error = "the subscription cannot be encoded";
		return false;
	}
	return send_packet(packet, n, error);
}

bool PosixMqttClient::unsubscribe(const char* filter, const char** error) {
	uint8_t packet[MqttClient::kMaxTopic + 16];
	uint16_t id = next_packet_id_++;
	if (next_packet_id_ == 0) {
		next_packet_id_ = 1;
	}
	size_t n = mqtt::encode_unsubscribe(packet, sizeof(packet), id, filter);
	if (n == 0) {
		*error = "the request cannot be encoded";
		return false;
	}
	return send_packet(packet, n, error);
}

size_t PosixMqttClient::pending() const { return session_ ? session_->inflight.load() : 0; }

// ---- The reader thread -------------------------------------------------------------------------

namespace {

/// Runs `fn` on the loop if the session is still the client's current one.
void deliver(const std::shared_ptr<PosixMqttClient::Shared>& shared, uint32_t generation,
	const std::function<void(MqttClientListener&)>& fn) {
	shared->executor->run([&shared, generation, &fn] {
		if (shared->alive && shared->generation == generation && shared->listener != nullptr) {
			fn(*shared->listener);
		}
	});
}

/// Opens a TCP connection to host:port within kConnectTimeoutS; -1 with `error` filled.
int open_socket(const mqtt::Broker& broker, std::string& error) {
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* list = nullptr;
	char port[8];
	std::snprintf(port, sizeof(port), "%u", static_cast<unsigned>(broker.port));
	int rc = ::getaddrinfo(broker.host, port, &hints, &list);
	if (rc != 0) {
		error = std::string("cannot resolve ") + broker.host + ": " + gai_strerror(rc);
		return -1;
	}
	int fd = -1;
	error = "no address";
	for (addrinfo* a = list; a != nullptr; a = a->ai_next) {
		fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
		if (fd < 0) {
			error = std::string("socket: ") + std::strerror(errno);
			continue;
		}
		int flags = ::fcntl(fd, F_GETFL, 0);
		::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		int r = ::connect(fd, a->ai_addr, a->ai_addrlen);
		if (r != 0 && errno == EINPROGRESS) {
			pollfd p{fd, POLLOUT, 0};
			r = ::poll(&p, 1, PosixMqttClient::kConnectTimeoutS * 1000);
			if (r == 1) {
				int so_error = 0;
				socklen_t len = sizeof(so_error);
				::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
				r = so_error == 0 ? 0 : -1;
				errno = so_error;
			} else {
				errno = r == 0 ? ETIMEDOUT : errno;
				r = -1;
			}
		}
		if (r == 0) {
			::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
			timeval tv{PosixMqttClient::kSendTimeoutS, 0};
			::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
			int one = 1;
			::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
			break;
		}
		error = std::string("cannot connect to ") + broker.host + ":" + port + ": " + std::strerror(errno);
		::close(fd);
		fd = -1;
	}
	::freeaddrinfo(list);
	return fd;
}

} // namespace

void PosixMqttClient::reader(std::shared_ptr<Session> s, std::shared_ptr<Shared> shared) {
	uint32_t generation = s->generation;
	std::string reason;
	auto fail = [&](const std::string& why) {
		std::string text = why;
		deliver(shared, generation, [&text](MqttClientListener& l) { l.mqtt_disconnected(text.c_str()); });
	};

	int fd = open_socket(s->broker, reason);
	{
		std::lock_guard<std::mutex> lock(s->mutex);
		if (s->closed) {
			if (fd >= 0) {
				::close(fd);
			}
			s->done.store(true);
			return;
		}
		s->fd = fd;
	}
	if (fd < 0) {
		fail(reason);
		s->done.store(true);
		return;
	}
	reason.clear();
	{
		MqttConnectOptions o;
		o.client_id = s->client_id.c_str();
		o.username = s->username.c_str();
		o.password = s->password.c_str();
		o.will_topic = s->will_topic.c_str();
		o.will_payload = s->will_payload.c_str();
		o.will_retain = s->will_retain;
		o.will_qos = s->will_qos;
		o.keepalive_s = s->keepalive_s;
		std::vector<uint8_t> packet(1024 + s->client_id.size() + s->username.size() + s->password.size() +
			s->will_topic.size() + s->will_payload.size());
		size_t n = mqtt::encode_connect(packet.data(), packet.size(), o);
		std::fill(s->password.begin(), s->password.end(), '\0');
		const char* error = "the CONNECT cannot be encoded";
		bool ok = n > 0;
		if (ok) {
			std::lock_guard<std::mutex> lock(s->mutex);
			ok = s->send_locked(packet.data(), n, &error);
		}
		std::fill(packet.begin(), packet.end(), 0);
		if (!ok) {
			reason = error;
		}
	}

	std::vector<uint8_t> rx;
	uint8_t chunk[4096];
	bool connected = false;
	int64_t start = now_ms();
	int64_t ping_sent = 0;
	const int64_t keepalive_ms = static_cast<int64_t>(s->keepalive_s) * 1000;
	while (reason.empty()) {
		int64_t now = now_ms();
		if (!connected && now - start > kConnectTimeoutS * 1000) {
			reason = "no CONNACK from the broker";
			break;
		}
		if (connected && keepalive_ms > 0) {
			if (ping_sent != 0 && now - ping_sent > keepalive_ms) {
				reason = "keepalive timeout: the broker did not answer";
				break;
			}
			if (ping_sent == 0 && now - s->last_send.load() >= keepalive_ms / 2) {
				uint8_t packet[2];
				size_t n = mqtt::encode_pingreq(packet, sizeof(packet));
				const char* error = nullptr;
				std::lock_guard<std::mutex> lock(s->mutex);
				if (!s->send_locked(packet, n, &error)) {
					reason = error;
					break;
				}
				ping_sent = now;
			}
		}
		pollfd p{fd, POLLIN, 0};
		int r = ::poll(&p, 1, 100);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r < 0) {
			reason = std::string("poll: ") + std::strerror(errno);
			break;
		}
		if (r == 0) {
			continue;
		}
		ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			reason = got == 0 ? "connection closed by the broker" : std::string("recv: ") + std::strerror(errno);
			break;
		}
		rx.insert(rx.end(), chunk, chunk + got);
		// Every complete packet in the buffer.
		size_t used = 0;
		while (reason.empty()) {
			mqtt::Header h;
			mqtt::DecodeStatus status = mqtt::decode_header(rx.data() + used, rx.size() - used, h);
			if (status == mqtt::DecodeStatus::need_more) {
				break;
			}
			if (status == mqtt::DecodeStatus::invalid || h.remaining_length > kMaxInbound) {
				reason = status == mqtt::DecodeStatus::invalid ? "malformed packet from the broker"
															  : "packet larger than 256 KB from the broker";
				break;
			}
			if (rx.size() - used < h.header_length + h.remaining_length) {
				break;
			}
			const uint8_t* body = rx.data() + used + h.header_length;
			size_t length = h.remaining_length;
			used += h.header_length + h.remaining_length;
			switch (h.type) {
				case mqtt::PacketType::connack: {
					bool present = false;
					uint8_t code = 0;
					if (connected || !mqtt::decode_connack(body, length, present, code)) {
						reason = "unexpected CONNACK";
						break;
					}
					if (code != 0) {
						reason = std::string("connection refused: ") + mqtt::connack_reason(code);
						break;
					}
					connected = true;
					{
						std::lock_guard<std::mutex> lock(s->mutex);
						s->connected = !s->closed;
					}
					deliver(shared, generation, [](MqttClientListener& l) { l.mqtt_connected(); });
					break;
				}
				case mqtt::PacketType::publish: {
					mqtt::Publish pub;
					if (!connected || !mqtt::decode_publish(h, body, length, pub) || pub.qos > 1) {
						reason = "malformed PUBLISH from the broker";
						break;
					}
					if (pub.qos == 1) {
						uint8_t ack[4];
						size_t n = mqtt::encode_puback(ack, sizeof(ack), pub.packet_id);
						const char* error = nullptr;
						std::lock_guard<std::mutex> lock(s->mutex);
						if (!s->send_locked(ack, n, &error)) {
							reason = error;
							break;
						}
					}
					std::string topic(pub.topic, pub.topic_length);
					MqttMessage m{topic.c_str(), reinterpret_cast<const char*>(pub.payload), pub.payload_length, pub.retain,
						false};
					deliver(shared, generation, [&m](MqttClientListener& l) { l.mqtt_message(m); });
					break;
				}
				case mqtt::PacketType::puback: {
					uint16_t id = 0;
					if (mqtt::decode_packet_id(body, length, id) && s->inflight.load() > 0) {
						s->inflight.fetch_sub(1);
					}
					break;
				}
				case mqtt::PacketType::suback: {
					uint16_t id = 0;
					uint8_t granted = 0;
					if (!mqtt::decode_suback(body, length, id, granted)) {
						reason = "malformed SUBACK";
					} else if (granted == 0x80) {
						Log::warn(kTag, "the broker refused a subscription (packet %u)", static_cast<unsigned>(id));
					}
					break;
				}
				case mqtt::PacketType::unsuback:
					break;
				case mqtt::PacketType::pingresp:
					ping_sent = 0;
					break;
				default:
					reason = "unexpected packet type from the broker";
					break;
			}
		}
		rx.erase(rx.begin(), rx.begin() + static_cast<long>(used));
	}

	bool ours;
	{
		std::lock_guard<std::mutex> lock(s->mutex);
		ours = s->closed;
		s->connected = false;
		::close(s->fd);
		s->fd = -1;
	}
	if (!ours) {
		fail(reason);
	}
	s->done.store(true);
}

} // namespace posix
} // namespace hc
