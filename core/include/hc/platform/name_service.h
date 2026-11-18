/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_NAME_SERVICE_H
#define HC_PLATFORM_NAME_SERVICE_H

#include <cstdint>

namespace hc {

/// The outcome of a NameService lookup.
enum class NameLookup : uint8_t {
	running, ///< No answer yet and the time is not up.
	free,    ///< Nobody answered for the name within the time.
	taken,   ///< Another host answered for the name (with an address that is not the controller's own).
	failed,  ///< The lookup broke off (the platform logged why).
};

/// The platform's link-local name service (mDNS) and the DHCP host name, behind the core's
/// NetworkName (doc/network.md "Network name"). The core decides which name to announce and when;
/// the platform asks the LAN and announces. Called on the event loop's thread.
class NameService {
public:
	/// Starts asking the LAN whether a host answers for `<name>.local`, for at most `timeout_ms`.
	/// An answer that carries `own_address` (the controller's address, "" while unknown) is the
	/// controller itself and does not take the name: a router that answers mDNS for the host names
	/// of its DHCP clients (the FRITZ!Box does) echoes the DHCP host name the controller sent before
	/// the name was picked. One lookup at a time. False with `reason` if it cannot start (mDNS cannot
	/// run, a lookup is still running).
	virtual bool start_lookup(const char* name, const char* own_address, uint32_t timeout_ms,
		const char** reason) = 0;

	/// The state of the lookup start_lookup() began. Once it answers free, taken or failed the lookup
	/// is over (its resources freed); without a lookup it answers failed.
	virtual NameLookup poll_lookup() = 0;

	/// Gives up the running lookup (no outcome is wanted any more).
	virtual void cancel_lookup() = 0;

	/// Announces `name` as the mDNS host name and instance name (starting the announcement and its
	/// services with the first call) and makes it the DHCP host name, carried by the next DHCP
	/// request. False with `reason` if mDNS could not announce it.
	virtual bool apply_name(const char* name, const char** reason) = 0;

	/// The host name the responder announces now: the name of the last apply_name(), or the name
	/// probing renamed it to when another host defended it (`name-2`, ...); "" before the first
	/// apply_name().
	virtual const char* announced_name() const = 0;

protected:
	~NameService() = default;
};

} // namespace hc

#endif // HC_PLATFORM_NAME_SERVICE_H
