/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_EVENT_LOOP_H
#define HC_EVENT_LOOP_H

#include <cstddef>
#include <cstdint>

namespace hc {

class Clock;

/// A function to run on the event loop: a plain function pointer plus an opaque context, so that
/// scheduling never allocates. Use Callback::member<T, &T::method>(object) for member functions.
struct Callback {
	void (*fn)(void* context) = nullptr;
	void* context = nullptr;

	/// A callback that invokes `(object->*Method)()`.
	template <class T, void (T::*Method)()>
	static Callback member(T* object) {
		return Callback{&invoke_member<T, Method>, object};
	}

	explicit operator bool() const { return fn != nullptr; }

	void operator()() const { fn(context); }

private:
	template <class T, void (T::*Method)()>
	static void invoke_member(void* object) {
		(static_cast<T*>(object)->*Method)();
	}
};

/// Identifies a scheduled timer. A default-constructed handle is invalid; so is the handle a
/// refused schedule call returns. Handles carry a generation, so a stale handle (its timer fired
/// or was cancelled and the slot reused) never cancels somebody else's timer.
class TimerHandle {
public:
	constexpr TimerHandle() = default;

	constexpr bool valid() const { return value_ != 0; }
	constexpr uint32_t value() const { return value_; }

	friend constexpr bool operator==(TimerHandle a, TimerHandle b) { return a.value_ == b.value_; }
	friend constexpr bool operator!=(TimerHandle a, TimerHandle b) { return a.value_ != b.value_; }

private:
	friend class EventLoop;
	constexpr explicit TimerHandle(uint32_t value) : value_(value) {}

	uint32_t value_ = 0;
};

/// The core's single-threaded event loop: timers on the monotonic time base and a FIFO of posted
/// callbacks. The core owns it, the platform drives it by calling run_once(now) and sleeping until
/// next_deadline(). See doc/core.md for the model.
///
/// Threading: every method must be called on the loop's thread (handlers may call all of them
/// except run_once). There is no cross-thread posting yet.
///
/// Ordering:
/// - A run_once(now) call processes exactly the work that was due when it was entered: first the
///   callbacks posted before entry, in FIFO order, then the timers with deadline <= now that were
///   scheduled before entry, by deadline and, for equal deadlines, by scheduling order.
/// - Work created by a handler (a post, a timer due at or before `now`) runs in the next
///   run_once call, never in the current one; next_deadline() then reports it as due, so the
///   platform calls again without sleeping. This bounds every run_once call.
/// - A timer cancelled before its handler ran does not run, even if it was due in this call.
///
/// Capacity: kTimerCapacity timers and kEventCapacity pending posts, statically allocated. When
/// either is exhausted the call is refused: it returns an invalid handle / false and logs an error.
class EventLoop {
public:
	/// Maximum number of timers scheduled at the same time (one-shot and periodic).
	static constexpr size_t kTimerCapacity = 32;

	/// Maximum number of posted callbacks waiting to run.
	static constexpr size_t kEventCapacity = 32;

	/// next_deadline() when nothing is scheduled.
	static constexpr uint64_t kNever = UINT64_MAX;

	EventLoop();
	EventLoop(const EventLoop&) = delete;
	EventLoop& operator=(const EventLoop&) = delete;

	/// Schedules a one-shot timer at the absolute monotonic time `deadline_ms`.
	TimerHandle schedule_at(uint64_t deadline_ms, Callback callback);

	/// Schedules a periodic timer: first at `first_deadline_ms`, then every `period_ms` after the
	/// previous deadline (no drift). If the loop falls behind by more than a period, the missed
	/// periods are skipped (logged as a warning) and the timer stays on its original grid. Every
	/// re-arm takes a new scheduling order: a timer scheduled earlier for the same deadline runs
	/// before the periodic timer's next tick.
	TimerHandle schedule_periodic(uint64_t first_deadline_ms, uint32_t period_ms, Callback callback);

