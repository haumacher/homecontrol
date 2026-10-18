/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_WEB_HOST_H
#define HC_PLATFORM_WEB_HOST_H

#include <cstddef>
#include <cstdint>

#include "hc/log.h"

namespace hc {

class JsonWriter;

/// What a platform hook of the web API answers (WebHost): done, not possible on this platform (the
/// API answers 501 system.unavailable with the reason), or refused this time (the reason says why).
enum class HostResult : uint8_t { ok, unavailable, refused };

/// The system actions the API defers until its answer is on the way (WebHost::system_action()).
enum class SystemAction : uint8_t { restart, factory_reset };

/// What the web API (hc::WebApi) needs from the platform beyond hc::Platform: the device's
/// network name, the services that live in platform components (mDNS, SNTP, the OTA update) and
/// the web UI image. Called on the event loop's thread.
class WebHost {
public:
	/// The network name ("homecontrol-a1b2c3"); its mDNS host name is "<name>.local".
	virtual const char* device_name() const = 0;

	/// Whether the mDNS announcement is up (false on platforms without mDNS).
	virtual bool mdns_announced() const = 0;

	/// The SNTP server in use, "" if SNTP is not running (or the platform has none).
	virtual const char* ntp_server() const = 0;

	/// ntp.server was changed: restart SNTP if it runs.
	virtual void ntp_server_changed() = 0;

	/// Validates and stores the update source (update.url); "" erases it. On refusal returns false
	/// with `error` pointing to the reason (doc/ota.md has the rules).
	virtual bool set_update_url(const char* url, const char** error) = 0;

	/// Writes the members of the "update" object of /api/status (spec/api.md) into an open object:
	/// at least "available" (whether the platform updates itself) and, where known, "partition",
	/// "source", "last" and "next_check_s".
	virtual void write_update_state(JsonWriter& json) const = 0;

	/// The content hash of the web UI image ("" if none is mounted).
	virtual const char* ui_version() const = 0;

	/// The TCP port of the WebSocket endpoint /ws: 0 when it is the port of the HTTP server (the
	/// ESP build), otherwise its own port (the POSIX build listens on the HTTP port + 1).
	virtual uint16_t websocket_port() const = 0;

	// ---- The console's system commands over the network (issue #38). Each has a default that says
	// the platform cannot do it; a platform overrides what it can. ----

	/// Whether the platform can carry out `action` (ok), and if not, why (`reason`).
	virtual HostResult system_action_possible(SystemAction action, const char** reason) const {
		(void)action;
		*reason = "this platform cannot restart or reset itself";
		return HostResult::unavailable;
	}

	/// Carries out `action` (restart; factory reset: erase the settings and the stored state, then
	/// restart). The API calls it on the loop a moment after it answered the request, once
	/// system_action_possible() said ok. On the device it does not return.
	virtual void system_action(SystemAction action) { (void)action; }

	/// Starts an update check (`force` false, the console's `update check`) or an installation
	/// whatever the version (`force` true, `update now`) from `url` or, when nullptr, update.url. The
	/// outcome follows in the log and the "update" status. ok, unavailable (no updater) or refused
	/// (a check runs, no source) with `reason`.
	virtual HostResult start_update(bool force, const char* url, const char** reason) {
		(void)force;
		(void)url;
		*reason = "this platform does not update itself (no OTA updater)";
		return HostResult::unavailable;
	}

	/// Erases the stored core dump (the console's `coredump clear`). ok, unavailable, or refused
	/// (the erase failed) with `reason`.
	virtual HostResult clear_coredump(const char** reason) {
		*reason = "this platform keeps no core dump";
		return HostResult::unavailable;
	}

	/// Sets the level of `tag` in the platform's own log as well ("*" for all; the device's IDF log,
	/// where tags outside "hc." live). False when the platform has no log of its own: then only the
	/// core's hc.* tags can be set.
	virtual bool set_platform_log_level(const char* tag, LogLevel level) {
		(void)tag;
		(void)level;
		return false;
	}
	/// Whether set_platform_log_level() takes tags outside "hc.".
	virtual bool has_platform_log() const { return false; }

	/// The size and the used bytes of the storage partition (the blob store's file system). False
	/// when the platform does not know them.
	virtual bool storage_usage(uint64_t& total, uint64_t& used) const {
		(void)total;
		(void)used;
		return false;
	}

protected:
	~WebHost() = default;
};

} // namespace hc

#endif // HC_PLATFORM_WEB_HOST_H
