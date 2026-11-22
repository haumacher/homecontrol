/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/shutdown.h"

#include "hc/log.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.shutdown";

} // namespace

Shutdown::Shutdown(EventLoop& loop, const Clock& clock) : loop_(loop), clock_(clock) {}

Shutdown::~Shutdown() {
	loop_.cancel(timer_);
}

bool Shutdown::add(ShutdownParticipant& participant) {
	for (size_t i = 0; i < count_; ++i) {
		if (participants_[i] == &participant) {
			Log::error(kTag, "refusing the shutdown participant %s: registered already", participant.shutdown_name());
			return false;
		}
	}
	if (count_ == kCapacity) {
		Log::error(kTag, "refusing the shutdown participant %s: capacity of %u exhausted", participant.shutdown_name(),
			static_cast<unsigned>(kCapacity));
		return false;
	}
	participants_[count_++] = &participant;
	return true;
}

bool Shutdown::begin(const char* reason, Callback then) {
	if (running_ || finished_) {
		Log::warn(kTag, "%s refused: a restart is already under way", reason);
		return false;
	}
	running_ = true;
	then_ = then;
	started_ms_ = clock_.monotonic_ms();
	outstanding_ = count_;
	for (size_t i = 0; i < count_; ++i) {
		waiting_[i] = true;
	}
	Log::info(kTag, "%s: closing the connections first (at most %u ms)", reason, static_cast<unsigned>(kMaxWaitMs));
	if (outstanding_ > 0) {
		timer_ = loop_.schedule_at(started_ms_ + kMaxWaitMs, Callback::member<Shutdown, &Shutdown::timeout>(this, "shutdown.timeout"));
	}
	// A participant may finish within its shutdown_begin(); the last one then runs `then`.
	for (size_t i = 0; i < count_ && running_; ++i) {
		participants_[i]->shutdown_begin(*this);
	}
	if (running_ && outstanding_ == 0) {
		finish(false);
	}
	return true;
}

void Shutdown::done(ShutdownParticipant& participant) {
	if (!running_) {
		return;
	}
	for (size_t i = 0; i < count_; ++i) {
		if (participants_[i] == &participant && waiting_[i]) {
			waiting_[i] = false;
			--outstanding_;
			Log::info(kTag, "%s closed after %u ms", participant.shutdown_name(),
				static_cast<unsigned>(clock_.monotonic_ms() - started_ms_));
		}
	}
	if (outstanding_ == 0) {
		finish(false);
	}
}

void Shutdown::timeout() {
	timer_ = TimerHandle();
	if (running_) {
		finish(true);
	}
}

void Shutdown::finish(bool timed_out) {
	if (timed_out) {
		for (size_t i = 0; i < count_; ++i) {
			if (waiting_[i]) {
				Log::warn(kTag, "%s did not close within %u ms: restarting anyway", participants_[i]->shutdown_name(),
					static_cast<unsigned>(kMaxWaitMs));
			}
		}
	}
	loop_.cancel(timer_);
	timer_ = TimerHandle();
	running_ = false;
	finished_ = true;
	Callback then = then_;
	then_ = Callback();
	if (then) {
		then();
	}
}

} // namespace hc
