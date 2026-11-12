/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_UDP_SOCKET_H
#define HC_PLATFORM_UDP_SOCKET_H

#include <cstddef>
#include <cstdint>

#include "hc/ipv4.h"

namespace hc {

/// Receives what a UdpSocket delivers: datagrams, receive errors and the results of resolve().
/// Always called on the event loop's thread, never from within a UdpSocket call.
class UdpReceiver {
public:
	/// A datagram from `from`. The octets are valid during the call only.
	virtual void udp_received(const uint8_t* data, size_t length, const Ipv4Endpoint& from) = 0;

	/// Receiving failed for `reason`; the socket stays open (the core decides what to do).
	virtual void udp_error(const char* reason) = 0;

	/// The outcome of resolve(): `ok` with the IPv4 address (host byte order), or the reason.
	virtual void udp_resolved(bool ok, uint32_t address, const char* error) = 0;

protected:
	~UdpReceiver() = default;
};

/// One UDP socket of the platform (BSD sockets on POSIX and over lwIP on the ESP). The KNXnet/IP
/// tunnel opens it for a connection and closes it when the connection ends; the platform moves the
/// datagrams and hands them to the receiver on the event loop's thread (POSIX polls the descriptor
/// in the loop's sleep, the ESP has a reader task that posts through its loop queue).
///
/// Every call is made on the event loop's thread. A refused call returns false and points `error`
/// to a reason (valid until the next call), which the caller logs.
class UdpSocket {
public:
	/// Longest datagram delivered; longer ones are dropped by the platform (logged and counted, never
	/// truncated). The controller's own KNXnet/IP tunnelling frames are at most about 60 octets, search
	/// and description responses well under 256; a bus session (spec/api.md "Bus session") forwards
	/// frames up to an APDU of 254 octets, whose TUNNELING_REQUEST takes 274 octets plus the additional
	/// information a gateway may put into an L_Data.ind. The ESP's receive ring holds slots of this
	/// size (issue #44).
	static constexpr size_t kMaxDatagram = 320;

	/// Opens the socket on an ephemeral port of all local IPv4 addresses (closing it first if it
	/// is open); its datagrams and errors go to `receiver`.
	virtual bool open(UdpReceiver& receiver, const char** error) = 0;

	/// Closes the socket; datagrams still in flight are discarded. Harmless when closed.
	virtual void close() = 0;

	virtual bool is_open() const = 0;

	/// Sends one datagram to `to`. The socket stays open when this fails.
	virtual bool send_to(const Ipv4Endpoint& to, const uint8_t* data, size_t length, const char** error) = 0;

	/// The endpoint a peer at `remote` sees: the address of the interface the route to `remote` takes,
	/// and the port of the open socket.
	virtual bool local_endpoint(const Ipv4Endpoint& remote, Ipv4Endpoint& local, const char** error) = 0;

	/// Starts resolving the host name `host` to an IPv4 address; the result goes to
	/// `receiver.udp_resolved()` later (needs no open socket). One resolution at a time: a new one
	/// replaces a pending one, whose result is then not delivered.
	virtual bool resolve(const char* host, UdpReceiver& receiver, const char** error) = 0;

protected:
	~UdpSocket() = default;
};

/// UDP sockets for a short use (the KNX gateway search, a few seconds a few times in a device's
/// life): created on the heap with whatever the platform needs to receive (the ESP a reader task)
/// and given back when done, so that nothing of them is held while idle.
///
/// Every call is made on the event loop's thread.
class UdpSocketFactory {
public:
	/// A new socket (closed; open() it), or nullptr with `error` (valid until the next call): not
	/// enough memory, no task. The caller gives it back with release().
	virtual UdpSocket* create(const char** error) = 0;

	/// Closes `socket` and frees it (on the ESP once its reader has stopped, shortly after). The
	/// receiver is not called any more after this call. nullptr is harmless.
	virtual void release(UdpSocket* socket) = 0;

protected:
	~UdpSocketFactory() = default;
};

} // namespace hc

#endif // HC_PLATFORM_UDP_SOCKET_H
