/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/udp_socket.h"

#include <poll.h>

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <new>

#include "hc/common/bsd_udp.h"
#include "hc/log.h"

namespace hc {
namespace posix {

namespace {

constexpr const char* kTag = "hc.posix.udp";

} // namespace

PosixUdpSocket::PosixUdpSocket(EventLoop& loop) : loop_(loop) {}

PosixUdpSocket::~PosixUdpSocket() {
	close();
}

bool PosixUdpSocket::open(UdpReceiver& receiver, const char** error) {
	close();
	fd_ = bsd::udp_open(error_, sizeof(error_));
	if (fd_ < 0) {
		*error = error_;
		return false;
	}
	receiver_ = &receiver;
	return true;
}

void PosixUdpSocket::close() {
	bsd::udp_close(fd_);
	fd_ = -1;
}

bool PosixUdpSocket::send_to(const Ipv4Endpoint& to, const uint8_t* data, size_t length, const char** error) {
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

bool PosixUdpSocket::local_endpoint(const Ipv4Endpoint& remote, Ipv4Endpoint& local, const char** error) {
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

bool PosixUdpSocket::resolve(const char* host, UdpReceiver& receiver, const char** error) {
	++resolve_generation_;
	resolve_receiver_ = &receiver;
	resolve_ok_ = bsd::resolve_ipv4(host, resolve_address_, resolve_error_, sizeof(resolve_error_));
	resolve_posted_ = resolve_generation_;
	if (!loop_.post(Callback::member<PosixUdpSocket, &PosixUdpSocket::deliver_resolution>(this))) {
		*error = "event loop queue full";
		return false;
	}
	return true;
}

void PosixUdpSocket::deliver_resolution() {
	if (resolve_posted_ != resolve_generation_ || resolve_receiver_ == nullptr) {
		return;
	}
	UdpReceiver* receiver = resolve_receiver_;
	resolve_receiver_ = nullptr;
	receiver->udp_resolved(resolve_ok_, resolve_address_, resolve_ok_ ? "" : resolve_error_);
}

void PosixUdpSocket::wait(uint64_t timeout_ms) {
	if (fd_ < 0) {
		timespec duration;
		duration.tv_sec = static_cast<time_t>(timeout_ms / 1000);
		duration.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000);
		nanosleep(&duration, nullptr); // a signal interrupts it
		return;
	}
	pollfd entry;
	entry.fd = fd_;
	entry.events = POLLIN;
	entry.revents = 0;
	int ready = poll(&entry, 1, static_cast<int>(timeout_ms));
	if (ready > 0 && (entry.revents & (POLLIN | POLLERR)) != 0) {
		drain();
	}
}

void PosixUdpSocket::drain() {
	uint8_t buffer[kMaxDatagram + 1];
	// Bounded, so a flood cannot keep the loop from its timers.
	for (int i = 0; i < 64 && fd_ >= 0; ++i) {
		Ipv4Endpoint from;
		int length = bsd::udp_receive(fd_, buffer, sizeof(buffer), from, error_, sizeof(error_));
		if (length == 0) {
			return;
		}
		if (length < 0) {
			receiver_->udp_error(error_);
			return;
		}
		if (static_cast<size_t>(length) > kMaxDatagram) {
			char text[Ipv4Endpoint::kMaxText];
			Log::warn(kTag, "dropping a datagram from %s longer than %u octets", from.format(text, sizeof(text)),
				static_cast<unsigned>(kMaxDatagram));
			continue;
		}
		receiver_->udp_received(buffer, static_cast<size_t>(length), from);
	}
}

PosixUdpSocketFactory::~PosixUdpSocketFactory() {
	delete current_;
}

UdpSocket* PosixUdpSocketFactory::create(const char** error) {
	if (current_ != nullptr) {
		*error = "a socket of the factory is in use";
		return nullptr;
	}
	current_ = new (std::nothrow) PosixUdpSocket(loop_);
	if (current_ == nullptr) {
		*error = "not enough memory";
	}
	return current_;
}

void PosixUdpSocketFactory::release(UdpSocket* socket) {
	if (socket == nullptr || socket != current_) {
		return;
	}
	delete current_; // closes it
	current_ = nullptr;
}

} // namespace posix
} // namespace hc
