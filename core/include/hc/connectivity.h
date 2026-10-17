/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_CONNECTIVITY_H
#define HC_CONNECTIVITY_H

#include <cstddef>
#include <cstdint>

#include "hc/backoff.h"
#include "hc/event_loop.h"
#include "hc/platform/clock.h"
#include "hc/platform/network_driver.h"

namespace hc {

/// The state of the network link.
enum class LinkState : uint8_t {
	down,
	connecting,
	up,
};

/// "down", "connecting", "up".
const char* link_state_name(LinkState state);

/// What the connection workflow is doing (doc/network.md has the diagram).
enum class NetworkMode : uint8_t {
	/// Connecting with the stored credentials, reconnecting with backoff.
	normal,
	/// No usable credentials: pairing, restarted after every failure, until a partner answers.
	unprovisioned,
	/// Pairing on request (button, console) while credentials are stored; on failure back to them.
	repairing,
	/// The stored credentials failed kFailsafeFailures times in a row: pairing phase of the failsafe
	/// alternation.
	failsafe_pairing,
	/// Failsafe alternation: retrying the stored credentials for a phase.
	failsafe_connecting,
};

/// "normal", "unprovisioned", "re-pairing", "failsafe (pairing)", "failsafe (stored credentials)".
const char* network_mode_name(NetworkMode mode);

class Connectivity;

/// Observes link state changes (the KNX adapter waits for "up", the ESP platform starts mDNS and
/// SNTP on it). Called on the event loop's thread, after the state has changed, in registration
/// order. A listener may read the Connectivity but must not change it (refused and logged).
class ConnectivityListener {
public:
	virtual void connectivity_changed(const Connectivity& connectivity, LinkState previous) = 0;

protected:
	~ConnectivityListener() = default;
};

/// The core's model of the network link and of the connection workflow around it (ported from the
/// phoneblock dongle's WiFi workflow, see doc/network.md):
///
/// - With stored credentials it connects, and reconnects after every failure or loss with the
///   delays of a Backoff (1 s, 2 s, 4 s, ... 60 s; reset when the link comes up). An attempt without
///   an outcome after kConnectTimeoutMs counts as a failure.
/// - Without credentials it pairs (WPS push-button on the ESP) and restarts pairing after every
///   failure or timeout: a device without credentials always listens for the router's button.
/// - request_pairing() (the button held, the console) pairs while credentials are stored; the
///   current link stays up until new credentials arrive; on failure it returns to the old ones.
/// - After kFailsafeFailures consecutive failures without the link coming up it enters the
///   failsafe: phases of kFailsafePhaseMs alternate between pairing (so a changed router can be
///   paired without a console) and retrying the stored credentials (so an outage heals by itself).
///   The credentials are never erased; the link coming up or a pairing success ends the failsafe.
/// - Every switch between connecting and pairing first stops the other activity and waits
///   kSettleMs before the next driver call; down reports during pairing and during that settle time
///   are not failures.
/// - pause_pairing() frees the station for a while (a WiFi scan: the driver refuses to scan while
///   WPS or a connection attempt owns the station): it stops pairing or the attempt in flight and
///   holds every step that comes due; resume_pairing() runs the held step after kSettleMs.
///
/// The platform feeds it through link_connecting(), link_up(), link_down(), pairing_succeeded() and
/// pairing_failed(); every transition is logged (tag "hc.net") with its reason.
///
/// Threading: every method is called on the event loop's thread.
class Connectivity {
public:
	static constexpr size_t kListenerCapacity = 8;

	/// An attempt without an outcome after this long counts as failed.
	static constexpr uint32_t kConnectTimeoutMs = 30000;

	/// A pairing without an outcome after this long counts as failed (WPS walk time is 120 s).
	static constexpr uint32_t kPairingTimeoutMs = 150000;

	/// Pause between stopping one activity (pairing, an attempt) and starting the next, so the
	/// platform's radio settles (the dongle's 500 ms grace period).
	static constexpr uint32_t kSettleMs = 500;

	/// Pause before pairing restarts after a failure or timeout.
	static constexpr uint32_t kPairingRestartMs = 1000;

	/// Consecutive failures with the stored credentials that start the failsafe. With the backoff
	/// they take 1+2+4+8+16+32 s of waiting plus the attempts themselves: at least about 75 s.
	static constexpr uint32_t kFailsafeFailures = 7;

	/// Length of each failsafe phase (one WPS walk time).
	static constexpr uint32_t kFailsafePhaseMs = 120000;

	/// A pause that is not resumed within this long resumes by itself (logged).
	static constexpr uint32_t kMaxPauseMs = 20000;

	/// Longest IP address text (an IPv6 address, INET6_ADDRSTRLEN) including the NUL.
	static constexpr size_t kMaxAddress = 46;

	/// Longest reason text including the NUL; longer reasons are truncated.
	static constexpr size_t kMaxReason = 64;

	Connectivity(EventLoop& loop, const Clock& clock, NetworkDriver& driver);
	~Connectivity();