	/// Cancels a timer. Returns true if the timer was pending (it will not run again); false for an
	/// invalid or stale handle (already fired one-shot, already cancelled). A periodic timer may
	/// cancel itself from within its handler.
	bool cancel(TimerHandle handle);

	/// Queues a callback to run on the loop in FIFO order. Returns false (and logs) if the queue
	/// is full or the callback is empty.
	bool post(Callback callback);

	/// Runs the work due at `now_ms` (see the ordering rules above). Must not be called from a
	/// handler (refused and logged).
	void run_once(uint64_t now_ms);

	/// The monotonic time at which run_once next has work: 0 if posted callbacks are waiting,
	/// the earliest timer deadline otherwise, kNever if nothing is scheduled. A value <= now means
	/// "call run_once immediately".
	uint64_t next_deadline() const;

	/// Number of scheduled timers (including a periodic timer whose handler is running).
	size_t timer_count() const;

	/// Number of posted callbacks waiting to run.
	size_t event_count() const { return event_count_; }

	// ---- Load figures since boot (the System page, spec/api.md "System diagnostics") ----

	/// The clock run_once() measures its turns with (the platform's monotonic clock; the Controller
	/// sets it). Without one the turns are not measured.
	void set_clock(const Clock* clock) { clock_ = clock; }

	/// Accounts a turn that ran from `start_ms` to `end_ms`: run_once() itself, or work the platform
	/// runs on the loop's thread outside it (messages from other tasks). Keeps the longest.
	void account_turn(uint64_t start_ms, uint64_t end_ms);

	/// Periods periodic timers skipped since boot because the loop was late (each skip is also
	/// logged as a warning).
	uint64_t missed_periods() const { return missed_periods_; }

	/// The longest turn since boot in ms, and when it began (monotonic ms; 0 while none was measured).
	uint64_t longest_turn_ms() const { return longest_turn_ms_; }
	uint64_t longest_turn_at_ms() const { return longest_turn_at_ms_; }

private:
	enum class SlotState : uint8_t {
		free,
		scheduled, ///< In the heap.
		due,       ///< Taken from the heap by the current run_once, handler not yet run.
		firing,    ///< Periodic timer whose handler is running.
	};

	struct TimerSlot {
		uint64_t deadline = 0;
		uint64_t sequence = 0;
		Callback callback;
		uint32_t period = 0; ///< 0 for one-shot timers.
		uint32_t generation = 1;
		uint8_t heap_index = 0;
		SlotState state = SlotState::free;
	};

	static constexpr unsigned kIndexBits = 8;
	static constexpr uint32_t kIndexMask = (1u << kIndexBits) - 1;
	static_assert(kTimerCapacity < (1u << kIndexBits), "timer slot index must fit the handle");
	static_assert(kTimerCapacity <= 255, "heap indices are stored in uint8_t");

	TimerHandle schedule(uint64_t deadline_ms, uint32_t period_ms, Callback callback);
	TimerSlot* resolve(TimerHandle handle);
	TimerHandle handle_of(uint8_t index) const;
	void release(uint8_t index);

	bool heap_less(uint8_t a, uint8_t b) const;
	void heap_set(size_t position, uint8_t index);
	void heap_push(uint8_t index);
	uint8_t heap_pop();
	void heap_remove(size_t position);
	void heap_sift_up(size_t position);
	void heap_sift_down(size_t position);

	void fire(uint8_t index, uint32_t generation, uint64_t now_ms);

	TimerSlot slots_[kTimerCapacity];
	uint8_t free_[kTimerCapacity];
	size_t free_count_ = 0;
	uint8_t heap_[kTimerCapacity];
	size_t heap_size_ = 0;
	uint64_t next_sequence_ = 0;

	Callback events_[kEventCapacity];
	size_t event_head_ = 0;
	size_t event_count_ = 0;

	bool running_ = false;

	const Clock* clock_ = nullptr;
	uint64_t missed_periods_ = 0;
	uint64_t longest_turn_ms_ = 0;
	uint64_t longest_turn_at_ms_ = 0;
};

} // namespace hc

#endif // HC_EVENT_LOOP_H
