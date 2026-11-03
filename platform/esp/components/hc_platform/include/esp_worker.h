/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_WORKER_H
#define HC_ESP_WORKER_H

#include <atomic>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "hc/platform/worker.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The core's Worker on the ESP (homecontrol #68, doc/core.md "Off the loop"): each job runs in a
/// task of its own, `hc_work`, created for it and deleted after it (as the HTTP fetcher's and the
/// image download's tasks are), so its stack is taken from the heap only while a job runs; `done`
/// reaches the loop through the LoopQueue.
///
/// The one job today is the runtime image's store (ImageStore::write on LittleFS: the chunk and
/// manifest writes, the temporary file and rename, the garbage collection with its directory walk,
/// and the log lines on a failure). kStack covers it with margin; the task's unused stack is logged
/// with every job's end, so the margin can be read off the device.
class EspWorker : public Worker {
public:
	static constexpr uint32_t kStack = 4096;
	/// Below the event loop (5), the E-Home socket (4) and the console writer (3): the store fills the
	/// loop's idle time.
	static constexpr UBaseType_t kPriority = 2;

	explicit EspWorker(LoopQueue& queue) : queue_(queue) {}
	EspWorker(const EspWorker&) = delete;
	EspWorker& operator=(const EspWorker&) = delete;

	bool run(Callback job, Callback done, const char** error) override;
	bool busy() const override { return busy_.load(); }

private:
	static void task(void* context);
	static void finished(void* context, const LoopMessage& message);

	LoopQueue& queue_;
	Callback job_;
	Callback done_;
	std::atomic<bool> busy_{false};
	uint32_t job_us_ = 0;
	uint32_t stack_unused_ = 0;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_WORKER_H