	Connectivity(const Connectivity&) = delete;
	Connectivity& operator=(const Connectivity&) = delete;

	/// Connects with the stored credentials, or pairs without. A second call is refused (logged).
	void start();

	/// Starts over with the credentials stored now (new ones from the console, or none after they
	/// were cleared): ends pairing and the failsafe, resets the backoff, then connects (dropping
	/// the current link) or, without credentials, pairs.
	void request_connect(const char* reason);

	/// Pairs while keeping the current link. Refused (logged, false) while pairing is active.
	bool request_pairing(const char* trigger);

	/// Frees the station for `why` (a WiFi scan): stops pairing (an existing link stays up) or
	/// aborts an attempt that has not reached the link, and holds the next driver call that takes
	/// the station - a pairing start or a connect - until resume_pairing(). Requests made meanwhile
	/// (request_connect(), request_pairing()) are decided as usual and their step is held too; the
	/// latest decided step wins. Returns false (logged) if already paused. station_free_at() tells
	/// when the radio has settled. A pause longer than kMaxPauseMs resumes by itself.
	bool pause_pairing(const char* why);

	/// Ends the pause: the held step (pairing, a connect) runs after kSettleMs. Ignored (logged)
	/// when not paused.
	void resume_pairing(const char* why);

	/// Whether a pause_pairing() is in effect.
	bool paused() const { return paused_; }

	/// Monotonic time from which the station is free after pause_pairing() (now, or kSettleMs after
	/// the activity it stopped).
	uint64_t station_free_at() const { return station_free_at_; }

	/// The driver started connecting on its own (or confirms the attempt the core started).
	void link_connecting();

	/// The link is up with the address `ip` (text, at most kMaxAddress - 1 characters).
	void link_up(const char* ip);

	/// The link went down or the attempt failed, for `reason`.
	void link_down(const char* reason);

	/// Pairing delivered credentials, which the driver has stored.
	void pairing_succeeded(const char* detail);

	/// Pairing failed or timed out, for `reason`. Duplicate reports are ignored.
	void pairing_failed(const char* reason);

	/// Registers a listener. Returns false (logged) when kListenerCapacity are registered.
	bool add_listener(ConnectivityListener* listener);

	LinkState state() const { return state_; }
	bool is_up() const { return state_ == LinkState::up; }
	NetworkMode mode() const { return mode_; }

	/// Whether pairing runs or is about to (re)start (also while held by a pause): what the status
	/// LED shows as "pairing".
	bool pairing_active() const {
		return pairing_ || action_ == Action::start_pairing || held_ == Action::start_pairing;
	}

	/// The address while up, "" otherwise.
	const char* ip() const { return ip_; }

	/// Why the link is in its current state ("" if never set).
	const char* reason() const { return reason_; }

	/// Consecutive failures with the stored credentials since the link was last up.
	uint32_t failures() const { return failures_; }

	/// Monotonic time of the next scheduled driver call (connect or pairing start), or
	/// EventLoop::kNever.
	uint64_t next_action_at() const { return action_at_; }

	const Backoff& backoff() const { return backoff_; }

private:
	enum class Action : uint8_t {
		none,
		connect,
		start_pairing,
	};

	void evaluate(uint32_t delay_ms);
	void attempt();
	void begin_pairing(NetworkMode mode, uint32_t delay_ms);
	void enter_failsafe();
	void exit_failsafe();
	void fail(const char* reason);
	void end_pairing();
	void schedule(Action action, uint32_t delay_ms);
	void action_due();
	void timeout_due();
	void phase_due();
	void cancel_action();
	void cancel_timeout();
	void cancel_phase();
	void cancel_pause_timer();
	void pause_due();
	ConnectResult driver_connect();
	void driver_disconnect();
	bool driver_start_pairing();
	void driver_stop_pairing();
	bool refuse_reentry(const char* what);
	void set_mode(NetworkMode mode, const char* why);
	void set_state(LinkState state, const char* reason);

	EventLoop& loop_;
	const Clock& clock_;
	NetworkDriver& driver_;
	Backoff backoff_;
	LinkState state_ = LinkState::down;
	NetworkMode mode_ = NetworkMode::normal;
	Action action_ = Action::none;
	/// The step that came due (or was stopped) during a pause, run at resume.
	Action held_ = Action::none;
	char ip_[kMaxAddress] = {};
	char reason_[kMaxReason] = {};
	TimerHandle action_timer_;
	TimerHandle timeout_timer_;
	TimerHandle phase_timer_;
	TimerHandle pause_timer_;
	uint64_t station_free_at_ = 0;
	uint64_t action_at_ = EventLoop::kNever;
	uint32_t attempts_ = 0;
	uint32_t failures_ = 0;
	ConnectivityListener* listeners_[kListenerCapacity] = {};
	size_t listener_count_ = 0;
	bool started_ = false;
	bool pairing_ = false;
	bool paused_ = false;
	char pause_reason_[kMaxReason] = {};
	bool in_driver_ = false;
	bool notifying_ = false;
};

} // namespace hc

#endif // HC_CONNECTIVITY_H
