/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_COMMON_BSD_UDP_H
#define HC_COMMON_BSD_UDP_H

#include <cstddef>
#include <cstdint>

#include "hc/ipv4.h"

namespace hc {
namespace bsd {

/// UDP over the BSD socket API, shared by the POSIX build and the ESP build (lwIP's BSD sockets):
/// the socket calls are the same, only how a platform waits for datagrams and hands them to the
/// event loop differs (POSIX polls in the loop's sleep, the ESP has a reader task). Every function
/// that fails writes a reason into `error` (capacity `size`).

/// Longest reason text including the NUL.
constexpr size_t kMaxError = 96;

/// Opens a non-blocking UDP socket bound to an ephemeral port of all local IPv4 addresses. Returns
/// the descriptor, -1 on failure.
int udp_open(char* error, size_t size);

/// Closes the descriptor (ignores -1).
void udp_close(int fd);

bool udp_send_to(int fd, const Ipv4Endpoint& to, const uint8_t* data, size_t length, char* error, size_t size);

/// Receives one datagram without blocking into `buffer`. Returns its length, 0 if none is waiting,
/// -1 on an error. A datagram longer than `capacity` is cut at it (the caller passes one octet
/// more than it accepts, to recognize one that is too long).
int udp_receive(int fd, uint8_t* buffer, size_t capacity, Ipv4Endpoint& from, char* error, size_t size);

/// The address of the interface the route to `remote` takes (a connected scratch socket's local
/// address) with the port `fd` is bound to.
bool udp_local_endpoint(int fd, const Ipv4Endpoint& remote, Ipv4Endpoint& local, char* error, size_t size);

/// Resolves `host` to its first IPv4 address (getaddrinfo, blocking).
bool resolve_ipv4(const char* host, uint32_t& address, char* error, size_t size);

} // namespace bsd
} // namespace hc

#endif // HC_COMMON_BSD_UDP_H
