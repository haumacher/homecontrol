/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_UDP_SOCKET_H
#define HC_POSIX_UDP_SOCKET_H

#include <cstddef>
#include <cstdint>

#include "hc/event_loop.h"
#include "hc/platform/udp_socket.h"

namespace hc {
namespace posix {

/// hc::UdpSocket on BSD sockets (platform/common/bsd_udp). The loop stays single-threaded: the
/// platform's main loop sleeps in wait() instead of nanosleep, which polls the descriptor and hands
/// the waiting datagrams to the receiver on the loop's thread. resolve() calls getaddrinfo (it
/// blocks the loop while the name server answers, acceptable for the host build) and delivers the
/// result through EventLoop::post().
class PosixUdpSocket : public UdpSocket {
public:
	explicit PosixUdpSocket(EventLoop& loop);
	~PosixUdpSocket();

	PosixUdpSocket(const PosixUdpSocket&) = delete;
	PosixUdpSocket& operator=(const PosixUdpSocket&) = delete;

	bool open(UdpReceiver& receiver, const char** error) override;
	void close() override;
	bool is_open() const override { return fd_ >= 0; }
	bool send_to(const Ipv4Endpoint& to, const uint8_t* data, size_t length, const char** error) override;
	bool local_endpoint(const Ipv4Endpoint& remote, Ipv4Endpoint& local, const char** error) override;
	bool resolve(const char* host, UdpReceiver& receiver, const char** error) override;

	/// Sleeps up to `timeout_ms` or until a datagram arrives (or a signal interrupts), then delivers
	/// the waiting datagrams. Call it between EventLoop::run_once() calls, on the loop's thread.
	void wait(uint64_t timeout_ms);
	/// The socket's descriptor for a main loop that polls several descriptors (-1 while closed);
	/// after poll() reports it readable, call drain().
	int fd() const { return fd_; }
	/// Delivers the waiting datagrams to the receiver on the loop's thread (bounded).
	void drain();

private:
	void deliver_resolution();

	EventLoop& loop_;
	int fd_ = -1;
	UdpReceiver* receiver_ = nullptr;
	char error_[96] = {};
	UdpReceiver* resolve_receiver_ = nullptr;
	bool resolve_ok_ = false;
	uint32_t resolve_address_ = 0;
	char resolve_error_[96] = {};
	uint32_t resolve_generation_ = 0;
	uint32_t resolve_posted_ = 0;
};

} // namespace posix
} // namespace hc

#endif // HC_POSIX_UDP_SOCKET_H
