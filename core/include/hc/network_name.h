/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_NETWORK_NAME_H
#define HC_NETWORK_NAME_H

#include <cstddef>
#include <cstdint>

#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/network_settings.h"
#include "hc/platform/clock.h"
#include "hc/platform/name_service.h"
#include "hc/platform/settings.h"

namespace hc {

/// The controller's network name (doc/network.md "Network name", homecontrol #89): the DHCP host
/// name, the mDNS host name and the mDNS instance name. It is used for nothing else; the device's
/// identity (MQTT node id, Improv device name, `device.name`) stays homecontrol-<MAC>.
///
/// - **Picked once, then stored.** When the network first comes up and no name is stored
///   (setting::kNetHostname), the controller asks the LAN by mDNS for kDefaultNetHostname, then
///   kDefaultNetHostname-2, -3, ... (each lookup kLookupMs) and announces the first name nobody
///   answers for. kSettleMs later - probing may still have renamed it, when another host defended
///   it at the same moment - it stores the name the responder announces. From then on the stored
///   name is announced at every start and never picked again.
/// - **Changed only by the user** (begin_rename(): PUT /api/settings/net.hostname). The input is
///   normalized (normalize_network_name()); a name another host answers for is refused; a free
///   one is stored and announced at once (mDNS now, DHCP with the next request).
/// - A stored name that another host took while this controller was off: the responder's probing
///   renames it for this run (logged); the setting stays as the user chose it.
///
/// Runs on the event loop.
class NetworkName : public ConnectivityListener {
public:
	/// How long one lookup waits for an answer. mDNS responders answer a query for their unique A
	/// record within a few tens of milliseconds; the margin covers a sleeping WiFi station.
	static constexpr uint32_t kLookupMs = 2000;
	/// How often a running lookup is polled.
	static constexpr uint32_t kPollMs = 100;
	/// The time over kLookupMs after which a lookup that still has no outcome is given up (failed).
	static constexpr uint32_t kLookupGraceMs = 1000;
	/// After announcing a name: how long the responder's probing may still rename it before the
	/// announced name is read (and, for a picked name, stored).
	static constexpr uint32_t kSettleMs = 5000;
	/// Candidates asked for at the first pick: kDefaultNetHostname and -2 to -<kMaxCandidates>. When
	/// all are taken, the last is announced and probing renames it.
	static constexpr unsigned kMaxCandidates = 9;

	/// What begin_rename() did.
	enum class Begin : uint8_t {
		refused,  ///< Nothing changed; `code` and `error` say why.
		done,     ///< Stored and applied at once (no lookup was needed).
		checking, ///< The lookup runs; rename_finished() gives the outcome.
	};

	NetworkName(EventLoop& loop, const Clock& clock, Settings& settings, NameService& service);
	~NetworkName();

	NetworkName(const NetworkName&) = delete;
	NetworkName& operator=(const NetworkName&) = delete;

	/// Reads the stored name (name() is it from now on, or kDefaultNetHostname until one is picked)
	/// and registers with the connectivity. Call before the WiFi starts: name() is the first DHCP
	/// host name. False (logged) if the listener cannot be registered.
	bool start(Connectivity& connectivity);

	/// The stored name, or, before one is stored, the candidate being picked (kDefaultNetHostname
	/// before the first lookup).
	const char* name() const { return name_; }
	/// Whether name() is stored (setting::kNetHostname).
	bool stored() const { return stored_; }
	/// Whether the first pick runs (a lookup or the settle time before the store).
	bool picking() const { return !stored_ && (lookup_ == Lookup::pick || settling_); }
	/// The name the responder announces (it differs from name() when probing renamed it), name()
	/// before the first announcement.
	const char* announced() const;

	/// Starts the change to `input` (normalized: trimmed, lower-cased; normalize_network_name()). A
	/// name equal to the stored or the announced one is stored at once (done); another is looked up
	/// first (checking). Refusals: settings.invalid (the rules), settings.busy (the first pick or
	/// another check runs), settings.name_unchecked (the lookup cannot start), settings.storage_error.
	Begin begin_rename(const char* input, const char** code, const char** error);

	/// The outcome of the check begin_rename() started: false while it runs. True once it is over,
	/// with `ok` and, if not, `code` (settings.name_taken, settings.name_unchecked,
	/// settings.storage_error) and `error`; the outcome is handed out once (afterwards: true, not
	/// ok, code api.not_deferred).
	bool rename_finished(bool& ok, const char** code, const char** error);

	/// Called on the loop when a check begun by begin_rename() has its outcome (the E-Home connection
	/// sends its deferred answer then).
	void set_rename_listener(Callback callback) { rename_listener_ = callback; }

	void connectivity_changed(const Connectivity& connectivity, LinkState previous) override;

private:
	enum class Lookup : uint8_t { none, pick, rename };
	enum class Rename : uint8_t { idle, checking, finished };

	void start_pick_lookup();
	bool start_lookup(const char* name, Lookup kind, const char** reason);
	void poll();
	void pick_result(NameLookup result);
	void rename_result(NameLookup result);
	void finish_rename(bool ok, const char* code);
	bool commit_rename(const char** error);
	void apply();
	void settle_due();
	void stop_polling();
	void set_candidate(unsigned index);

	EventLoop& loop_;
	const Clock& clock_;
	Settings& settings_;
	NameService& service_;
	char name_[setting::kMaxNetHostnameLength + 1] = {};
	/// The name of a rename being checked.
	char target_[setting::kMaxNetHostnameLength + 1] = {};
	char rename_text_[96] = {};
	const char* rename_code_ = "";
	bool rename_ok_ = false;
	Rename rename_ = Rename::idle;
	Lookup lookup_ = Lookup::none;
	unsigned candidate_ = 1;
	uint64_t lookup_deadline_ = 0;
	bool stored_ = false;
	bool applied_ = false;
	bool settling_ = false;
	TimerHandle poll_timer_;
	TimerHandle settle_timer_;
	Callback rename_listener_{};
};

} // namespace hc

#endif // HC_NETWORK_NAME_H
