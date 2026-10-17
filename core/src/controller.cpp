/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/controller.h"

#include <cstdio>

#include "hc/network_settings.h"
#include "hc/tz.h"
#include "hc/version.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.ctl";

/// Length of a formatted local time ("2026-10-17 14:03:12 CEST (UTC+02:00)") with room to spare.
constexpr size_t kTimeText = 64;

} // namespace

Controller::Controller(const Platform& platform)
	: clock_(platform.clock),
	  sink_(platform.log),
	  system_(platform.system),
	  settings_(platform.settings),
	  time_zone_platform_(platform.time_zone),
	  status_light_(platform.status_light),
	  connectivity_(loop_, platform.clock, platform.network),
	  improv_(loop_, platform.clock, platform.settings, connectivity_, platform.improv, platform.scanner),
	  log_ring_(&platform.clock) {
	Log::set_sink(&sink_);
	Log::set_ring(&log_ring_);
}

Controller::~Controller() {
	if (Log::sink() == &sink_) {
		Log::set_sink(nullptr);
	}
	if (Log::ring() == &log_ring_) {
		Log::set_ring(nullptr);
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

	read_string_setting(settings_, setting::kTimeZone, setting::kDefaultTimeZone, time_zone_, sizeof(time_zone_),
		kTag, &posix_tz_valid);
	time_zone_platform_.apply(time_zone_);
	Log::info(kTag, "time zone %s", time_zone_);

	WallTime wall = clock_.wall_time();
	if (wall.synchronized) {
		log_clock_synchronized();
	} else {
		Log::info(kTag, "wall clock not synchronized (reads %lld s)", static_cast<long long>(wall.seconds));
		clock_watch_ = loop_.schedule_periodic(start_ms_ + kClockWatchPeriodMs, kClockWatchPeriodMs,
			Callback::member<Controller, &Controller::watch_clock>(this));
	}

	MemoryInfo memory = system_.memory();
	Log::info(kTag, "free heap %u B (min %u B)", static_cast<unsigned>(memory.free_bytes),
		static_cast<unsigned>(memory.min_free_bytes));

	loop_.schedule_periodic(start_ms_ + kHeartbeatPeriodMs, kHeartbeatPeriodMs,
		Callback::member<Controller, &Controller::heartbeat>(this));

	if (status_light_ != nullptr) {
		status_light_->set(false);
		loop_.schedule_periodic(start_ms_, StatusLed::kTickMs, Callback::member<Controller, &Controller::led_tick>(this));
	}

	improv_.start();
	connectivity_.start();
}

LedStatus Controller::led_status() const {
	if (connectivity_.pairing_active() || improv_.provisioning()) {
		return LedStatus::pairing;
	}
	if (!connectivity_.is_up()) {
		return LedStatus::connecting;
	}
	return LedStatus::setup;
}

void Controller::led_tick() {
	status_led_.set_status(led_status());
	bool on = status_led_.tick();
	if (on != light_on_) {
		light_on_ = on;
		status_light_->set(on);
	}
}

void Controller::run_once() {
	loop_.run_once(clock_.monotonic_ms());
}

uint64_t Controller::uptime_ms() const {
	return started_ ? clock_.monotonic_ms() - start_ms_ : 0;
}

bool Controller::set_time_zone(const char* tz, const char** error) {
	const char* reason = nullptr;
	if (!posix_tz_valid(tz, &reason)) {
		Log::error(kTag, "refusing time zone \"%s\": %s", tz == nullptr ? "" : tz, reason);
		if (error != nullptr) {
			*error = reason;
		}
		return false;
	}
	SettingsStatus status = settings_.set_string(setting::kTimeZone, tz);
	if (status != SettingsStatus::ok) {
		Log::error(kTag, "refusing time zone \"%s\": cannot store it (%s)", tz, settings_status_name(status));
		if (error != nullptr) {
			*error = settings_status_name(status);
		}
		return false;
	}
	std::snprintf(time_zone_, sizeof(time_zone_), "%s", tz);
	time_zone_platform_.apply(time_zone_);
	char now[kTimeText];
	bool synchronized = format_wall_time(now, sizeof(now));
	Log::info(kTag, "time zone set to %s, local time %s%s", time_zone_, now,
		synchronized ? "" : " (clock not synchronized)");
	return true;
}

bool Controller::format_wall_time(char* buffer, size_t size) const {
	WallTime wall = clock_.wall_time();
	LocalTime local;
	if (time_zone_platform_.to_local(wall.seconds, local)) {
		format_local_time(local, wall.seconds, buffer, size);
	} else {
		std::snprintf(buffer, size, "%lld s since the epoch (no local time)", static_cast<long long>(wall.seconds));
	}
	return wall.synchronized;
}

void Controller::heartbeat() {
	MemoryInfo memory = system_.memory();
	const char* ip = connectivity_.ip();
	bool normal = connectivity_.mode() == NetworkMode::normal;
	Log::info(kTag, "heartbeat: uptime %llu s, network %s%s%s%s%s, free heap %u B (min %u B)",
		static_cast<unsigned long long>(uptime_ms() / 1000), link_state_name(connectivity_.state()),
		ip[0] != '\0' ? " " : "", ip, normal ? "" : ", ", normal ? "" : network_mode_name(connectivity_.mode()),
		static_cast<unsigned>(memory.free_bytes),
		static_cast<unsigned>(memory.min_free_bytes));
}

void Controller::watch_clock() {
	if (!clock_.wall_time().synchronized) {
		return;
	}
	loop_.cancel(clock_watch_);
	clock_watch_ = TimerHandle();
	log_clock_synchronized();
}

void Controller::log_clock_synchronized() {
	char now[kTimeText];
	format_wall_time(now, sizeof(now));
	Log::info(kTag, "wall clock synchronized: %s", now);
}

} // namespace hc
