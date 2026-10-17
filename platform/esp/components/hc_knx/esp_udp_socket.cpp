/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_udp_socket.h"

#include <sys/select.h>
#include <sys/socket.h>

#include <cstdio>
#include <cstring>

#include "esp_log.h"

#include "hc/common/bsd_udp.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.udp";

/// Below the event loop (5): the reader only copies datagrams.
constexpr UBaseType_t kPriority = 4;

/// How long the reader waits on the socket before it looks for retired descriptors and requests.
constexpr int kPollMs = 100;

} // namespace

EspUdpSocket::EspUdpSocket(LoopQueue& queue) : queue_(queue) {
	for (std::atomic<int>& fd : retire_) {
		fd.store(-1);
	}
}

bool EspUdpSocket::init() {
	mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
	task_ = xTaskCreateStatic(&task_main, "hc_udp", kStack, this, kPriority, stack_, &task_storage_);
	if (mutex_ == nullptr || task_ == nullptr) {
		ESP_LOGE(kTag, "UDP reader task not created: the KNX tunnel cannot receive");
		task_ = nullptr;
		return false;
	}
	return true;
}

bool EspUdpSocket::open(UdpReceiver& receiver, const char** error) {
	close();
	if (task_ == nullptr) {
		*error = "no UDP reader task";
		return false;
	}
	fd_ = bsd::udp_open(error_, sizeof(error_));
	if (fd_ < 0) {
		*error = error_;
		return false;
	}
	receiver_ = &receiver;
	active_fd_.store(fd_);
	xTaskNotifyGive(task_);
	return true;
}

void EspUdpSocket::close() {
	if (fd_ < 0) {
		return;
	}
	int fd = fd_;
	fd_ = -1;
	active_fd_.store(-1);
	generation_.fetch_add(1);
	// The reader may be waiting on the descriptor: it closes it itself on its next round.
	for (std::atomic<int>& slot : retire_) {
		int expected = -1;
		if (slot.compare_exchange_strong(expected, fd)) {
			xTaskNotifyGive(task_);
			return;
		}
	}
	ESP_LOGW(kTag, "retire queue full: closing socket %d directly", fd);
	bsd::udp_close(fd);
}

bool EspUdpSocket::send_to(const Ipv4Endpoint& to, const uint8_t* data, size_t length, const char** error) {
	if (fd_ < 0) {
		*error = "socket is closed";
		return false;
	}
	if (!bsd::udp_send_to(fd_, to, data, length, error_, sizeof(error_))) {
		*error = error_;
		return false;
	}
	return true;
}

bool EspUdpSocket::local_endpoint(const Ipv4Endpoint& remote, Ipv4Endpoint& local, const char** error) {
	if (fd_ < 0) {
		*error = "socket is closed";
		return false;
	}
	if (!bsd::udp_local_endpoint(fd_, remote, local, error_, sizeof(error_))) {
		*error = error_;
		return false;
	}
	return true;
}

bool EspUdpSocket::resolve(const char* host, UdpReceiver& receiver, const char** error) {
	if (task_ == nullptr) {
		*error = "no UDP reader task";
		return false;
	}
	if (std::strlen(host) >= kMaxHost) {
		*error = "host name too long";
		return false;
	}
	xSemaphoreTake(mutex_, portMAX_DELAY);
	std::snprintf(resolve_host_, sizeof(resolve_host_), "%s", host);
	xSemaphoreGive(mutex_);
	resolve_receiver_ = &receiver;
	resolve_request_.fetch_add(1);
	xTaskNotifyGive(task_);
	return true;
}

// ---- Reader task ---------------------------------------------------------------------------

void EspUdpSocket::task_main(void* self) {
	static_cast<EspUdpSocket*>(self)->run();
}

void EspUdpSocket::run() {
	for (;;) {
		retire_closed();
		if (resolve_request_.load() != resolve_served_) {
			do_resolve();
		}
		int fd = active_fd_.load();
		if (fd < 0) {
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kPollMs));
			continue;
		}
		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(fd, &readable);
		timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = kPollMs * 1000;
		int ready = select(fd + 1, &readable, nullptr, nullptr, &timeout);
		if (ready > 0 && FD_ISSET(fd, &readable)) {
			receive(fd);
		}
	}
}

void EspUdpSocket::retire_closed() {
	for (std::atomic<int>& slot : retire_) {
		int fd = slot.exchange(-1);
		if (fd >= 0) {
			bsd::udp_close(fd);
		}
	}
}

