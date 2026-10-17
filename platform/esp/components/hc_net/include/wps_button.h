/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_WPS_BUTTON_H
#define HC_ESP_WPS_BUTTON_H

#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/hold_button.h"
#include "hc/platform/clock.h"

namespace hc {
namespace esp {

/// Asks the Connectivity to pair (WPS) when the button on CONFIG_HC_WPS_BUTTON_GPIO is held for
/// HoldButton::kDefaultHoldMs: the way to re-pair a device that has credentials (a device without
/// credentials pairs by itself).
/// The pin is polled every kPollPeriodMs by a timer on the event loop (no interrupt, nothing to
/// hand between tasks) and fed into the core's HoldButton, which debounces and measures the hold.
class WpsButton {
public:
	static constexpr uint32_t kPollPeriodMs = 20;

	WpsButton(EventLoop& loop, const Clock& clock, Connectivity& connectivity);

	/// Configures the pin and starts polling; logs and returns false if no button is configured or
	/// the pin is invalid. Call on the event loop, after boot.
	bool start();

private:
	void poll();

	EventLoop& loop_;
	const Clock& clock_;
	Connectivity& connectivity_;
	HoldButton button_;
	int gpio_ = -1;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_WPS_BUTTON_H
