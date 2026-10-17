/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/common/bsd_udp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace hc {
namespace bsd {

namespace {

bool fail(char* error, size_t size, const char* what) {
	std::snprintf(error, size, "%s: %s", what, std::strerror(errno));
	return false;
}

sockaddr_in to_sockaddr(const Ipv4Endpoint& endpoint) {
	sockaddr_in address;
	std::memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(endpoint.address);
	address.sin_port = htons(endpoint.port);
	return address;
}

} // namespace

int udp_open(char* error, size_t size) {
	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		fail(error, size, "socket");
		return -1;
	}
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		fail(error, size, "fcntl O_NONBLOCK");
		close(fd);
		return -1;
	}
	sockaddr_in any = to_sockaddr(Ipv4Endpoint{0, 0});
	if (bind(fd, reinterpret_cast<sockaddr*>(&any), sizeof(any)) < 0) {
		fail(error, size, "bind");
		close(fd);
		return -1;
	}
	return fd;
}

void udp_close(int fd) {
	if (fd >= 0) {
		close(fd);
	}
}

bool udp_send_to(int fd, const Ipv4Endpoint& to, const uint8_t* data, size_t length, char* error, size_t size) {
	sockaddr_in address = to_sockaddr(to);
	ssize_t sent = sendto(fd, data, length, 0, reinterpret_cast<sockaddr*>(&address), sizeof(address));
	if (sent < 0) {
		return fail(error, size, "sendto");
	}
	if (static_cast<size_t>(sent) != length) {
		std::snprintf(error, size, "sendto: %d of %u octets sent", static_cast<int>(sent), static_cast<unsigned>(length));
		return false;
	}
	return true;
}

int udp_receive(int fd, uint8_t* buffer, size_t capacity, Ipv4Endpoint& from, char* error, size_t size) {
	sockaddr_in address;
	socklen_t address_length = sizeof(address);
	ssize_t received = recvfrom(fd, buffer, capacity, 0, reinterpret_cast<sockaddr*>(&address), &address_length);
	if (received < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			return 0;
		}
		fail(error, size, "recvfrom");
		return -1;
	}
	from.address = ntohl(address.sin_addr.s_addr);
	from.port = ntohs(address.sin_port);
	// An empty datagram carries nothing: it is consumed and reported as none (the caller polls again).
	return received == 0 ? 0 : static_cast<int>(received);
}

bool udp_local_endpoint(int fd, const Ipv4Endpoint& remote, Ipv4Endpoint& local, char* error, size_t size) {
	sockaddr_in bound;
	socklen_t bound_length = sizeof(bound);
	if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_length) < 0) {
		return fail(error, size, "getsockname");
	}
	int scratch = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (scratch < 0) {
		return fail(error, size, "socket");
	}
	// Connecting a UDP socket sends nothing; it only picks the route and with it the local address.
	sockaddr_in target = to_sockaddr(remote);
	sockaddr_in chosen;
	socklen_t chosen_length = sizeof(chosen);
	bool ok = connect(scratch, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0 &&
		getsockname(scratch, reinterpret_cast<sockaddr*>(&chosen), &chosen_length) == 0;
	if (!ok) {
		fail(error, size, "no route to the gateway");
		close(scratch);
		return false;
	}
	close(scratch);
	local.address = ntohl(chosen.sin_addr.s_addr);
	local.port = ntohs(bound.sin_port);
	if (local.address == 0) {
		std::snprintf(error, size, "no local IPv4 address towards the gateway");
		return false;
	}
	return true;
}

bool resolve_ipv4(const char* host, uint32_t& address, char* error, size_t size) {
	addrinfo hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo* result = nullptr;
	int status = getaddrinfo(host, nullptr, &hints, &result);
	if (status != 0 || result == nullptr) {
		const char* reason = status == EAI_NONAME ? "host not found" : status == EAI_FAIL ? "name server failure" :
			status == EAI_MEMORY ? "out of memory" : "resolution failed";
		std::snprintf(error, size, "%s (getaddrinfo %d)", reason, status);
		if (result != nullptr) {
			freeaddrinfo(result);
		}
		return false;
	}
	const sockaddr_in* found = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
	address = ntohl(found->sin_addr.s_addr);
	freeaddrinfo(result);
	return true;
}

} // namespace bsd
} // namespace hc
