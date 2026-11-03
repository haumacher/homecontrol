/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "loop_queue.h"

#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"

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
	// A plain bounded copy: post() runs on foreign tasks with small stacks (the IDF's sys_evt among
	// them), where the ROM's snprintf (newlib's full _svfprintf_r) once overflowed the stack (#48).
	size_t length = 0;
	if (text != nullptr) {
		while (length + 1 < sizeof(message.text) && text[length] != '\0') {
			message.text[length] = text[length];
			++length;
		}
	}
	message.text[length] = '\0';
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
	uint64_t started_ms = static_cast<uint64_t>(esp_timer_get_time()) / 1000u; // the EspClock's time base
	size_t handled = 0;
	do {
		uint64_t one = static_cast<uint64_t>(esp_timer_get_time()) / 1000u;
		message.handler(message.context, message);
		// As the event loop's handlers (EventLoop::invoke, homecontrol #50): a message that holds the
		// loop longer than 25 ms is logged with the address of its handler.
		uint64_t took = static_cast<uint64_t>(esp_timer_get_time()) / 1000u - one;
		if (took > 25) {
			ESP_LOGW(kTag, "slow message handler %p took %llu ms", reinterpret_cast<void*>(message.handler),
				static_cast<unsigned long long>(took));
		}
	} while (++handled < kDepth && xQueueReceive(queue_, &message, 0) == pdTRUE);
	if (loop_ != nullptr) {
		loop_->account_turn(started_ms, static_cast<uint64_t>(esp_timer_get_time()) / 1000u);
	}
}

} // namespace esp
} // namespace hc
