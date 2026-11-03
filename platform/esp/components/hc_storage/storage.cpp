/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "storage.h"

#include <cstdio>

#include "esp_littlefs.h"
#include "hc/log.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.storage";

char g_failure[96] = "";
bool g_mounted = false;

bool mount() {
	esp_vfs_littlefs_conf_t conf = {};
	conf.base_path = kStorageMountPoint;
	conf.partition_label = kStoragePartition;
	// Never format: an unreadable partition means storage.bin was not flashed (or the partition is
	// damaged); the blob store refuses, instead of an empty file system hiding it.
	conf.format_if_mount_failed = false;
	conf.dont_mount = false;
	conf.read_only = false;
	esp_err_t err = esp_vfs_littlefs_register(&conf);
	if (err != ESP_OK) {
		std::snprintf(g_failure, sizeof(g_failure), "cannot mount the storage partition (%s); flash storage.bin",
			esp_err_to_name(err));
		Log::error(kTag, "%s (doc/persistence.md): values, history and the runtime image are not saved", g_failure);
		return false;
	}
	g_mounted = true;
	size_t total = 0;
	size_t used = 0;
	storage_usage(total, used);
	Log::info(kTag, "storage partition mounted at %s: %u of %u B used, %u B free", kStorageMountPoint,
		static_cast<unsigned>(used), static_cast<unsigned>(total), static_cast<unsigned>(total - used));
	return true;
}

} // namespace

bool storage_mount() {
	// A function-local static is initialized exactly once, thread-safe (the C++ runtime's guard).
	static const bool mounted = mount();
	return mounted;
}

bool storage_mounted() {
	return g_mounted;
}

const char* storage_failure() {
	return g_failure;
}

bool storage_usage(size_t& total, size_t& used) {
	total = 0;
	used = 0;
	if (!g_mounted) {
		return false;
	}
	return esp_littlefs_info(kStoragePartition, &total, &used) == ESP_OK;
}

} // namespace esp
} // namespace hc
