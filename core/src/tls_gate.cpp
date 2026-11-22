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

void TlsGateClient::tls_gate_describe(char* out, size_t size) const {
	std::snprintf(out, size, "a TLS session");
}

TlsGate::TlsGate(EventLoop& loop, const Clock& clock, MemoryWatch* memory, Needs needs)
	: loop_(loop), clock_(clock), memory_(memory), needs_(needs) {}

TlsGate::~TlsGate() {
	loop_.cancel(rearm_);
	loop_.cancel(pause_timer_);
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
	last_free_ = room.free_bytes;
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
			if (&client == link_client_) {
				link_admitted();
			}
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
				if (pausing_) {
					++pause_sessions_;
				}
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
		try_pause(last_free_);
	}
	return false;
}

void TlsGate::release(TlsGateClient& client) {
	if (holder_ == &client) {
		holder_ = nullptr;
		if (&client == link_client_ && link_before_ != 0) {
			// The link's socket is open (or its attempt failed): what its session keeps of the memory.
			DmaRoom room;
			if (link_ != nullptr && link_->tls_link_open() && memory_ != nullptr && memory_->dma_room(room)) {
				link_footprint_ = link_before_ > room.free_bytes ? link_before_ - room.free_bytes : 0;
			}
			link_before_ = 0;
		}
		if (count_ > 0 || pausing_) {
			kick();
		}
		return;
	}
	if (pausing_) {
		kick(); // the pause may be over
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
	if (holder_ != nullptr) {
		return;
	}
	if (count_ == 0) {
		if (pausing_) {
			end_pause("nothing waits any more");
		}
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
			try_pause(last_free_);
			return;
		}
		++admitted_;
		if (pausing_) {
			++pause_sessions_;
		}
		if (chosen.memory) {
			++waited_for_memory_;
			Log::info(kTag, "a TLS session is admitted: the memory was freed");
		}
	}
	remove(next);
	holder_ = chosen.client;
	if (chosen.client == link_client_) {
		link_admitted();
	}
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

// ---- The pause (homecontrol #112) --------------------------------------------------------------

void TlsGate::link_admitted() {
	DmaRoom room;
	link_before_ = memory_ != nullptr && memory_->dma_room(room) ? room.free_bytes : 0;
}

bool TlsGate::session_waits() const {
	for (size_t i = 0; i < count_; ++i) {
		if (waiting_[i].kind == Kind::session) {
			return true;
		}
	}
	return false;
}

void TlsGate::link_tick() {
	if (pausing_ || link_ == nullptr || memory_ == nullptr || holder_ != nullptr) {
		return;
	}
	bool memory_waits = false;
	for (size_t i = 0; i < count_; ++i) {
		memory_waits = memory_waits || (waiting_[i].kind == Kind::session && waiting_[i].memory);
	}
	DmaRoom room;
	if (memory_waits && memory_->dma_room(room)) {
		try_pause(room.free_bytes);
	}
}

void TlsGate::try_pause(uint32_t free_bytes) {
	if (link_ == nullptr || pausing_ || memory_ == nullptr || link_footprint_ == 0 || !session_waits()) {
		return;
	}
	uint64_t now = clock_.monotonic_ms();
	uint32_t need = needs_.session_bytes + needs_.reserve_bytes;
	if (paused_before_ && now - pause_since_ms_ < kPauseIntervalMs) {
		return; // at most one pause per interval
	}
	if (static_cast<uint64_t>(free_bytes) + link_footprint_ < need) {
		return; // would not fit even without the link
	}
	if (!link_->tls_link_pausable()) {
		return; // the link's tick asks again
	}
	char names[96] = "";
	size_t n = 0;
	for (size_t i = 0; i < count_ && n + 1 < sizeof(names); ++i) {
		if (waiting_[i].kind != Kind::session) {
			continue;
		}
		char name[48];
		waiting_[i].client->tls_gate_describe(name, sizeof(name));
		int r = std::snprintf(names + n, sizeof(names) - n, "%s%s", n > 0 ? ", " : "", name);
		n += r > 0 ? static_cast<size_t>(r) : 0;
	}
	pausing_ = true;
	paused_before_ = true;
	pause_since_ms_ = now;
	pause_sessions_ = 0;
	++pauses_;
	Log::info(kTag, "pausing the E-Home link for %s: HP %u B free, the link holds %u B, %u B needed",
		names, static_cast<unsigned>(free_bytes), static_cast<unsigned>(link_footprint_), static_cast<unsigned>(need));
	loop_.cancel(pause_timer_);
	pause_timer_ = loop_.schedule_at(now + kPauseMaxMs, Callback::member<TlsGate, &TlsGate::pause_over>(this, "tls_gate.pause_over"));
	link_->tls_link_pause();
}

void TlsGate::pause_over() {
	pause_timer_ = TimerHandle();
	if (pausing_) {
		end_pause("the longest pause is over");
	}
}

void TlsGate::end_pause(const char* why) {
	pausing_ = false;
	loop_.cancel(pause_timer_);
	pause_timer_ = TimerHandle();
	uint64_t ms = clock_.monotonic_ms() - pause_since_ms_;
	Log::info(kTag, "the E-Home link reconnects after a pause of %u.%u s: %u TLS session(s) ran, %s",
		static_cast<unsigned>(ms / 1000), static_cast<unsigned>(ms % 1000 / 100), static_cast<unsigned>(pause_sessions_),
		why);
	if (link_ != nullptr) {
		link_->tls_link_resume();
	}
}

} // namespace hc
