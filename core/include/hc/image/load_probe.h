/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_LOAD_PROBE_H
#define HC_IMAGE_LOAD_PROBE_H

namespace hc {
namespace image {

/// Observes the phases of an image load (homecontrol #70, doc/image.md "Memory of a swap"): the
/// platform logs the heap at each one (the ESP: free and lowest free since the previous phase), the
/// host tests measure the peak of each. Called on the event loop's thread, synchronously; must not
/// allocate when it is to measure, nor call back into the image service.
class LoadProbe {
public:
	/// A load begins (before the signature check).
	virtual void load_begun() {}
	/// The phase `name` ended ("json", "datapoints", ..., "release"; doc/image.md lists them).
	virtual void load_phase(const char* name) = 0;
	/// The load ended (`ok`: the image runs; false: refused or failed, nothing more follows).
	virtual void load_ended(bool ok) { (void)ok; }

protected:
	~LoadProbe() = default;
};

} // namespace image
} // namespace hc

#endif // HC_IMAGE_LOAD_PROBE_H
