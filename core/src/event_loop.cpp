/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/event_loop.h"

#include "hc/log.h"
#include "hc/platform/clock.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.loop";

} // namespace

EventLoop::EventLoop() {
	// Hand out low slot indices first (purely cosmetic: predictable handles in logs).
	for (size_t i = 0; i < kTimerCapacity; ++i) {
		free_[i] = static_cast<uint8_t>(kTimerCapacity - 1 - i);
	}
	free_count_ = kTimerCapacity;
}

TimerHandle EventLoop::schedule_at(uint64_t deadline_ms, Callback callback) {
	return schedule(deadline_ms, 0, callback);
}

TimerHandle EventLoop::schedule_periodic(uint64_t first_deadline_ms, uint32_t period_ms, Callback callback) {
	if (period_ms == 0) {
		Log::error(kTag, "refusing periodic timer with period 0");
		return TimerHandle();
	}
	return schedule(first_deadline_ms, period_ms, callback);
}

TimerHandle EventLoop::schedule(uint64_t deadline_ms, uint32_t period_ms, Callback callback) {
	if (!callback) {
		Log::error(kTag, "refusing timer without callback");
		return TimerHandle();
	}
	if (free_count_ == 0) {
		Log::error(kTag, "timer pool exhausted (capacity %u), refusing timer at %llu ms",
			static_cast<unsigned>(kTimerCapacity), static_cast<unsigned long long>(deadline_ms));
		return TimerHandle();
	}
	uint8_t index = free_[--free_count_];
	TimerSlot& slot = slots_[index];
	slot.deadline = deadline_ms;
	slot.sequence = next_sequence_++;
	slot.callback = callback;
	slot.period = period_ms;
	heap_push(index);
	return handle_of(index);
}

bool EventLoop::cancel(TimerHandle handle) {
	TimerSlot* slot = resolve(handle);
	if (slot == nullptr) {
		Log::debug(kTag, "cancel: timer %08x is not pending", static_cast<unsigned>(handle.value()));
		return false;
	}
	uint8_t index = static_cast<uint8_t>(slot - slots_);
	if (slot->state == SlotState::scheduled) {
		heap_remove(slot->heap_index);
	}
	// A due slot is skipped by run_once because its generation changes; a firing slot is not
	// rescheduled for the same reason.
	release(index);
	return true;
}

bool EventLoop::post(Callback callback) {
	if (!callback) {
		Log::error(kTag, "refusing post without callback");
		return false;
	}
	if (event_count_ == kEventCapacity) {
		Log::error(kTag, "event queue full (capacity %u), refusing post",
			static_cast<unsigned>(kEventCapacity));
		return false;
	}
	events_[(event_head_ + event_count_) % kEventCapacity] = callback;
	++event_count_;
	return true;
}

void EventLoop::run_once(uint64_t now_ms) {
	if (running_) {
		Log::error(kTag, "refusing nested run_once from within a handler");
		return;
	}
	running_ = true;
	// The turn is measured on the clock, not from `now_ms`: the platform may pass a time it read
	// earlier, and a handler's time is what counts.
	uint64_t started_ms = clock_ != nullptr ? clock_->monotonic_ms() : 0;

	// Snapshot the due timers before running anything, so that timers scheduled by handlers wait
	// for the next call. Popping yields (deadline, sequence) order.
	uint8_t due_index[kTimerCapacity];
	uint32_t due_generation[kTimerCapacity];
	size_t due_count = 0;
	while (heap_size_ > 0 && slots_[heap_[0]].deadline <= now_ms) {
		uint8_t index = heap_pop();
		slots_[index].state = SlotState::due;
		due_index[due_count] = index;
		due_generation[due_count] = slots_[index].generation;
		++due_count;
	}

	// Posted callbacks queued before entry, FIFO.
	size_t events = event_count_;
	for (size_t i = 0; i < events; ++i) {
		Callback callback = events_[event_head_];
		event_head_ = (event_head_ + 1) % kEventCapacity;
		--event_count_;
		callback();
	}

	for (size_t i = 0; i < due_count; ++i) {
		fire(due_index[i], due_generation[i], now_ms);
	}

	running_ = false;
	if (clock_ != nullptr) {
		account_turn(started_ms, clock_->monotonic_ms());
	}
}

void EventLoop::account_turn(uint64_t start_ms, uint64_t end_ms) {
	uint64_t length = end_ms > start_ms ? end_ms - start_ms : 0;
	if (length > longest_turn_ms_) {
		longest_turn_ms_ = length;
		longest_turn_at_ms_ = start_ms;
	}
}

