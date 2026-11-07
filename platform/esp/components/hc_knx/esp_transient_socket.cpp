/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_transient_socket.h"

#include <sys/select.h>
#include <sys/socket.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "esp_udp_socket.h"
#include "hc/common/bsd_udp.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.udp";

/// Below the event loop (5), as the tunnel's reader.
constexpr UBaseType_t kPriority = 4;

constexpr int kPollMs = 100;

} // namespace

EspTransientSocket::EspTransientSocket(LoopQueue& queue) : queue_(queue) {}

EspTransientSocket::~EspTransientSocket() {
	if (mutex_ != nullptr) {
		vSemaphoreDelete(mutex_);
	}
}

bool EspTransientSocket::start(const char** error) {
	mutex_ = xSemaphoreCreateMutex();
	if (mutex_ == nullptr) {
		*error = "no memory for the socket's mutex";
		return false;
	}
	if (xTaskCreate(&task_main, "hc_udp_tmp", kStack, this, kPriority, &task_) != pdPASS) {
		task_ = nullptr;
		*error = "no memory for the socket's reader task";
		return false;
	}
	return true;
}

bool EspTransientSocket::open(UdpReceiver& receiver, const char** error) {
	if (used_) {
		*error = "a transient socket opens once";
		return false;
	}
	fd_ = bsd::udp_open(error_, sizeof(error_));
	if (fd_ < 0) {
		*error = error_;
		return false;
	}
	used_ = true;
	receiver_ = &receiver;
	active_fd_.store(fd_);
	xTaskNotifyGive(task_);
	return true;
}

void EspTransientSocket::close() {
	// The descriptor stays with the reader, which may be waiting on it; it closes it when it ends.
	fd_ = -1;
	receiver_ = nullptr;
}

bool EspTransientSocket::send_to(const Ipv4Endpoint& to, const uint8_t* data, size_t length, const char** error) {
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

bool EspTransientSocket::local_endpoint(const Ipv4Endpoint& remote, Ipv4Endpoint& local, const char** error) {
	if (fd_ < 0) {
		*error = "socket is closed";
		return false;
	}
	return esp_local_endpoint(fd_, remote, local, error);
}

bool EspTransientSocket::resolve(const char*, UdpReceiver&, const char** error) {
	*error = "a transient socket resolves no names";
	return false;
}

void EspTransientSocket::stop() {
	close();
	stop_.store(true);
	xTaskNotifyGive(task_);
}

// ---- Reader task ---------------------------------------------------------------------------

void EspTransientSocket::task_main(void* self) {
	static_cast<EspTransientSocket*>(self)->run();
}

void EspTransientSocket::run() {
	while (!stop_.load()) {
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
		if (ready > 0 && FD_ISSET(fd, &readable) && !stop_.load()) {
			receive(fd);
		}
	}
	int fd = active_fd_.exchange(-1);
	if (fd >= 0) {
		bsd::udp_close(fd);
	}
	ESP_LOGD(kTag, "transient socket's reader ends: %u B of its %u B stack never used",
		static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)),
		static_cast<unsigned>(kStack));
	// The exit message frees the object on the loop, behind every message posted before; after the
	// post nothing of the object may be touched.
	LoopQueue& queue = queue_;
	while (!queue.post(&handle_exited, this, 0, nullptr, "UDP socket exit")) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	vTaskDelete(nullptr);
}

void EspTransientSocket::receive(int fd) {
	char error[bsd::kMaxError];
	for (size_t i = 0; i < kRing; ++i) {
		Slot* slot = nullptr;
		xSemaphoreTake(mutex_, portMAX_DELAY);
		if (ring_count_ < kRing) {
			slot = &ring_[(ring_head_ + ring_count_) % kRing];
		}
		xSemaphoreGive(mutex_);
		Ipv4Endpoint from;
		uint8_t discard[1];
		int length = slot != nullptr
			? bsd::udp_receive(fd, slot->data, sizeof(slot->data), from, error, sizeof(error))
			: bsd::udp_receive(fd, discard, sizeof(discard), from, error, sizeof(error));
		if (length <= 0) {
			return; // nothing more (a receive error ends this round; the search's window ends it all)
		}
		if (slot == nullptr) {
			ESP_LOGW(kTag, "transient socket: ring full (%u slots), datagram dropped", static_cast<unsigned>(kRing));
			continue;
		}
		if (static_cast<size_t>(length) > kMaxDatagram) {
			ESP_LOGW(kTag, "transient socket: datagram longer than %u octets dropped", static_cast<unsigned>(kMaxDatagram));
			continue;
		}
		xSemaphoreTake(mutex_, portMAX_DELAY);
		slot->from = from;
		slot->length = static_cast<uint16_t>(length);
		++ring_count_;
		xSemaphoreGive(mutex_);
		if (!drain_posted_.exchange(true)) {
			if (!queue_.post(&handle_datagrams, this, 0, nullptr, "UDP datagrams")) {
				drain_posted_.store(false);
			}
		}
	}
}

// ---- Event loop ----------------------------------------------------------------------------

void EspTransientSocket::handle_datagrams(void* context, const LoopMessage&) {
	static_cast<EspTransientSocket*>(context)->drain();
}

void EspTransientSocket::drain() {
	drain_posted_.store(false);
	for (;;) {
		Slot slot;
		xSemaphoreTake(mutex_, portMAX_DELAY);
		if (ring_count_ == 0) {
			xSemaphoreGive(mutex_);
			return;
		}
		Slot& head = ring_[ring_head_];
		slot.from = head.from;
		slot.length = head.length;
		std::memcpy(slot.data, head.data, head.length);
		ring_head_ = (ring_head_ + 1) % kRing;
		--ring_count_;
		xSemaphoreGive(mutex_);
		if (fd_ >= 0 && receiver_ != nullptr) {
			receiver_->udp_received(slot.data, slot.length, slot.from);
		}
	}
}

void EspTransientSocket::handle_exited(void* context, const LoopMessage&) {
	delete static_cast<EspTransientSocket*>(context);
}

// ---- Factory -------------------------------------------------------------------------------

UdpSocket* EspUdpSocketFactory::create(const char** error) {
	size_t need = sizeof(EspTransientSocket) + kOverhead;
	size_t free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
	size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
	if (free < need + kHeapFloor || largest < EspTransientSocket::kStack) {
		std::snprintf(error_, sizeof(error_), "not enough memory: %u B free, the socket needs %u B and must leave %u B",
			static_cast<unsigned>(free), static_cast<unsigned>(need), static_cast<unsigned>(kHeapFloor));
		*error = error_;
		return nullptr;
	}
	EspTransientSocket* socket = new (std::nothrow) EspTransientSocket(queue_);
	if (socket == nullptr) {
		*error = "not enough memory for the socket";
		return nullptr;
	}
	if (!socket->start(error)) {
		delete socket; // no task was created: nothing refers to it
		return nullptr;
	}
	ESP_LOGI(kTag, "transient socket: about %u B of %u B free heap while it is in use", static_cast<unsigned>(need),
		static_cast<unsigned>(free));
	return socket;
}

void EspUdpSocketFactory::release(UdpSocket* socket) {
	if (socket != nullptr) {
		static_cast<EspTransientSocket*>(socket)->stop();
	}
}

} // namespace esp
} // namespace hc
