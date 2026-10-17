/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/controller.h"

#include "hc/version.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.ctl";

} // namespace

Controller::Controller(const Clock& clock, LogSink& sink, const SystemInfo& system)
	: clock_(clock), sink_(sink), system_(system) {
	Log::set_sink(&sink_);
}

Controller::~Controller() {
	if (Log::sink() == &sink_) {
		Log::set_sink(nullptr);
	}
}

void Controller::start() {
	if (started_) {
		Log::error(kTag, "refusing start: controller already started");
		return;
	}
	started_ = true;
	start_ms_ = clock_.monotonic_ms();

	Log::info(kTag, "HomeControl %s (build %s) starting", kVersion, build_id());
	WallTime wall = clock_.wall_time();
	if (!wall.synchronized) {
		Log::info(kTag, "wall clock not synchronized (reads %lld s)", static_cast<long long>(wall.seconds));
	}
	MemoryInfo memory = system_.memory();
	Log::info(kTag, "free heap %u B (min %u B)", static_cast<unsigned>(memory.free_bytes),
		static_cast<unsigned>(memory.min_free_bytes));

	loop_.schedule_periodic(start_ms_ + kHeartbeatPeriodMs, kHeartbeatPeriodMs,
		Callback::member<Controller, &Controller::heartbeat>(this));
}

void Controller::run_once() {
	loop_.run_once(clock_.monotonic_ms());
}

uint64_t Controller::uptime_ms() const {
	return started_ ? clock_.monotonic_ms() - start_ms_ : 0;
}

void Controller::heartbeat() {
	MemoryInfo memory = system_.memory();
	Log::info(kTag, "heartbeat: uptime %llu s, free heap %u B (min %u B)",
		static_cast<unsigned long long>(uptime_ms() / 1000), static_cast<unsigned>(memory.free_bytes),
		static_cast<unsigned>(memory.min_free_bytes));
}

} // namespace hc
