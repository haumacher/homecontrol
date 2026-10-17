/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IPV4_H
#define HC_IPV4_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// An IPv4 address with a UDP or TCP port; the address in host byte order (192.0.2.1 is
/// 0xC0000201).
struct Ipv4Endpoint {
	uint32_t address = 0;
	uint16_t port = 0;

	/// Longest text format() writes, including the NUL ("255.255.255.255:65535").
	static constexpr size_t kMaxText = 22;

	/// 0.0.0.0:0 (KNXnet/IP's NAT mode endpoint: "answer to where the request came from").
	bool is_any() const { return address == 0 && port == 0; }

	/// Writes "192.0.2.1:3671". Returns `buffer`.
	const char* format(char* buffer, size_t size) const;

	friend bool operator==(Ipv4Endpoint a, Ipv4Endpoint b) { return a.address == b.address && a.port == b.port; }
	friend bool operator!=(Ipv4Endpoint a, Ipv4Endpoint b) { return !(a == b); }
};

/// Longest IPv4 address text including the NUL ("255.255.255.255").
constexpr size_t kMaxIpv4Text = 16;

/// Reads an IPv4 literal "192.0.2.1" (four decimal octets 0..255 of at most 3 digits, nothing else)
/// into host byte order. False for anything else.
bool parse_ipv4(const char* text, uint32_t& address);

/// Writes "192.0.2.1". Returns `buffer`.
const char* format_ipv4(uint32_t address, char* buffer, size_t size);

} // namespace hc

#endif // HC_IPV4_H
