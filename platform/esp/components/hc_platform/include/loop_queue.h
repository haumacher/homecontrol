/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_LOOP_QUEUE_H
#define HC_ESP_LOOP_QUEUE_H

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hc/event_loop.h"

namespace hc {
namespace esp {

/// A message for the event-loop task, copied by value into the queue.
struct LoopMessage {
	/// Runs on the event-loop task.
	using Handler = void (*)(void* context, const LoopMessage& message);

	/// Longest text payload including the NUL; longer texts are truncated.
	static constexpr size_t kMaxText = 48;

	Handler handler;
	void* context;
	int32_t number;
	char text[kMaxText];
};

/// The platform-provided thread-safe queue with wakeup that doc/core.md announces: other tasks
/// (the IDF event task, the console REPL) post messages; the event-loop task sleeps in dispatch()
/// until the next timer deadline or the next message, whichever comes first, and runs the
/// messages between two EventLoop::run_once() calls, on its own thread. Statically allocated.
class LoopQueue {
public:
	static constexpr size_t kDepth = 16;

	/// Creates the queue. Call once before any post().
	bool init();

	/// Queues a message (any task, not from an ISR). Never blocks; when the queue is full the
	/// message is dropped and an error logged with `what`.
	bool post(LoopMessage::Handler handler, void* context, int32_t number, const char* text, const char* what);

	/// Event-loop task only: waits up to `ticks` for a message, then runs the waiting messages (at
	/// most kDepth, the rest in the next call).
	void dispatch(TickType_t ticks);

	/// The loop whose turn figures include the messages' run time (EventLoop::account_turn(): a web
	/// request or a console command runs here, outside run_once). nullptr: not accounted.
	void set_loop(EventLoop* loop) { loop_ = loop; }

private:
	StaticQueue_t control_;
	uint8_t storage_[kDepth * sizeof(LoopMessage)];
	QueueHandle_t queue_ = nullptr;
	EventLoop* loop_ = nullptr;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_LOOP_QUEUE_H
