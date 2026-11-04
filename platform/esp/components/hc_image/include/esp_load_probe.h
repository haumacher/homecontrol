/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_LOAD_PROBE_H
#define HC_ESP_LOAD_PROBE_H

#include <cstddef>
#include <cstdint>

#include "esp_timer.h"
#include "hc/image/load_probe.h"
#include "loop_queue.h"
#include "sdkconfig.h"

namespace hc {
namespace esp {

/// The heap at each phase of an image load (homecontrol #70, doc/image.md "Memory of a swap"): one
/// line per phase with the free heap at its end (heap_caps_get_free_size), the lowest free heap
/// during the phase (heap_caps_get_minimum_free_size over a local minimum the IDF restarts at each
/// phase, heap_caps_monitor_local_minimum_free_size_start) and the largest free block at its end
/// (heap_caps_get_largest_free_block: what one contiguous allocation can get); then, 3 s and 10 s
/// after the load, the free heap, the lowest since boot and the largest block (what the store, the
/// answer and the tasks the load held back took; the timer posts them to the loop).
///
/// With CONFIG_HC_IMAGE_HEAP_BY_TASK (which turns on the IDF's heap task tracking) the lines at the
/// end of the load and after it add, per task, how much more it holds than when the load began:
/// whatever the load's own allocations do not explain (network buffers of tasks the load held back).
///
/// Logged under hc.image at debug level (`log level hc.image debug` on the console switches it on),
/// or at info level when CONFIG_HC_IMAGE_HEAP_PHASES is set (default off). When neither would print,
/// the probe does nothing. While a load is measured, heap_caps_get_minimum_free_size() answers the
/// phase's minimum; the probe stops the monitor when the load ends (before the load's summary line),
/// and the IDF then restores the minimum since boot, including the load's lows.
class EspLoadProbe : public image::LoadProbe {
public:
	/// The lines after the load are posted to `queue` (they run on the loop, like the load).
	explicit EspLoadProbe(LoopQueue& queue);
	~EspLoadProbe();

	void load_begun() override;
	void load_phase(const char* name) override;
	void load_ended(bool ok) override;

private:
	static void after(void* self);
	static void after_on_loop(void* self, const LoopMessage& message);
	void log_tasks(const char* when);

	LoopQueue& queue_;
	bool active_ = false;
	uint32_t lowest_ = 0;      ///< The lowest of the load so far.
	uint32_t since_boot_ = 0;  ///< The lowest since boot, before the load and with its phases.
	uint32_t at_begin_ = 0;    ///< Free when the load began.
	esp_timer_handle_t timer_ = nullptr;
	int after_step_ = 0;       ///< 0: none pending; 1: the 3 s line next; 2: the 10 s line next.
#if defined(CONFIG_HC_IMAGE_HEAP_BY_TASK)
	static constexpr size_t kTasks = 32;
	struct TaskHeap {
		void* task;
		uint32_t bytes;
	};
	/// Each task's heap when the load began (static: the probe allocates nothing while it measures).
	TaskHeap begin_[kTasks] = {};
	size_t begin_count_ = 0;
	void snapshot(TaskHeap* out, size_t& count);
#endif
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_LOAD_PROBE_H
