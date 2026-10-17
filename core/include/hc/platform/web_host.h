/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_WEB_HOST_H
#define HC_PLATFORM_WEB_HOST_H

#include <cstddef>
#include <cstdint>

namespace hc {

class JsonWriter;

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

protected:
	~WebHost() = default;
};

} // namespace hc

#endif // HC_PLATFORM_WEB_HOST_H