void EventLoop::fire(uint8_t index, uint32_t generation, uint64_t now_ms) {
	TimerSlot& slot = slots_[index];
	if (slot.generation != generation || slot.state != SlotState::due) {
		return; // Cancelled by an earlier handler.
	}
	Callback callback = slot.callback;
	if (slot.period == 0) {
		release(index);
		callback();
		return;
	}

	slot.state = SlotState::firing;
	callback();
	if (slot.generation != generation || slot.state != SlotState::firing) {
		return; // Cancelled from within its handler (the slot may already be reused).
	}

	uint64_t next = slot.deadline + slot.period;
	if (next <= now_ms) {
		uint64_t missed = (now_ms - slot.deadline) / slot.period;
		next = slot.deadline + (missed + 1) * slot.period;
		missed_periods_ += missed;
		Log::warn(kTag, "periodic timer %08x (period %u ms) missed %llu period(s), next at %llu ms",
			static_cast<unsigned>(handle_of(index).value()), static_cast<unsigned>(slot.period),
			static_cast<unsigned long long>(missed), static_cast<unsigned long long>(next));
	}
	slot.deadline = next;
	slot.sequence = next_sequence_++;
	heap_push(index);
}

uint64_t EventLoop::next_deadline() const {
	if (event_count_ > 0) {
		return 0;
	}
	if (heap_size_ == 0) {
		return kNever;
	}
	return slots_[heap_[0]].deadline;
}

size_t EventLoop::timer_count() const {
	return kTimerCapacity - free_count_;
}

EventLoop::TimerSlot* EventLoop::resolve(TimerHandle handle) {
	if (!handle.valid()) {
		return nullptr;
	}
	uint32_t index = (handle.value() & kIndexMask) - 1;
	if (index >= kTimerCapacity) {
		return nullptr;
	}
	TimerSlot& slot = slots_[index];
	if (slot.state == SlotState::free || slot.generation != (handle.value() >> kIndexBits)) {
		return nullptr;
	}
	return &slot;
}

TimerHandle EventLoop::handle_of(uint8_t index) const {
	return TimerHandle((slots_[index].generation << kIndexBits) | (static_cast<uint32_t>(index) + 1));
}

void EventLoop::release(uint8_t index) {
	TimerSlot& slot = slots_[index];
	slot.state = SlotState::free;
	slot.callback = Callback();
	// Generations live in the upper 24 bits of a handle; skip 0 so a handle is never 0.
	slot.generation = (slot.generation + 1) & (UINT32_MAX >> kIndexBits);
	if (slot.generation == 0) {
		slot.generation = 1;
	}
	free_[free_count_++] = index;
}

bool EventLoop::heap_less(uint8_t a, uint8_t b) const {
	const TimerSlot& x = slots_[a];
	const TimerSlot& y = slots_[b];
	if (x.deadline != y.deadline) {
		return x.deadline < y.deadline;
	}
	return x.sequence < y.sequence;
}

void EventLoop::heap_set(size_t position, uint8_t index) {
	heap_[position] = index;
	slots_[index].heap_index = static_cast<uint8_t>(position);
}

void EventLoop::heap_push(uint8_t index) {
	slots_[index].state = SlotState::scheduled;
	heap_set(heap_size_, index);
	++heap_size_;
	heap_sift_up(heap_size_ - 1);
}

uint8_t EventLoop::heap_pop() {
	uint8_t top = heap_[0];
	heap_remove(0);
	return top;
}

void EventLoop::heap_remove(size_t position) {
	--heap_size_;
	if (position != heap_size_) {
		uint8_t moved = heap_[heap_size_];
		heap_set(position, moved);
		// The moved element goes either up or down, never both.
		heap_sift_up(position);
		heap_sift_down(slots_[moved].heap_index);
	}
}

void EventLoop::heap_sift_up(size_t position) {
	while (position > 0) {
		size_t parent = (position - 1) / 2;
		if (!heap_less(heap_[position], heap_[parent])) {
			break;
		}
		uint8_t moved = heap_[parent];
		heap_set(parent, heap_[position]);
		heap_set(position, moved);
		position = parent;
	}
}

void EventLoop::heap_sift_down(size_t position) {
	for (;;) {
		size_t left = 2 * position + 1;
		if (left >= heap_size_) {
			break;
		}
		size_t smallest = left;
		size_t right = left + 1;
		if (right < heap_size_ && heap_less(heap_[right], heap_[left])) {
			smallest = right;
		}
		if (!heap_less(heap_[smallest], heap_[position])) {
			break;
		}
		uint8_t moved = heap_[smallest];
		heap_set(smallest, heap_[position]);
		heap_set(position, moved);
		position = smallest;
	}
}

} // namespace hc
