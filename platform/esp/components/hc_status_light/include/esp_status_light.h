/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_STATUS_LIGHT_H
#define HC_ESP_STATUS_LIGHT_H

#include <cstdint>

#include "hc/platform/status_light.h"

namespace hc {
namespace esp {

/// The board's status LED as configured in menuconfig ("HomeControl status LED"): none, a plain
/// LED on a GPIO (optionally active low) or one WS2812 pixel over RMT. The application's light is
/// one dim colour (set(bool)); the OTA updater chooses a colour per state (set(bool, r, g, b)).
class EspStatusLight : public StatusLight {
public:
	/// Sets up the hardware. Returns false (logged) if no LED is configured or it cannot be driven;
	/// the light must then not be handed to the Controller.
	bool init();

	/// The application's light: the dim cyan of the WS2812 or the plain LED.
	void set(bool on) override;

	/// Lit in the colour (r, g, b; 0..255 each) on a WS2812; a plain GPIO LED ignores the colour.
	void set(bool on, uint8_t red, uint8_t green, uint8_t blue);

private:
	void* strip_ = nullptr; ///< led_strip_handle_t of the WS2812 variant
	int gpio_ = -1;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_STATUS_LIGHT_H
