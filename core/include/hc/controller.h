/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_CONTROLLER_H
#define HC_CONTROLLER_H

#include <cstddef>
#include <cstdint>

#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/improv.h"
#include "hc/log.h"
#include "hc/status_led.h"
#include "hc/platform/clock.h"
#include "hc/platform/improv_port.h"
#include "hc/platform/network_driver.h"
#include "hc/platform/settings.h"
#include "hc/platform/status_light.h"
#include "hc/platform/system_info.h"
#include "hc/platform/time_zone.h"
#include "hc/platform/wifi_scanner.h"

namespace hc {

/// Everything a platform hands to the Controller. The objects must outlive it.
struct Platform {
	const Clock& clock;
	LogSink& log;
	const SystemInfo& system;
	Settings& settings;
	TimeZone& time_zone;
	NetworkDriver& network;
	/// The status LED; nullptr if the board has none.
	StatusLight* status_light = nullptr;
	/// The serial port for Improv Wi-Fi provisioning; nullptr if the platform offers none.
	ImprovPort* improv = nullptr;
	/// The WiFi scan Improv lists networks with; nullptr if the platform has none.
	WifiScanner* scanner = nullptr;
};

/// The object a platform boots. It owns the event loop, the network Connectivity and the Improv
/// Wi-Fi provisioning service (active only if the platform has an Improv port); the platform
/// supplies its capabilities (Platform), calls start() once and then drives the loop with
/// run_once() and next_deadline().
///
/// At start it logs the version, applies the time zone setting, logs the wall clock state and
/// starts connecting. It watches the wall clock and logs the local time once it is synchronized,
/// and logs a heartbeat every kHeartbeatPeriodMs with the uptime, the network state and the heap.
/// With a status light it drives the LED pattern (hc::StatusLed) every StatusLed::kTickMs from
/// led_status().
class Controller {
public:
	static constexpr uint32_t kHeartbeatPeriodMs = 10000;

	/// How often an unsynchronized wall clock is checked for synchronization.
	static constexpr uint32_t kClockWatchPeriodMs = 1000;

	/// Longest time zone string including the NUL.
	static constexpr size_t kMaxTimeZone = Settings::kMaxStringLength + 1;

	/// Installs the platform's log sink (for the lifetime of the controller).
	explicit Controller(const Platform& platform);
	~Controller();

	Controller(const Controller&) = delete;
	Controller& operator=(const Controller&) = delete;

	/// Logs the version, applies the time zone, schedules the heartbeat and the clock watch, and
	/// starts the network. A second call is refused (logged).
	void start();

	bool started() const { return started_; }

	/// Runs the loop's work due at the clock's current monotonic time.
	void run_once();

	/// See EventLoop::next_deadline().
	uint64_t next_deadline() const { return loop_.next_deadline(); }

	/// Milliseconds since start() (0 before).
	uint64_t uptime_ms() const;

	/// Validates, stores and applies a POSIX TZ string. On refusal returns false, logs and points
	/// `error` (if given) to the reason.
	bool set_time_zone(const char* tz, const char** error = nullptr);

	/// The POSIX TZ string in effect.
	const char* time_zone() const { return time_zone_; }

	/// Writes the wall time as local time ("2026-10-17 14:03:12 CEST (UTC+02:00)") into `buffer`;
	/// returns whether the wall clock is synchronized.
	bool format_wall_time(char* buffer, size_t size) const;

	/// What the status LED shows now: pairing while pairing is active or an Improv `wifi settings`
	/// exchange waits for the link, connecting without a link, setup while up (no runtime image can
	/// be loaded yet, issue #8).
	LedStatus led_status() const;

	EventLoop& loop() { return loop_; }
	Connectivity& connectivity() { return connectivity_; }
	const Connectivity& connectivity() const { return connectivity_; }
	Improv& improv() { return improv_; }
	const Improv& improv() const { return improv_; }
	const Clock& clock() const { return clock_; }
	Settings& settings() { return settings_; }
	const SystemInfo& system() const { return system_; }

private:
	void heartbeat();
	void watch_clock();
	void log_clock_synchronized();
	void led_tick();

	const Clock& clock_;
	LogSink& sink_;
	const SystemInfo& system_;
	Settings& settings_;
	TimeZone& time_zone_platform_;
	StatusLight* status_light_;
	StatusLed status_led_;
	bool light_on_ = false;
	EventLoop loop_;
	Connectivity connectivity_;
	Improv improv_;
	TimerHandle clock_watch_;
	char time_zone_[kMaxTimeZone] = {};
	uint64_t start_ms_ = 0;
	bool started_ = false;
};

} // namespace hc

#endif // HC_CONTROLLER_H
