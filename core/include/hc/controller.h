/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_CONTROLLER_H
#define HC_CONTROLLER_H

#include <cstdint>

#include "hc/event_loop.h"
#include "hc/log.h"
#include "hc/platform/clock.h"
#include "hc/platform/system_info.h"

namespace hc {

/// The object a platform boots. It owns the event loop; the platform supplies the clock, the log
/// sink and the system diagnostics, calls start() once and then drives the loop with run_once()
/// and next_deadline().
///
/// For now it only logs the version at start and a heartbeat every kHeartbeatPeriodMs.
class Controller {
public:
	static constexpr uint32_t kHeartbeatPeriodMs = 10000;

	/// Installs `sink` as the log sink (for the lifetime of the controller).
	Controller(const Clock& clock, LogSink& sink, const SystemInfo& system);
	~Controller();

	Controller(const Controller&) = delete;
	Controller& operator=(const Controller&) = delete;

	/// Logs the version and schedules the heartbeat. A second call is refused (logged).
	void start();

	bool started() const { return started_; }

	/// Runs the loop's work due at the clock's current monotonic time.
	void run_once();

	/// See EventLoop::next_deadline().
	uint64_t next_deadline() const { return loop_.next_deadline(); }

	/// Milliseconds since start() (0 before).
	uint64_t uptime_ms() const;

	EventLoop& loop() { return loop_; }
	const Clock& clock() const { return clock_; }

private:
	void heartbeat();

	const Clock& clock_;
	LogSink& sink_;
	const SystemInfo& system_;
	EventLoop loop_;
	uint64_t start_ms_ = 0;
	bool started_ = false;
};

} // namespace hc

#endif // HC_CONTROLLER_H
