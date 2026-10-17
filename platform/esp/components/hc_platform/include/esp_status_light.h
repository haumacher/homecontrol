/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_STATUS_LIGHT_H
#define HC_ESP_STATUS_LIGHT_H

#include "hc/platform/status_light.h"

namespace hc {
namespace esp {

/// The board's status LED as configured in menuconfig ("HomeControl status LED"): none, a plain
/// LED on a GPIO (optionally active low) or one WS2812 pixel over RMT, lit in one dim colour.
class EspStatusLight : public StatusLight {
public:
	/// Sets up the hardware. Returns false (logged) if no LED is configured or it cannot be driven;
	/// the light must then not be handed to the Controller.
	bool init();

	void set(bool on) override;

private:
	void* strip_ = nullptr; ///< led_strip_handle_t of the WS2812 variant
	int gpio_ = -1;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_STATUS_LIGHT_H
