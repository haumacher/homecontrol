/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_SYSTEM_INFO_H
#define HC_PLATFORM_SYSTEM_INFO_H

#include <cstdint>

#include "hc/platform/diagnostics.h"

namespace hc {

/// Free-memory figures as the platform reports them. A platform that cannot measure a figure
/// reports 0 for it and says so where it implements SystemInfo.
struct MemoryInfo {
	/// Bytes currently free on the heap.
	uint32_t free_bytes;

	/// The lowest value free_bytes has had since boot.
	uint32_t min_free_bytes;

	/// The largest block the heap can hand out now (fragmentation: an allocation larger than this
	/// fails although free_bytes would suffice). 0: unknown.
	uint32_t largest_block = 0;

	/// The heap's size. 0: unknown.
	uint32_t total_bytes = 0;

	/// The process's resident memory (a host process, whose heap grows on demand). 0: not applicable.
	uint64_t resident_bytes = 0;
};

/// A task's (thread's) scheduling state.
enum class TaskState : uint8_t { running, ready, blocked, suspended, deleted, unknown };

/// The state in words ("running", "ready", "blocked", "suspended", "deleted", "unknown"); stable,
/// the web API reports it.
const char* task_state_name(TaskState state);

/// One task of the platform (SystemInfo::visit_tasks()). The name is valid during the call only.
struct TaskInfo {
	const char* name = "";
	TaskState state = TaskState::unknown;
	/// The scheduler's priority (larger runs first); -1: the platform has none comparable.
	int32_t priority = -1;
	/// Bytes of its stack it never used since it started (the high-water mark); -1: unknown.
	int64_t stack_free = -1;
	/// Its share of the CPU time since it started, in permille of all cores; -1: not measured.
	int32_t cpu_permille = -1;
	/// The core it is pinned to; -1: any or unknown.
	int32_t core = -1;
	/// It is the calling task. The core calls on the event loop's thread, so this marks the loop.
	bool current = false;
};

/// Receives the tasks of SystemInfo::visit_tasks().
class TaskVisitor {
public:
	virtual void task(const TaskInfo& info) = 0;

protected:
	~TaskVisitor() = default;
};

/// A temperature reading (SystemInfo::temperature()).
struct Temperature {
	/// Degrees Celsius.
	double celsius = 0;
	/// The sensor's documented accuracy (plus/minus, degrees); 0: unknown.
	double accuracy = 0;
	/// Where it was read ("die sensor", "thermal_zone0").
	const char* source = "";
};

/// The console output path (SystemInfo::console_output(), issue #46): what the queue between the
/// printing tasks and the port's writer task holds and what it had to drop.
struct ConsoleOutputInfo {
	/// Lines dropped since boot because the queue was full (a reader too slow or none at all).
	uint64_t dropped = 0;
	/// Of those, the lines dropped while nobody read the port (the device: no USB host attached, or
	/// the port not opened, so the writer made no progress for a second).
	uint64_t dropped_unread = 0;
	/// Bytes queued now, and the queue's size.
	uint32_t queued = 0;
	uint32_t capacity = 0;
	/// A host is attached to the port now (the device: USB frames arrive).
	bool host = false;
};

/// Diagnostics the platform supplies to the core: the memory figures (also in the heartbeat and the
/// status), and what the System page shows (GET /api/system/diagnostics, spec/api.md): the tasks, the
/// chip's die temperature, the chip and firmware facts and the storage statistics. Every method but
/// memory() has a default that says "not available"; a platform overrides what it can measure.
///
/// Threading: called on the event loop's thread.
class SystemInfo {
public:
	virtual MemoryInfo memory() const = 0;

	/// Calls `visitor` for every task. False with the reason when the platform cannot list them.
	virtual bool visit_tasks(TaskVisitor& visitor, const char** reason) const {
		(void)visitor;
		*reason = "this platform does not list its tasks";
		return false;
	}

	/// Reads the die temperature (one reading per call). False with the reason when there is none.
	virtual bool temperature(Temperature& out, const char** reason) const {
		(void)out;
		*reason = "this platform has no temperature sensor";
		return false;
	}

	/// The chip and firmware facts in the platform's terms, keys stable because the web API reports
	/// them (spec/api.md "System diagnostics": `model`, `revision`, `cores`, `cpu_mhz`, `flash_size`,
	/// `flash_mode`, `idf`, `ota_state`, `os`). Default: none.
	virtual void visit_chip(DiagnosticsVisitor& visitor) const { (void)visitor; }

	/// Statistics of the platform's key-value store (the device's NVS: `used`, `free`, `total`
	/// entries, `namespaces`). False with the reason when there is none.
	virtual bool visit_settings_store(DiagnosticsVisitor& visitor, const char** reason) const {
		(void)visitor;
		*reason = "this platform keeps no statistics of its settings store";
		return false;
	}

	/// The console output queue's figures. False with the reason when the platform writes its
	/// console output directly (the host build: stdout).
	virtual bool console_output(ConsoleOutputInfo& out, const char** reason) const {
		(void)out;
		*reason = "this platform writes its console output directly, without a queue";
		return false;
	}

protected:
	~SystemInfo() = default;
};

} // namespace hc

#endif // HC_PLATFORM_SYSTEM_INFO_H
