/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_STORAGE_H
#define HC_ESP_STORAGE_H

#include <cstddef>

namespace hc {
namespace esp {

/// Where the LittleFS partition `storage` is mounted. Its layout (doc/persistence.md):
/// `/storage/state` the BlobStore's blobs (written by the device); `/storage/ui`, the web UI of an
/// older firmware, is removed by the web server at start (the UI is compiled into the firmware).
constexpr const char* kStorageMountPoint = "/storage";
constexpr const char* kStoragePartition = "storage";

/// Mounts the partition read-write at kStorageMountPoint, once: the first call mounts, every later
/// call returns the first call's result (the web server and the blob store both call it, in either
/// order; thread-safe). Never formats: a partition without a file system (storage.bin never
/// flashed) or a damaged one stays unmounted, so the device state is never wiped by accident. Logs the
/// outcome and the usage. Returns whether the partition is mounted.
bool storage_mount();

/// Whether storage_mount() succeeded (false before it was called).
bool storage_mounted();

/// Why the mount failed ("" if it did not fail or was not tried).
const char* storage_failure();

/// Size and use of the mounted file system in bytes; false if it is not mounted.
bool storage_usage(size_t& total, size_t& used);

} // namespace esp
} // namespace hc

#endif // HC_ESP_STORAGE_H
