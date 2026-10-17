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
#include "hc/log_ring.h"
#include "hc/status_led.h"
#include "hc/time_service.h"
#include "hc/platform/clock.h"
#include "hc/platform/diagnostics.h"
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
	/// The reset reason and the platform's boot facts (doc/diagnostics.md); nullptr: none (the
	/// reason is then reported as unknown).
	const Diagnostics* diagnostics = nullptr;
};

/// What the status LED needs to know about the runtime image beyond the network (the image
/// service, issue #8): whether one is loaded and whether something it needs is failing.
class LedInput {
public:
	/// A runtime image is loaded.
	virtual bool image_loaded() const = 0;
	/// The image is loaded but something it needs is failing (the KNX tunnel is down while the image
	/// has KNX datapoints).
	virtual bool image_degraded() const = 0;

protected:
	~LedInput() = default;
};

/// The object a platform boots. It owns the event loop, the network Connectivity and the Improv
/// Wi-Fi provisioning service (active only if the platform has an Improv port); the platform
/// supplies its capabilities (Platform), calls start() once and then drives the loop with
/// run_once() and next_deadline().
///
/// At start it logs the version with the reset reason and the platform's boot facts (Diagnostics), applies the time zone setting, logs the wall clock state and
/// starts connecting. Its TimeService watches the wall clock; the controller logs the local time
/// once it is synchronized, and logs a heartbeat every kHeartbeatPeriodMs with the uptime, the network state and the heap.
/// With a status light it drives the LED pattern (hc::StatusLed) every StatusLed::kTickMs from
/// led_status().
class Controller : private TimeListener {
public:
	static constexpr uint32_t kHeartbeatPeriodMs = 10000;

	/// How often an unsynchronized wall clock is checked for synchronization (the TimeService's watch).
	static constexpr uint32_t kClockWatchPeriodMs = TimeService::kWatchPeriodMs;

	/// Longest time zone string including the NUL.
	static constexpr size_t kMaxTimeZone = Settings::kMaxStringLength + 1;

	/// Installs the platform's log sink and its log ring (for the lifetime of the controller).
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
	/// exchange waits for the link, connecting without a link; with the link up setup without a
	/// runtime image, degraded when the image's needs fail (LedInput), ready otherwise.
	LedStatus led_status() const;

	/// The source of the image part of led_status() (nullptr: none, the LED shows setup when up).
	void set_led_input(const LedInput* input) { led_input_ = input; }

	EventLoop& loop() { return loop_; }
	Connectivity& connectivity() { return connectivity_; }
	const Connectivity& connectivity() const { return connectivity_; }
	Improv& improv() { return improv_; }
	const Improv& improv() const { return improv_; }
	const Clock& clock() const { return clock_; }
	/// The time service: wall clock, local time, astronomy and time events (doc/time.md).
	TimeService& time() { return time_; }
	const TimeService& time() const { return time_; }
	Settings& settings() { return settings_; }
	const SystemInfo& system() const { return system_; }
	/// The platform's diagnostics (nullptr if it has none); hc::write_boot_status() reports them.
	const Diagnostics* diagnostics() const { return diagnostics_; }
	TimeZone& time_zone_platform() { return time_zone_platform_; }
	/// The lines logged since the controller was constructed (GET /api/log, doc/web.md).
	const LogRing& log_ring() const { return log_ring_; }

private:
	void heartbeat();
	void time_changed(uint8_t events) override;
	void log_clock_synchronized();
	void led_tick();

	const Clock& clock_;
	LogSink& sink_;
	const SystemInfo& system_;
	const Diagnostics* diagnostics_;
	Settings& settings_;
	TimeZone& time_zone_platform_;
	StatusLight* status_light_;
	StatusLed status_led_;
	bool light_on_ = false;
	EventLoop loop_;
	Connectivity connectivity_;
	Improv improv_;
	LogRing log_ring_;
	TimeService time_;
	char time_zone_[kMaxTimeZone] = {};
	uint64_t start_ms_ = 0;
	bool started_ = false;
	const LedInput* led_input_ = nullptr;
};

} // namespace hc

#endif // HC_CONTROLLER_H
