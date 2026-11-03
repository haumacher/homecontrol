/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_worker.h"

#include "esp_timer.h"
#include "freertos/task.h"
#include "hc/log.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.worker";

} // namespace

bool EspWorker::run(Callback job, Callback done, const char** error) {
	if (busy_.load()) {
		*error = "a job runs already";
		return false;
	}
	job_ = job;
	done_ = done;
	busy_.store(true);
	if (xTaskCreate(&EspWorker::task, "hc_work", kStack, this, kPriority, nullptr) != pdPASS) {
		busy_.store(false);
		*error = "cannot create the worker task (memory)";
		return false;
	}
	return true;
}

void EspWorker::task(void* context) {
	EspWorker* self = static_cast<EspWorker*>(context);
	int64_t start = esp_timer_get_time();
	self->job_();
	self->job_us_ = static_cast<uint32_t>(esp_timer_get_time() - start);
	self->stack_unused_ = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
	// The last touch of `self` from this task: the loop runs `done` and may start the next job.
	while (!self->queue_.post(&EspWorker::finished, self, 0, nullptr, "worker job done")) {
		vTaskDelay(pdMS_TO_TICKS(20)); // the queue is full only for moments
	}
	vTaskDelete(nullptr);
}

void EspWorker::finished(void* context, const LoopMessage&) {
	EspWorker* self = static_cast<EspWorker*>(context);
	Log::info(kTag, "job done in %u ms; task stack %u of %u B unused", static_cast<unsigned>(self->job_us_ / 1000),
		static_cast<unsigned>(self->stack_unused_), static_cast<unsigned>(kStack));
	Callback done = self->done_;
	self->job_ = Callback();
	self->done_ = Callback();
	self->busy_.store(false);
	done();
}

} // namespace esp
} // namespace hc
