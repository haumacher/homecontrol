/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_DEMO_H
#define HC_DEMO_H

#include <cstdint>

#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/technology.h"

namespace hc {

/// A handful of simulated datapoints for developing the web UI without a bus (POSIX
/// `homecontrol --demo`, console `web demo` on the device). The technology "demo" owns the
/// switching datapoints and accepts every command; it reports a slowly changing temperature
/// every kReportMs and toggles a window contact every kWindowMs. Not a runtime image: the real
/// model comes with issue #8, and load() refuses to replace a non-empty model.
class DemoTechnology : public Technology {
public:
	static constexpr uint32_t kReportMs = 5000;
	static constexpr uint32_t kWindowMs = 30000;

	DemoTechnology(DatapointModel& model, EventLoop& loop, const Clock& clock);

	/// Loads the demo datapoints into an empty model, binds them and starts the simulation.
	/// Refused (logged) if the model holds datapoints already.
	bool load();

	const char* name() const override { return "demo"; }
	bool command(const Datapoint& datapoint, const Value& value) override;

private:
	void report();

	DatapointModel& model_;
	EventLoop& loop_;
	const Clock& clock_;
	uint32_t ticks_ = 0;
	bool loaded_ = false;
};

} // namespace hc

#endif // HC_DEMO_H
