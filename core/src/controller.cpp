/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/controller.h"

#include <cstdio>
#include <cstring>

#include "hc/diagnostics.h"
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
	  diagnostics_(platform.diagnostics),
	  settings_(platform.settings),
	  time_zone_platform_(platform.time_zone),
	  status_light_(platform.status_light),
	  connectivity_(loop_, platform.clock, platform.network),
	  improv_(loop_, platform.clock, platform.settings, connectivity_, platform.improv, platform.scanner),
	  log_ring_(&platform.clock),
	  time_(loop_, platform.clock, platform.time_zone, platform.settings),
	  shutdown_(loop_, platform.clock) {
	Log::set_sink(&sink_);
	Log::set_ring(&log_ring_);
	loop_.set_clock(&clock_); // the loop measures its turns (the System page)
	time_.add_listener(*this, TimeEvents::kSynchronized);
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

	ResetReason reason = diagnostics_ != nullptr ? diagnostics_->reset_reason() : ResetReason::unknown;
	Log::write(reset_reason_is_crash(reason) || reason == ResetReason::brownout ? LogLevel::warn : LogLevel::info, kTag,
		"HomeControl %s (build %s) starting, reset reason: %s", kVersion, build_id(), reset_reason_name(reason));
	if (diagnostics_ != nullptr) {
		char facts[160];
		if (format_boot_facts(*diagnostics_, facts, sizeof(facts)) > 0) {
			Log::info(kTag, "boot: %s", facts);
		}
	}

	read_string_setting(settings_, setting::kTimeZone, setting::kDefaultTimeZone, time_zone_, sizeof(time_zone_),
		kTag, &posix_tz_valid);
	time_zone_platform_.apply(time_zone_);
	Log::info(kTag, "time zone %s", time_zone_);

	WallTime wall = clock_.wall_time();
	if (!wall.synchronized) {
		Log::info(kTag, "wall clock not synchronized (reads %lld s)", static_cast<long long>(wall.seconds));
	}
	// Watches the clock; tells time_changed() when it is (or becomes) synchronized.
	time_.start();

	MemoryInfo memory = system_.memory();
	Log::info(kTag, "free heap %u B (min %u B)", static_cast<unsigned>(memory.free_bytes),
		static_cast<unsigned>(memory.min_free_bytes));

	// The heartbeat reports a change of the network from what it is now (the start line names none:
	// Connectivity logs its own start).
	beat_state_ = connectivity_.state();
	beat_mode_ = connectivity_.mode();
	std::snprintf(beat_ip_, sizeof(beat_ip_), "%s", connectivity_.ip());
	loop_.schedule_periodic(start_ms_ + kHeartbeatPeriodMs, kHeartbeatPeriodMs,
		Callback::member<Controller, &Controller::heartbeat>(this, "controller.heartbeat"));

	bool quiet = false;
	SettingsStatus quiet_status = settings_.get_bool(setting::kLedQuiet, quiet);
	if (quiet_status != SettingsStatus::ok && quiet_status != SettingsStatus::not_found) {
		Log::error(kTag, "setting %s unreadable (%s): the status LED is not quiet", setting::kLedQuiet,
			settings_status_name(quiet_status));
		quiet = false;
	}
	status_led_.set_quiet(quiet);
	if (quiet) {
		Log::info(kTag, "status LED quiet: dark while ready");
	}

	if (status_light_ != nullptr) {
		status_light_->set(false);
		update_led();
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
	if (led_input_ == nullptr || !led_input_->image_loaded()) {
		return LedStatus::setup;
	}
	return led_input_->image_degraded() ? LedStatus::degraded : LedStatus::ready;
}

void Controller::update_led() {
	if (status_light_ == nullptr) {
		return;
	}
	LedStatus status = led_status();
	bool quiet = status_led_.quiet();
	if (led_shown_ && status == status_led_.status() && quiet == led_quiet_shown_) {
		return;
	}
	led_shown_ = true;
	led_quiet_shown_ = quiet;
	// A new pattern starts at its first tick, now; a blinking one gets its tick timer on the grid of
	// this moment, a steady one none (homecontrol #101: no tick while nothing blinks).
	status_led_.set_status(status);
	if (led_timer_.valid()) {
		loop_.cancel(led_timer_);
		led_timer_ = TimerHandle();
	}
	show_led(status_led_.tick());
	if (!status_led_.steady()) {
		led_timer_ = loop_.schedule_periodic(clock_.monotonic_ms() + StatusLed::kTickMs, StatusLed::kTickMs,
			Callback::member<Controller, &Controller::led_tick>(this, "controller.led_tick"));
	}
}

void Controller::led_tick() {
	show_led(status_led_.tick());
}

void Controller::show_led(bool on) {
	if (on != light_on_) {
		light_on_ = on;
		status_light_->set(on);
	}
}

bool Controller::set_led_quiet(bool quiet, const char** error) {
	SettingsStatus status = settings_.set_bool(setting::kLedQuiet, quiet);
	if (status != SettingsStatus::ok) {
		Log::error(kTag, "refusing %s %s: cannot store it (%s)", setting::kLedQuiet, quiet ? "on" : "off",
			settings_status_name(status));
		if (error != nullptr) {
			*error = settings_status_name(status);
		}
		return false;
	}
	status_led_.set_quiet(quiet);
	Log::info(kTag, "status LED %s", quiet ? "quiet: dark while ready" : "lit while ready");
	return true;
}

void Controller::run_once() {
	loop_.run_once(clock_.monotonic_ms());
	// The LED's status is a function of state that only the loop's work changes (the network, the
	// provisioning, the image): checked after every turn, it follows each change at once without a
	// tick. A message of another task (the ESP's LoopQueue) is followed by the next turn.
	if (started_) {
		update_led();
	}
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
	time_.refresh(); // the listeners learn about a changed UTC offset or date at once
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
	LinkState state = connectivity_.state();
	NetworkMode mode = connectivity_.mode();
	bool changed = state != beat_state_ || mode != beat_mode_ || std::strcmp(ip, beat_ip_) != 0;
	// A measured heap (free_bytes 0: the platform has no figure) whose minimum fell to a new low below
	// the low-water mark.
	bool low = memory.free_bytes != 0 && memory.min_free_bytes < kHeapLowWaterBytes &&
		(heap_low_warned_ == 0 || memory.min_free_bytes < heap_low_warned_);
	if (!changed && !low) {
		return; // steady state: nothing new for the log ring
	}
	bool normal = mode == NetworkMode::normal;
	char low_text[64] = "";
	if (low) {
		std::snprintf(low_text, sizeof(low_text), ": the minimum is a new low below %u B",
			static_cast<unsigned>(kHeapLowWaterBytes));
	}
	Log::write(low ? LogLevel::warn : LogLevel::info, kTag,
		"heartbeat: uptime %llu s, network %s%s%s%s%s, free heap %u B (min %u B)%s",
		static_cast<unsigned long long>(uptime_ms() / 1000), link_state_name(state), ip[0] != '\0' ? " " : "", ip,
		normal ? "" : ", ", normal ? "" : network_mode_name(mode), static_cast<unsigned>(memory.free_bytes),
		static_cast<unsigned>(memory.min_free_bytes), low_text);
	if (changed) {
		beat_state_ = state;
		beat_mode_ = mode;
		std::snprintf(beat_ip_, sizeof(beat_ip_), "%s", ip);
	}
	if (low) {
		heap_low_warned_ = memory.min_free_bytes;
	}
}

void Controller::time_changed(uint8_t events) {
	if ((events & TimeEvents::kSynchronized) != 0) {
		log_clock_synchronized();
	}
}

void Controller::log_clock_synchronized() {
	char now[kTimeText];
	format_wall_time(now, sizeof(now));
	Log::info(kTag, "wall clock synchronized: %s", now);
}

} // namespace hc
