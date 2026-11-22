/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_memory_watch.h"

#include "heap_watch.h"

namespace hc {
namespace esp {

bool EspMemoryWatch::dma_room(DmaRoom& out) const {
	return heap_watch::dma_room(out.free_bytes, out.largest_block);
}

void EspMemoryWatch::notify_at(uint32_t free_bytes, MemoryListener& listener) {
	listener_ = &listener;
	free_bytes_.store(free_bytes);
	generation_.fetch_add(1);
	heap_watch::notify_free_at(free_bytes, &EspMemoryWatch::freed, this);
}

void EspMemoryWatch::cancel_notify() {
	heap_watch::cancel_free_notify();
	generation_.fetch_add(1);
	listener_ = nullptr;
}

void EspMemoryWatch::freed(void* self) {
	EspMemoryWatch& watch = *static_cast<EspMemoryWatch*>(self);
	if (!watch.queue_.post_quiet(&EspMemoryWatch::on_loop, self, watch.generation_.load(), "memory freed")) {
		// The queue is full for a moment: the next free tries again.
		heap_watch::notify_free_at(watch.free_bytes_.load(), &EspMemoryWatch::freed, self);
	}
}

void EspMemoryWatch::on_loop(void* context, const LoopMessage& message) {
	EspMemoryWatch& watch = *static_cast<EspMemoryWatch*>(context);
	if (message.number != watch.generation_.load() || watch.listener_ == nullptr) {
		return; // cancelled or replaced meanwhile
	}
	MemoryListener* listener = watch.listener_;
	watch.listener_ = nullptr;
	listener->memory_freed();
}

} // namespace esp
} // namespace hc