void EspUdpSocket::receive(int fd) {
	uint8_t buffer[kMaxDatagram + 1];
	char error[bsd::kMaxError];
	for (int i = 0; i < static_cast<int>(kRing); ++i) {
		Ipv4Endpoint from;
		int length = bsd::udp_receive(fd, buffer, sizeof(buffer), from, error, sizeof(error));
		if (length == 0) {
			return;
		}
		if (length < 0) {
			queue_.post(&handle_error, this, 0, error, "UDP receive error");
			return;
		}
		if (static_cast<size_t>(length) > kMaxDatagram) {
			ESP_LOGW(kTag, "dropping a datagram longer than %u octets", static_cast<unsigned>(kMaxDatagram));
			continue;
		}
		bool stored = false;
		xSemaphoreTake(mutex_, portMAX_DELAY);
		if (ring_count_ < kRing) {
			Slot& slot = ring_[(ring_head_ + ring_count_) % kRing];
			slot.generation = generation_.load();
			slot.from = from;
			slot.length = static_cast<uint16_t>(length);
			std::memcpy(slot.data, buffer, static_cast<size_t>(length));
			++ring_count_;
			stored = true;
		}
		xSemaphoreGive(mutex_);
		if (!stored) {
			uint32_t dropped = dropped_.fetch_add(1) + 1;
			ESP_LOGW(kTag, "datagram ring full (%u slots): dropped (%u since boot)", static_cast<unsigned>(kRing),
				static_cast<unsigned>(dropped));
			continue;
		}
		if (!drain_posted_.exchange(true)) {
			if (!queue_.post(&handle_datagrams, this, 0, nullptr, "UDP datagrams")) {
				drain_posted_.store(false); // posted again with the next datagram
			}
		}
	}
}

void EspUdpSocket::do_resolve() {
	uint32_t request = resolve_request_.load();
	char host[kMaxHost];
	xSemaphoreTake(mutex_, portMAX_DELAY);
	std::memcpy(host, resolve_host_, sizeof(host));
	xSemaphoreGive(mutex_);
	resolve_served_ = request;
	uint32_t address = 0;
	char error[bsd::kMaxError];
	bool ok = bsd::resolve_ipv4(host, address, error, sizeof(error));
	xSemaphoreTake(mutex_, portMAX_DELAY);
	resolve_answered_ = request;
	xSemaphoreGive(mutex_);
	// number carries the address (its bits), the text the reason; a failed lookup has number 0 and
	// a text (an address of 0.0.0.0 is no answer either).
	queue_.post(&handle_resolved, this, ok ? static_cast<int32_t>(address) : 0, ok ? "" : error, "DNS result");
}

// ---- Event loop ----------------------------------------------------------------------------

void EspUdpSocket::handle_datagrams(void* context, const LoopMessage&) {
	static_cast<EspUdpSocket*>(context)->drain();
}

void EspUdpSocket::drain() {
	drain_posted_.store(false);
	for (;;) {
		Slot slot;
		xSemaphoreTake(mutex_, portMAX_DELAY);
		if (ring_count_ == 0) {
			xSemaphoreGive(mutex_);
			return;
		}
		Slot& head = ring_[ring_head_];
		slot.generation = head.generation;
		slot.from = head.from;
		slot.length = head.length;
		std::memcpy(slot.data, head.data, head.length);
		ring_head_ = (ring_head_ + 1) % kRing;
		--ring_count_;
		xSemaphoreGive(mutex_);
		if (slot.generation != generation_.load() || fd_ < 0 || receiver_ == nullptr) {
			continue; // from a socket that is closed
		}
		receiver_->udp_received(slot.data, slot.length, slot.from);
	}
}

void EspUdpSocket::handle_error(void* context, const LoopMessage& message) {
	EspUdpSocket* self = static_cast<EspUdpSocket*>(context);
	if (self->fd_ >= 0 && self->receiver_ != nullptr) {
		self->receiver_->udp_error(message.text);
	}
}

void EspUdpSocket::handle_resolved(void* context, const LoopMessage& message) {
	EspUdpSocket* self = static_cast<EspUdpSocket*>(context);
	xSemaphoreTake(self->mutex_, portMAX_DELAY);
	uint32_t answered = self->resolve_answered_;
	xSemaphoreGive(self->mutex_);
	if (answered != self->resolve_request_.load() || self->resolve_receiver_ == nullptr) {
		return; // superseded by a newer request
	}
	UdpReceiver* receiver = self->resolve_receiver_;
	self->resolve_receiver_ = nullptr;
	bool ok = message.text[0] == '\0';
	receiver->udp_resolved(ok, static_cast<uint32_t>(message.number), message.text);
}

} // namespace esp
} // namespace hc
