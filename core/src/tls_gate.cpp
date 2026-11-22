/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/tls_gate.h"

#include <cstdio>

#include "hc/log.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.tls";

} // namespace

TlsGate::TlsGate(EventLoop& loop, const Clock& clock, MemoryWatch* memory, Needs needs)
	: loop_(loop), clock_(clock), memory_(memory), needs_(needs) {}

TlsGate::~TlsGate() {
	loop_.cancel(rearm_);
	if (memory_ != nullptr && notify_armed_) {
		memory_->cancel_notify();
	}
}

bool TlsGate::waits(const TlsGateClient& client) const {
	for (size_t i = 0; i < count_; ++i) {
		if (waiting_[i].client == &client) {
			return true;
		}
	}
	return false;
}

bool TlsGate::room_for_session() {
	reason_[0] = '\0';
	block_short_ = false;
	DmaRoom room;
	if (memory_ == nullptr || !memory_->dma_room(room)) {
		return true;
	}
	uint32_t need = needs_.session_bytes + needs_.reserve_bytes;
	if (room.free_bytes >= need && room.largest_block >= needs_.block_bytes) {
		return true;
	}
	std::snprintf(reason_, sizeof(reason_), "HP %u B free of %u B, largest block %u B of %u B",
		static_cast<unsigned>(room.free_bytes), static_cast<unsigned>(need), static_cast<unsigned>(room.largest_block),
		static_cast<unsigned>(needs_.block_bytes));
	block_short_ = room.free_bytes >= need;
	return false;
}

bool TlsGate::acquire(TlsGateClient& client, Kind kind) {
	if (holder_ == &client) {
		return true;
	}
	if (waits(client)) {
		return false;
	}
	bool memory_short = false;
	if (holder_ == nullptr) {
		if (kind == Kind::link) {
			holder_ = &client;
			return true;
		}
		bool link_waits = false;
		for (size_t i = 0; i < count_; ++i) {
			link_waits = link_waits || waiting_[i].kind == Kind::link;
		}
		if (count_ == 0 && !link_waits) {
			if (room_for_session()) {
				holder_ = &client;
				++admitted_;
				return true;
			}
			memory_short = true;
		}
	}
	if (count_ >= kMaxWaiting) {
		Log::warn(kTag, "more than %u TLS clients wait: refused, the client retries on its own",
			static_cast<unsigned>(kMaxWaiting));
		return false;
	}
	waiting_[count_++] = Waiter{&client, kind, ++sequence_, memory_short};
	if (memory_short) {
		Log::info(kTag, "a TLS session waits for memory: %s", reason_);
		ask_for_memory();
	}
	return false;
}

void TlsGate::release(TlsGateClient& client) {
	if (holder_ == &client) {
		holder_ = nullptr;
		if (count_ > 0) {
			kick();
		}
		return;
	}
	for (size_t i = 0; i < count_; ++i) {
		if (waiting_[i].client == &client) {
			remove(i);
			break;
		}
	}
	if (count_ == 0) {
		reason_[0] = '\0';
		loop_.cancel(rearm_);
		rearm_ = TimerHandle();
		if (memory_ != nullptr && notify_armed_) {
			memory_->cancel_notify();
			notify_armed_ = false;
		}
	}
}

void TlsGate::remove(size_t index) {
	for (size_t i = index; i + 1 < count_; ++i) {
		waiting_[i] = waiting_[i + 1];
	}
	--count_;
	waiting_[count_] = Waiter{};
}

void TlsGate::kick() {
	if (kicked_) {
		return;
	}
	kicked_ = loop_.post(Callback::member<TlsGate, &TlsGate::evaluate>(this, "tls_gate.evaluate"));
}

void TlsGate::evaluate() {
	kicked_ = false;
	if (holder_ != nullptr || count_ == 0) {
		return;
	}
	size_t next = count_;
	for (size_t i = 0; i < count_; ++i) {
		const Waiter& w = waiting_[i];
		if (next == count_ || (w.kind == Kind::link && waiting_[next].kind != Kind::link) ||
			(w.kind == waiting_[next].kind && w.sequence < waiting_[next].sequence)) {
			next = i;
		}
	}
	Waiter chosen = waiting_[next];
	if (chosen.kind == Kind::session) {
		if (!room_for_session()) {
			if (!chosen.memory) {
				waiting_[next].memory = true;
				Log::info(kTag, "a TLS session waits for memory: %s", reason_);
			}
			ask_for_memory();
			return;
		}
		++admitted_;
		if (chosen.memory) {
			++waited_for_memory_;
			Log::info(kTag, "a TLS session is admitted: the memory was freed");
		}
	}
	remove(next);
	holder_ = chosen.client;
	reason_[0] = '\0';
	if (count_ == 0 && memory_ != nullptr && notify_armed_) {
		memory_->cancel_notify();
		notify_armed_ = false;
	}
	chosen.client->tls_gate_opened();
}

void TlsGate::ask_for_memory() {
	if (memory_ == nullptr) {
		return;
	}
	if (block_short_) {
		// Free memory enough, the block too small: a notification would come with the next free.
		if (!rearm_.valid()) {
			rearm_ = loop_.schedule_at(clock_.monotonic_ms() + kBlockRetryMs,
				Callback::member<TlsGate, &TlsGate::rearm>(this, "tls_gate.rearm"));
		}
		return;
	}
	memory_->notify_at(needs_.session_bytes + needs_.reserve_bytes, *this);
	notify_armed_ = true;
}

void TlsGate::rearm() {
	rearm_ = TimerHandle();
	if (holder_ == nullptr && count_ > 0 && memory_ != nullptr) {
		memory_->notify_at(needs_.session_bytes + needs_.reserve_bytes, *this);
		notify_armed_ = true;
	}
}

void TlsGate::memory_freed() {
	notify_armed_ = false;
	if (holder_ == nullptr && count_ > 0) {
		kick();
	}
}

} // namespace hc
