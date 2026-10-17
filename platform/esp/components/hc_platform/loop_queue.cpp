/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "loop_queue.h"

#include <cstdio>

#include "esp_log.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.queue";

} // namespace

bool LoopQueue::init() {
	queue_ = xQueueCreateStatic(kDepth, sizeof(LoopMessage), storage_, &control_);
	if (queue_ == nullptr) {
		ESP_LOGE(kTag, "cannot create the event-loop queue");
		return false;
	}
	return true;
}

bool LoopQueue::post(LoopMessage::Handler handler, void* context, int32_t number, const char* text,
	const char* what) {
	if (queue_ == nullptr) {
		ESP_LOGE(kTag, "event-loop queue not initialized, dropping %s", what);
		return false;
	}
	LoopMessage message;
	message.handler = handler;
	message.context = context;
	message.number = number;
	std::snprintf(message.text, sizeof(message.text), "%s", text == nullptr ? "" : text);
	if (xQueueSend(queue_, &message, 0) != pdTRUE) {
		ESP_LOGE(kTag, "event-loop queue full (depth %u), dropping %s", static_cast<unsigned>(kDepth), what);
		return false;
	}
	return true;
}

void LoopQueue::dispatch(TickType_t ticks) {
	LoopMessage message;
	if (xQueueReceive(queue_, &message, ticks) != pdTRUE) {
		return;
	}
	// Bounded, so that a handler that posts again cannot keep the loop from its timers.
	size_t handled = 0;
	do {
		message.handler(message.context, message);
	} while (++handled < kDepth && xQueueReceive(queue_, &message, 0) == pdTRUE);
}

} // namespace esp
} // namespace hc
