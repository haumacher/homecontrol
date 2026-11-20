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
/// name, the services that live in platform components (mDNS, SNTP, the OTA update) and
/// the web UI image. Called on the event loop's thread.
class WebHost {
public:
	/// The device name ("homecontrol-a1b2c3"): its identity (`device.name`, the factory reset's
	/// confirmation, the Zigbee export), not its network name (hc::NetworkName,
	/// WebApi::set_network_name()). A platform without a network name answers to "<name>.local".
	virtual const char* device_name() const = 0;

	/// Whether the mDNS announcement is up (false on platforms without mDNS).
	virtual bool mdns_announced() const = 0;

	/// The SNTP server in use, "" if SNTP is not running (or the platform has none).
	virtual const char* ntp_server() const = 0;

	/// ntp.server was changed: restart SNTP if it runs.
	virtual void ntp_server_changed() = 0;

	/// Writes the members of the "update" object of /api/status (spec/api.md) into an open object:
	/// at least "available" (whether the platform updates itself) and, where known, "partition",
	/// "source", "last" and "next_check_s".
	virtual void write_update_state(JsonWriter& json) const = 0;

	/// Whether the platform updates itself from an update channel and so offers the setting
	/// update.channel. False where the system's package manager updates the controller (the Linux
	/// package, doc/linux.md): GET /api/settings leaves the setting out and PUT refuses it.
	virtual bool has_update_channel() const { return true; }

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
	/// system_action_possible() said ok. A planned restart: the platform closes the connections first
	/// (Controller::shutdown(), homecontrol #103), so the restart follows within Shutdown::kMaxWaitMs.
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

	/// Prepares a firmware upload from the browser (doc/ota.md "Upload from the browser"): checks that
	/// the device can take one (an updater that serves uploads, the application in ota_0, no update
	/// under way), stores the upload instruction with `token` (hc::store_upload_instruction) and makes
	/// the updater the boot partition. It does not restart: the API restarts the device a moment
	/// after its answer (system_action(SystemAction::restart)). ok, unavailable (no updater) or
	/// refused with `reason`; a refusal leaves no instruction behind.
	virtual HostResult start_upload(const char* token, const char** reason) {
		(void)token;
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

protected:
	~WebHost() = default;
};

} // namespace hc

#endif // HC_PLATFORM_WEB_HOST_H
