/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_LOAD_PROBE_H
#define HC_ESP_LOAD_PROBE_H

#include <cstdint>

#include "hc/image/load_probe.h"

namespace hc {
namespace esp {

/// The heap at each phase of an image load (homecontrol #70, doc/image.md "Memory of a swap"): one
/// line per phase with the free heap at its end (heap_caps_get_free_size) and the lowest free heap
/// during the phase (heap_caps_get_minimum_free_size over a local minimum the IDF restarts at each
/// phase, heap_caps_monitor_local_minimum_free_size_start), and the lowest since boot.
///
/// Logged under hc.image at debug level (`log level hc.image debug` on the console switches it on),
/// or at info level when CONFIG_HC_IMAGE_HEAP_PHASES is set (default off). When neither would print,
/// the probe does nothing. While a load is measured, heap_caps_get_minimum_free_size() answers the
/// phase's minimum; the probe stops the monitor when the load ends (before the load's summary line),
/// and the IDF then restores the minimum since boot, including the load's lows.
class EspLoadProbe : public image::LoadProbe {
public:
	void load_begun() override;
	void load_phase(const char* name) override;
	void load_ended(bool ok) override;

private:
	bool active_ = false;
	uint32_t lowest_ = 0;      ///< The lowest of the load so far.
	uint32_t since_boot_ = 0;  ///< The lowest since boot, before the load and with its phases.
	uint32_t at_begin_ = 0;    ///< Free when the load began.
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_LOAD_PROBE_H
