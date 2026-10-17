/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_IMPROV_PORT_H
#define HC_PLATFORM_IMPROV_PORT_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// The serial port an Improv Wi-Fi client (ESP Web Tools in a browser) talks to, and what the
/// device tells it about itself. The ESP platform shares the USB Serial/JTAG port between the
/// console and Improv; it parses the incoming frames (hc::ImprovReader) and hands them to
/// hc::Improv::handle_frame() on the event loop's thread. Every call below is made on that thread.
class ImprovPort {
public:
	/// Sends one complete frame (magic through checksum). The platform frames it as the Improv
	/// serial clients expect (a newline before and after, in one write, so log output cannot land
	/// inside it). Must not block indefinitely when nobody reads.
	virtual void send_frame(const uint8_t* frame, size_t length) = 0;

	/// Improv has ended (doc/network.md, "Improv / web flasher"): from now on the port belongs to
	/// the console and the log alone; the platform stops looking for frames.
	virtual void improv_ended() = 0;

	/// The chip as the web flasher names it ("ESP32-C6").
	virtual const char* chip_family() const = 0;

	/// The device's network name ("homecontrol-a1b2c3"), also its mDNS host name.
	virtual const char* device_name() const = 0;

protected:
	~ImprovPort() = default;
};

} // namespace hc

#endif // HC_PLATFORM_IMPROV_PORT_H
