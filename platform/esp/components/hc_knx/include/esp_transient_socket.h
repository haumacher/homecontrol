/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_TRANSIENT_SOCKET_H
#define HC_ESP_TRANSIENT_SOCKET_H

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

class EspUdpSocketFactory;

/// A UDP socket for a few seconds (the KNX gateway search): it and its reader task `hc_udp_tmp` live
/// on the heap only between EspUdpSocketFactory::create() and release(), so that an idle device
/// holds nothing of them. The reader waits on the socket (select, 100 ms), receives into a ring of
/// kRing slots and posts one LoopQueue message, whose handler hands the datagrams to the receiver on
/// the loop's thread (as EspUdpSocket does).
///
/// One-shot: it opens once; close() stops the delivery, the descriptor stays with the reader. On
/// release() the reader closes the descriptor, posts its exit through the LoopQueue and deletes its
/// task; the exit message, behind every message the reader posted before (the queue is FIFO), frees
/// the object on the loop. It resolves no names.
class EspTransientSocket final : public UdpSocket {
public:
	/// Datagrams waiting for the loop; more are dropped (logged).
	static constexpr size_t kRing = 4;
	/// The reader's stack: select and recvfrom, a log line; no getaddrinfo (the tunnel's reader, which
	/// also resolves, used 784 B of 3072 on the C6 with a gateway by address).
	static constexpr uint32_t kStack = 2560;

	EspTransientSocket(const EspTransientSocket&) = delete;
	EspTransientSocket& operator=(const EspTransientSocket&) = delete;

	bool open(UdpReceiver& receiver, const char** error) override;
	void close() override;
	bool is_open() const override { return fd_ >= 0; }
	bool send_to(const Ipv4Endpoint& to, const uint8_t* data, size_t length, const char** error) override;
	bool local_endpoint(const Ipv4Endpoint& remote, Ipv4Endpoint& local, const char** error) override;
	bool resolve(const char* host, UdpReceiver& receiver, const char** error) override;

private:
	friend class EspUdpSocketFactory;

	struct Slot {
		Ipv4Endpoint from;
		uint16_t length;
		/// One octet more than a datagram may have: a datagram that fills it was longer (dropped).
		uint8_t data[kMaxDatagram + 1];
	};

	explicit EspTransientSocket(LoopQueue& queue);
	~EspTransientSocket();

	/// Creates the mutex and the reader task. False with `error`.
	bool start(const char** error);
	/// From release(): stops the delivery and asks the reader to end; the object is freed later.
	void stop();

	static void task_main(void* self);
	void run();
	void receive(int fd);
	static void handle_datagrams(void* context, const LoopMessage& message);
	static void handle_exited(void* context, const LoopMessage& message);
	void drain();

	LoopQueue& queue_;
	TaskHandle_t task_ = nullptr;
	SemaphoreHandle_t mutex_ = nullptr;

	// Loop side.
	int fd_ = -1;
	bool used_ = false;
	UdpReceiver* receiver_ = nullptr;
	char error_[96] = {};

	// Shared with the reader task.
	std::atomic<int> active_fd_{-1};
	std::atomic<bool> stop_{false};
	std::atomic<bool> drain_posted_{false};
	Slot ring_[kRing];      ///< Guarded by mutex_ (the reader fills the claimed tail slot outside it).
	size_t ring_head_ = 0;   ///< Guarded by mutex_.
	size_t ring_count_ = 0;  ///< Guarded by mutex_.
};

/// The platform's UdpSocketFactory: EspTransientSockets, refused while the heap is low.
class EspUdpSocketFactory final : public UdpSocketFactory {
public:
	/// Free heap a socket must leave behind; below it create() refuses (the heap after loading a
	/// large runtime image sits around 50 KB at its lowest).
	static constexpr size_t kHeapFloor = 20 * 1024;
	/// What a socket takes besides the object: its task's stack and control block, the mutex, lwIP's
	/// socket (an estimate for the floor check).
	static constexpr size_t kOverhead = EspTransientSocket::kStack + 1024;

	explicit EspUdpSocketFactory(LoopQueue& queue) : queue_(queue) {}

	UdpSocket* create(const char** error) override;
	void release(UdpSocket* socket) override;

private:
	LoopQueue& queue_;
	char error_[112] = {};
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_TRANSIENT_SOCKET_H
