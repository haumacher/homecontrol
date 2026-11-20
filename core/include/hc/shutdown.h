/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_SHUTDOWN_H
#define HC_SHUTDOWN_H

#include <cstddef>
#include <cstdint>

#include "hc/event_loop.h"
#include "hc/platform/clock.h"

namespace hc {

class Shutdown;

/// A component that ends a connection before a planned restart (homecontrol #103): the KNX tunnel
/// sends its DISCONNECT_REQUEST, so the gateway frees the tunnel slot at once instead of after its
/// heartbeat timeout.
class ShutdownParticipant {
public:
	/// For the log ("knx tunnel").
	virtual const char* shutdown_name() const = 0;

	/// Starts closing for the planned restart. The participant calls Shutdown::done() once it has
	/// finished (also from within this call, when there is nothing to close). It does not connect
	/// again afterwards.
	virtual void shutdown_begin(Shutdown& shutdown) = 0;

protected:
	~ShutdownParticipant() = default;
};

/// The planned restart's closing phase (homecontrol #103, doc/core.md "Planned restart"): every
/// restart the device decides on (the API's restart and factory reset, the console's `restart` and
/// `factory reset`, the switch to the OTA updater for an update or a firmware upload) first asks
/// the participants to close their connections, waits until each has finished or kMaxWaitMs has
/// passed, then runs the platform's restart. Best effort: a participant that does not finish in
/// time is named in the log and does not hold the restart up.
///
/// The core decides when to close, the platform restarts (esp_restart(), the process exit): `then`
/// is the platform's. Threading: every method is called on the event loop's thread.
class Shutdown {
public:
	static constexpr size_t kCapacity = 4;
	/// The longest the restart waits for the participants.
	static constexpr uint32_t kMaxWaitMs = 1000;

	Shutdown(EventLoop& loop, const Clock& clock);
	~Shutdown();

	Shutdown(const Shutdown&) = delete;
	Shutdown& operator=(const Shutdown&) = delete;

	/// Registers a participant; false (logged) when kCapacity are registered or it is already.
	bool add(ShutdownParticipant& participant);

	/// Starts the closing phase for `reason` ("restart over the API"): every participant's
	/// shutdown_begin(), then `then` once all have called done() or after kMaxWaitMs. `then` runs on
	/// the loop, inside the call that finished the last participant (possibly begin() itself) or in
	/// the timeout. Refused (false, logged) while a closing phase runs or after it ended: the restart
	/// is under way.
	bool begin(const char* reason, Callback then);

	/// A participant has finished closing. Ignored outside the closing phase and for a participant
	/// that finished already.
	void done(ShutdownParticipant& participant);

	/// Whether the closing phase runs (begin() called, `then` not yet).
	bool running() const { return running_; }
	/// Whether `then` has run.
	bool finished() const { return finished_; }

private:
	void timeout();
	void finish(bool timed_out);

	EventLoop& loop_;
	const Clock& clock_;
	ShutdownParticipant* participants_[kCapacity] = {};
	bool waiting_[kCapacity] = {};
	size_t count_ = 0;
	size_t outstanding_ = 0;
	bool running_ = false;
	bool finished_ = false;
	uint64_t started_ms_ = 0;
	Callback then_;
	TimerHandle timer_;
};

} // namespace hc

#endif // HC_SHUTDOWN_H
