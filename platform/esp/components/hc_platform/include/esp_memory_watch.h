/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_MEMORY_WATCH_H
#define HC_ESP_MEMORY_WATCH_H

#include <atomic>
#include <cstdint>

#include "hc/platform/memory_watch.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The device's DMA-capable memory for the TLS gate (homecontrol #110/#112): the HP SRAM's free bytes
/// and largest block (heap_watch::dma_room()), and the notification after a free that left enough of
/// it (heap_watch::notify_free_at(), the heap's free hook), handed to the loop through the LoopQueue.
class EspMemoryWatch : public MemoryWatch {
public:
	explicit EspMemoryWatch(LoopQueue& queue) : queue_(queue) {}

	EspMemoryWatch(const EspMemoryWatch&) = delete;
	EspMemoryWatch& operator=(const EspMemoryWatch&) = delete;

	bool dma_room(DmaRoom& out) const override;
	void notify_at(uint32_t free_bytes, MemoryListener& listener) override;
	void cancel_notify() override;

private:
	/// In the freeing task (heap_watch's free hook): posts to the loop.
	static void freed(void* self);
	static void on_loop(void* context, const LoopMessage& message);

	LoopQueue& queue_;
	MemoryListener* listener_ = nullptr; ///< Loop only.
	std::atomic<uint32_t> free_bytes_{0}; ///< The request, for a renewed hook after a full queue.
	std::atomic<int32_t> generation_{0}; ///< Tells a stale notification from the current one.
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_MEMORY_WATCH_H
