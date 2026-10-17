/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_SYSTEM_INFO_H
#define HC_PLATFORM_SYSTEM_INFO_H

#include <cstdint>

namespace hc {

/// Free-memory figures as the platform reports them. A platform that cannot measure reports 0
/// and says so where it implements SystemInfo.
struct MemoryInfo {
	/// Bytes currently free on the heap.
	uint32_t free_bytes;

	/// The lowest value free_bytes has had since boot.
	uint32_t min_free_bytes;
};

/// Diagnostics the platform supplies to the core.
class SystemInfo {
public:
	virtual MemoryInfo memory() const = 0;

protected:
	~SystemInfo() = default;
};

} // namespace hc

#endif // HC_PLATFORM_SYSTEM_INFO_H
