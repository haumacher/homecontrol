/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_TLS_GATE_H
#define HC_TLS_GATE_H

#include <cstddef>
#include <cstdint>

#include "hc/event_loop.h"
#include "hc/platform/clock.h"
#include "hc/platform/memory_watch.h"

namespace hc {

/// Admitted by the TlsGate (on the event loop, never from within acquire() or release()).
class TlsGateClient {
public:
	virtual void tls_gate_opened() = 0;

	/// What waits, for the log's pause line ("language, update"); default "a TLS session".
	virtual void tls_gate_describe(char* out, size_t size) const;

protected:
	~TlsGateClient() = default;
};

/// The E-Home link as the gate pauses it (TlsGate::set_link(), homecontrol #112).
class TlsGateLink {
public:
	/// The link's socket is open (after its handshake, while the session lasts).
	virtual bool tls_link_open() const = 0;
	/// A pause would lose nothing now: connected, no request, streamed answer, bus session or offered
	/// key in progress.
	virtual bool tls_link_pausable() const = 0;
	/// Closes the session for the pause (close code 4000, "fetch") and waits for tls_link_resume().
	virtual void tls_link_pause() = 0;
	/// Connects again at once (no backoff: the close was planned).
	virtual void tls_link_resume() = 0;

protected:
	~TlsGateLink() = default;
};

/// One TLS session's start at a time, and a second session only when the memory DMA reaches can
/// carry it (homecontrol #110/#112, doc/core.md "The TLS gate").
///
/// On the house controller the language-pack fetch's TLS session started beside the E-Home session
/// and a browser's WebSocket and took the HP SRAM down to 140 B: the WiFi driver's frame copies
/// (DMA) failed, frames were lost, and the fetch failed too. The gate orders the TLS clients:
///
/// - Kind::link, the E-Home link: admitted whenever no session holds the gate, ahead of every
///   waiting session, without a memory check (the link is the controller's purpose; its own failure
///   is reported and retried by its backoff). It holds the gate from its connect until the socket
///   is open (the handshake's peak) or the attempt failed.
/// - Kind::session, everything else that opens a TLS session (the fetch worker's https requests,
///   an mqtts:// broker): admitted only when no one holds the gate, no link waits, and the DMA-
///   capable memory has `session_bytes + reserve_bytes` free with a block of `block_bytes`
///   (Needs). It holds the gate for the whole fetch, or until the broker's connection is up or failed.
///
/// A client that is not admitted waits (one place per client; asking again while waiting changes
/// nothing). The gate admits the next one when the holder releases, and when the platform reports
/// that the DMA-capable memory reached the figure (MemoryWatch::notify_at(): after a free, no
/// polling); otherwise the waiter's own limit ends the wait (the fetch queue's wait limit, then the
/// consumer's next scheduled attempt). When only the largest block is short, the notification is
/// asked for again a second later at the earliest (kBlockRetryMs), so frees do not flood the loop.
///
/// Without a MemoryWatch (the host) or without a figure from it, sessions are admitted by the order
/// alone.
///
/// **The pause** (homecontrol #112). Beside an open E-Home session a second TLS session rarely finds
/// the room (the house controller, 2026-11-22: every fetch starved, HP 32300 B of 49152 B). The gate
/// measures the link's session: the DMA-capable free memory when the link is admitted to connect
/// and when its socket is open (the link releases the gate then); the difference is its footprint.
/// When a session waits for memory, the link is pausable (TlsGateLink::tls_link_pausable()), the
/// free memory plus the footprint reaches the need, and the last pause started kPauseIntervalMs ago
/// or more, the gate pauses the link: the link closes with code 4000 "fetch", the waiting sessions
/// run one after the other as their memory comes free (sessions that arrive meanwhile join), and when
/// none waits and none holds the gate the link reconnects at once; after kPauseMaxMs it reconnects
/// anyway, and what still waits stays for the next pause. A session that would not fit even without
/// the link, or a link that is not open or busy, starts no pause. The link checks every second
/// whether a waiting session wants a pause (link_tick()).
///
/// Threading: the event loop's thread.
class TlsGate : private MemoryListener {
public:
	enum class Kind : uint8_t { link, session };

	/// What a session needs of the DMA-capable memory to be admitted.
	struct Needs {
		/// What the session takes while it runs (the heap hands out the DMA-capable HP SRAM first).
		uint32_t session_bytes;
		/// What must stay free beside it for the WiFi driver and the E-Home link's writes.
		uint32_t reserve_bytes;
		/// Its largest single allocation.
		uint32_t block_bytes;
	};

	/// The device's figures (homecontrol #110, measured on the house controller, 2026-11-22):
	/// - session 40960 B: the boot's language-pack fetch took the free heap from 41728 B (13.59 s) to
	///   2140 B (21.05 s) and failed before its handshake ended, so it needs at least 39588 B;
	/// - reserve 8192 B: an E-Home write in flight (a 2389 B record buffer and the 2880 B TCP send
	///   buffer, held twice: in lwIP and in the WiFi driver's copy, 8149 B); the WiFi copies that
	///   failed (180 B and 1682 B) did so with 140 B and 1880 B of HP SRAM free;
	/// - block 8448 B: the fetch worker's task stack (8192 B, EspHttpFetcher::kTaskStack) with the
	///   heap's overhead, the largest single allocation of a fetch.
	static constexpr Needs kDeviceNeeds{40960, 8192, 8448};

