/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_UDP_SOCKET_H
#define HC_ESP_UDP_SOCKET_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hc/platform/udp_socket.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// hc::UdpSocket over lwIP's BSD sockets. The event loop opens, sends and closes; the reader task
/// `hc_udp` waits on the socket (select, 100 ms), receives each datagram straight into the next
/// free one of a ring of kRing slots of kMaxDatagram octets and posts one message through the LoopQueue, whose handler hands the ring's datagrams to the
/// receiver on the loop's thread. It also runs resolve()'s getaddrinfo, so the loop never blocks on
/// the name server. The reader closes descriptors the loop retires (never one it may be waiting on);
/// datagrams of a closed socket are discarded by a generation count. Statically allocated.
class EspUdpSocket : public UdpSocket {
public:
	/// Datagrams waiting for the loop; more are dropped (counted, logged).
	static constexpr size_t kRing = 8;

	explicit EspUdpSocket(LoopQueue& queue);

	EspUdpSocket(const EspUdpSocket&) = delete;
	EspUdpSocket& operator=(const EspUdpSocket&) = delete;

	/// Creates the reader task. False (logged) if it cannot: open() then refuses.
	bool init();

	bool open(UdpReceiver& receiver, const char** error) override;
	void close() override;
	bool is_open() const override { return fd_ >= 0; }
	bool send_to(const Ipv4Endpoint& to, const uint8_t* data, size_t length, const char** error) override;
	bool local_endpoint(const Ipv4Endpoint& remote, Ipv4Endpoint& local, const char** error) override;
	bool resolve(const char* host, UdpReceiver& receiver, const char** error) override;

	/// Datagrams dropped because the ring was full, since boot.
	uint32_t dropped() const { return dropped_.load(); }
	/// Datagrams dropped because they were longer than kMaxDatagram, since boot.
	uint32_t oversized() const { return oversized_.load(); }

private:
	static constexpr size_t kRetire = 4;
	static constexpr size_t kMaxHost = 128;
	/// The reader's stack: select, recvfrom straight into the ring, and resolve()'s getaddrinfo.
	/// Measured on the C6 (2026-11-03, a gateway by address): 784 B used; 3072 B leave 2.2 KB for
	/// getaddrinfo of a gateway by name (lwIP's DNS client, not measured on the desk) and a log line.
	/// 4096 B before issue #45.
	static constexpr uint32_t kStack = 3072;

	struct Slot {
		uint32_t generation;
		Ipv4Endpoint from;
		uint16_t length;
		/// One octet more than a datagram may have: the reader receives straight into the slot, and
		/// a datagram that fills it was longer (dropped, not truncated).
		uint8_t data[kMaxDatagram + 1];
	};

	static void task_main(void* self);
	void run();
	void receive(int fd);
	void retire_closed();
	void do_resolve();
	static void handle_datagrams(void* context, const LoopMessage& message);
	static void handle_error(void* context, const LoopMessage& message);
	static void handle_resolved(void* context, const LoopMessage& message);
	void drain();

	LoopQueue& queue_;
	TaskHandle_t task_ = nullptr;
	StaticTask_t task_storage_;
	StackType_t stack_[kStack / sizeof(StackType_t)];
	SemaphoreHandle_t mutex_ = nullptr;
	StaticSemaphore_t mutex_storage_;

	// Loop side.
	int fd_ = -1;
	UdpReceiver* receiver_ = nullptr;
	char error_[96] = {};

	// Shared with the reader task.
	std::atomic<int> active_fd_{-1};
	std::atomic<uint32_t> generation_{0};
	std::atomic<int> retire_[kRetire];
	std::atomic<bool> drain_posted_{false};
	std::atomic<uint32_t> dropped_{0};
	std::atomic<uint32_t> oversized_{0};
	Slot ring_[kRing];          ///< Guarded by mutex_ (the reader fills the claimed tail slot outside it).
	size_t ring_head_ = 0;       ///< Guarded by mutex_.
	size_t ring_count_ = 0;      ///< Guarded by mutex_.

	// Resolution: the loop writes the host and bumps the request; the reader answers it.
	char resolve_host_[kMaxHost] = {}; ///< Guarded by mutex_.
	std::atomic<uint32_t> resolve_request_{0};
	uint32_t resolve_served_ = 0;      ///< Reader task only.
	uint32_t resolve_answered_ = 0;    ///< The request the posted answer belongs to (written before the post).
	UdpReceiver* resolve_receiver_ = nullptr;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_UDP_SOCKET_H
