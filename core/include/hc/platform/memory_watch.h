/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_MEMORY_WATCH_H
#define HC_PLATFORM_MEMORY_WATCH_H

#include <cstdint>

namespace hc {

/// The memory DMA reaches (on the ESP32-C6 the HP SRAM; the LP SRAM is CPU-only): what the WiFi
/// driver's frame copies and the AES engine's buffers must come from (homecontrol #110/#112).
struct DmaRoom {
	uint32_t free_bytes = 0;
	uint32_t largest_block = 0;
};

/// Told on the event loop that memory was freed (MemoryWatch::notify_at()).
class MemoryListener {
public:
	virtual void memory_freed() = 0;

protected:
	~MemoryListener() = default;
};

/// The platform's view of its DMA-capable memory for the TLS gate (hc::TlsGate). A platform without
/// such a distinction (the host) has none: the gate then admits by its order alone.
///
/// Threading: called on the event loop's thread; the notification arrives there too.
class MemoryWatch {
public:
	/// The DMA-capable memory now. False: the platform has no such figure.
	virtual bool dma_room(DmaRoom& out) const = 0;

	/// One notification, on the loop, as soon as the DMA-capable free memory has reached `free_bytes`
	/// after a free (no polling: the platform checks when memory is freed). A new call replaces the
	/// previous request.
	virtual void notify_at(uint32_t free_bytes, MemoryListener& listener) = 0;

	/// Drops a pending notify_at() (none follows).
	virtual void cancel_notify() = 0;

protected:
	~MemoryWatch() = default;
};

} // namespace hc

#endif // HC_PLATFORM_MEMORY_WATCH_H
