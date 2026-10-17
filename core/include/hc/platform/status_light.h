/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_STATUS_LIGHT_H
#define HC_PLATFORM_STATUS_LIGHT_H

namespace hc {

/// The board's status LED (a plain GPIO LED or one RGB pixel on the ESP, none on POSIX). The core
/// decides the blink pattern (hc::StatusLed) and switches it on and off; the platform only drives
/// the hardware.
class StatusLight {
public:
	/// Switches the light; called on the event loop, only when the level changes.
	virtual void set(bool on) = 0;

protected:
	~StatusLight() = default;
};

} // namespace hc

#endif // HC_PLATFORM_STATUS_LIGHT_H