	/// The earliest renewed memory notification after a check found the free memory but not the block.
	static constexpr uint32_t kBlockRetryMs = 1000;

	/// The least time between two pauses' starts: E-Home is down at most a few seconds in ten minutes,
	/// however often a fetch fails or comes back (the language packs retry after 1 min, doubling).
	static constexpr uint32_t kPauseIntervalMs = 10u * 60u * 1000u;
	/// The longest pause: four fetches at the lanes' 15 s timeout.
	static constexpr uint32_t kPauseMaxMs = 60000;

	/// Clients that may wait at once.
	static constexpr size_t kMaxWaiting = 4;

	/// `memory`: the platform's (nullptr: none, admission by order alone).
	TlsGate(EventLoop& loop, const Clock& clock, MemoryWatch* memory, Needs needs = kDeviceNeeds);
	~TlsGate();

	TlsGate(const TlsGate&) = delete;
	TlsGate& operator=(const TlsGate&) = delete;

	/// Asks for the gate. True: `client` holds it now. False: it waits (or waited already);
	/// tls_gate_opened() follows when it is admitted, then it holds the gate. False also when more
	/// than kMaxWaiting clients wait (logged; the client's own retry applies).
	bool acquire(TlsGateClient& client, Kind kind);

	/// Gives the gate back, or withdraws a waiting request. Nothing when `client` neither holds nor
	/// waits.
	void release(TlsGateClient& client);

	bool holds(const TlsGateClient& client) const { return holder_ == &client; }
	bool waits(const TlsGateClient& client) const;
	bool busy() const { return holder_ != nullptr; }
	size_t waiting() const { return count_; }

	/// Why the first waiting session waits ("" when none waits for memory): "HP 38120 B free of 49152
	/// B, largest block 24064 B of 8448 B".
	const char* memory_reason() const { return reason_; }

	/// Sessions admitted, and of them the ones that had waited for memory, since the start.
	uint32_t admitted() const { return admitted_; }
	uint32_t waited_for_memory() const { return waited_for_memory_; }

	const Needs& needs() const { return needs_; }

	/// The E-Home link (`link`) and its identity as a client of the gate (`client`); nullptr: no pauses.
	void set_link(TlsGateLink* link, TlsGateClient* client) {
		link_ = link;
		link_client_ = client;
	}
	/// The link's tick (every second): starts a pause when one is due.
	void link_tick();

	bool pausing() const { return pausing_; }
	/// The link's measured footprint in the DMA-capable memory (0: not measured yet).
	uint32_t link_footprint() const { return link_footprint_; }
	/// Pauses since the start.
	uint32_t pauses() const { return pauses_; }

private:
	struct Waiter {
		TlsGateClient* client;
		Kind kind;
		uint32_t sequence;
		bool memory; ///< It waited for memory at least once.
	};

	/// Whether a session may start now; fills reason_ when not.
	bool room_for_session();
	/// Posts evaluate() (once).
	void kick();
	void evaluate();
	void remove(size_t index);
	/// Asks the platform for a notification (or, when only the block is short, for one a second later).
	void ask_for_memory();
	void rearm();
	void memory_freed() override;
	/// A session waits (for memory or for the holder).
	bool session_waits() const;
	/// Starts a pause when one is due and would help (`free_bytes`: the DMA-capable free memory now).
	void try_pause(uint32_t free_bytes);
	void end_pause(const char* why);
	void pause_over();
	/// The link was admitted: the free memory before its connect.
	void link_admitted();

	EventLoop& loop_;
	const Clock& clock_;
	MemoryWatch* memory_;
	Needs needs_;
	TlsGateClient* holder_ = nullptr;
	Waiter waiting_[kMaxWaiting] = {};
	size_t count_ = 0;
	uint32_t sequence_ = 0;
	bool kicked_ = false;
	bool notify_armed_ = false;
	bool block_short_ = false; ///< The last check found the free memory but not the block.
	TimerHandle rearm_;
	char reason_[96] = {};
	uint32_t admitted_ = 0;
	uint32_t waited_for_memory_ = 0;
	uint32_t last_free_ = 0;      ///< The DMA-capable free memory of the last check (0: no figure).

	TlsGateLink* link_ = nullptr;
	TlsGateClient* link_client_ = nullptr;
	uint32_t link_before_ = 0;    ///< The free memory when the link was admitted (0: not measured).
	uint32_t link_footprint_ = 0;
	bool pausing_ = false;
	bool paused_before_ = false;
	uint64_t pause_since_ms_ = 0;
	uint32_t pause_sessions_ = 0; ///< Sessions admitted during this pause.
	uint32_t pauses_ = 0;
	TimerHandle pause_timer_;
};

} // namespace hc

#endif // HC_TLS_GATE_H
