/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_WPS_BUTTON_H
#define HC_ESP_WPS_BUTTON_H

#include <atomic>

#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/hold_button.h"
#include "hc/platform/clock.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// Asks the Connectivity to pair (WPS) when the button on CONFIG_HC_WPS_BUTTON_GPIO is held for
/// HoldButton::kDefaultHoldMs: the way to re-pair a device that has credentials (a device without
/// credentials pairs by itself).
///
/// By interrupt, not by polling (homecontrol #101): the pin interrupts on both edges; the handler
/// only switches the pin's interrupt off and posts the edge to the event loop (LoopQueue's ISR path).
/// On the loop the core's HoldButton waits out the bounce (a one-shot timer of its debounce time,
/// after which it reads the level and the interrupt is switched on again) and times the hold (a
/// one-shot timer of 3 s, cancelled by the release). So at most one interrupt per debounce time
/// reaches the CPU, even from a noisy pin, and while nobody touches the button no timer runs.
class WpsButton : private ButtonPin {
public:
	WpsButton(EventLoop& loop, const Clock& clock, LoopQueue& queue, Connectivity& connectivity);

	/// Configures the pin and its interrupt; logs and returns false if no button is configured or
	/// the pin is invalid. Call on the event loop, after boot.
	bool start();

private:
	// ButtonPin
	bool pressed() const override;

	static void isr(void* self);
	static void on_edge(void* self, const LoopMessage& message);
	void held();
	/// Due right after the button's settle timer: switches the interrupt on again, and takes a change
	/// of the level since the settle as a new edge.
	void rearm();

	EventLoop& loop_;
	const Clock& clock_;
	LoopQueue& queue_;
	Connectivity& connectivity_;
	HoldButton button_;
	int gpio_ = -1;
	/// An edge is on its way to the loop or settling; the interrupt is off meanwhile.
	std::atomic<bool> busy_{false};
	TimerHandle rearm_;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_WPS_BUTTON_H
