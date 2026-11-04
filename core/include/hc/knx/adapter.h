/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_ADAPTER_H
#define HC_KNX_ADAPTER_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "hc/datapoint.h"
#include "hc/dpt.h"
#include "hc/event_loop.h"
#include "hc/knx/address.h"
#include "hc/knx/telegram_log.h"
#include "hc/knx/tunnel.h"
#include "hc/platform/clock.h"
#include "hc/technology.h"

namespace hc {
namespace knx {

/// How one datapoint is connected to the bus (a KNX group object): the address it sends to, the
/// addresses it listens to and the datapoint type. The flags come from the datapoint
/// (doc/datapoints.md): kTransmit sends a command as a group write to `send`, kReadable answers a
/// group read of `send` with the current value, kReadOnStart reads `send` (or the first listening
/// address) when the tunnel comes up.
///
/// A `transmit_only` mapping is for a datapoint another component owns (the KNX time master): the
/// adapter becomes its transmitter (DatapointModel::set_transmitter()) instead of its owner. What
/// the owner publishes goes out as a group write to `send`; a group read of `send` is answered from
/// the model's value while its quality is good; received telegrams never update it (logged at debug
/// level), the adapter never marks it stale and never reads it on start. It needs a sending address
/// that no other datapoint of the mapping sends on, the kTransmit flag and no listening addresses.
struct KnxMapping {
	static constexpr size_t kMaxListen = 8;

	uint16_t datapoint = 0;
	Dpt dpt;
	bool has_send = false;
	GroupAddress send;
	/// The addresses whose writes and responses update the datapoint; `send` listens as well without
	/// being repeated here. Up to kMaxListen; may be nullptr with listen_count 0.
	const GroupAddress* listen = nullptr;
	size_t listen_count = 0;
	/// The adapter transmits for another owner (see above) instead of owning the datapoint.
	bool transmit_only = false;
};

/// The KNX technology adapter: translates between group telegrams of a KnxTunnel and the
/// datapoints bound to it (doc/knx.md).
///
/// - A group write or response on a listening address is decoded with the mapping's DPT and
///   reported; a decoding refusal is logged with the address and the octets, the datapoint stays.
/// - A group read of a readable datapoint's sending address is answered with a group response of
///   the current value if its quality is good (otherwise not answered, logged).
/// - A command of a transmitting datapoint is sent as a group write. When the gateway confirms it,
///   the other datapoints listening on the address take the value too, as the bus devices do.
/// - While the tunnel is not up a command is held (homecontrol #72): it is checked and encoded as
///   if it were sent (a value the DPT cannot encode is refused), the model stores it and marks the
///   datapoint pending (Datapoint::pending()); a later command replaces it (one value per
///   datapoint, no queue). When the tunnel comes up the held values are sent once, in datapoint
///   order, before the reads on start, through the same send path (at most kPendingCapacity
///   unconfirmed at a time, the rest as confirmations come). A report of the datapoint drops its
///   held value (the bus's is newer). A command while the held values still fill the send path
///   is held behind them. A write queued while the tunnel was up and lost when it went down is not
///   held (the probe KnxProbe.QueuedCommandsAreLostWithTheTunnelAndNotReplayed): only what was
///   commanded while it was not up is. A transmit-only datapoint is never held: its owner publishes
///   again (the time master's clock).
///   Across an image swap the model keeps the mark of a datapoint it keeps the value of (by name);
///   configure() drops it for a datapoint the new mapping cannot send.
/// - Refused: a command of a datapoint without a sending address, one the DPT cannot encode, and
///   while the tunnel is up and kPendingCapacity writes wait for their confirmation (outside the
///   sending of the held commands).
/// - Tunnel down: every datapoint becomes stale. Tunnel up: the read-on-start datapoints are read,
///   one every kReadSpacingMs, so a hundred reads do not burst onto the bus.
/// - A read the application asks for (read(), a rule's `read` action) joins the same queue: a group
///   read of the address read-on-start uses (`send`, else the first listening address), with the
///   same spacing; a read already waiting is not queued twice. Refused while the tunnel is down and
///   for a transmit-only datapoint (its owner publishes it; nothing on the bus answers for it).
/// - Every group telegram (received, and sent once confirmed) goes into the TelegramLog, if given.
///
/// Memory: configure() allocates the tables once (two blocks); nothing is allocated afterwards.
/// Threading: called on the event loop's thread.
class KnxAdapter : public Technology, public KnxTunnelListener {
public:
	/// Spacing of the reads after the tunnel came up (the first one follows after this delay).
	static constexpr uint32_t kReadSpacingMs = 100;

	/// Writes and responses sent and not yet confirmed that the adapter remembers (to exclude the
	/// sender from the local delivery).
	static constexpr size_t kPendingCapacity = KnxTunnel::kQueueCapacity;

	KnxAdapter(EventLoop& loop, const Clock& clock, DatapointModel& model, KnxTunnel& tunnel,
		TelegramLog* log = nullptr);
	~KnxAdapter();

	KnxAdapter(const KnxAdapter&) = delete;
	KnxAdapter& operator=(const KnxAdapter&) = delete;

	/// Registers with the tunnel (once).
	void start();

	/// Replaces the mapping by `count` mappings and binds their datapoints to this adapter (a
	/// transmit_only mapping makes it the datapoint's transmitter instead). The caller merges the
	/// mappings of every source into one list: the image's and the time master's. Every
	/// mapping is checked before anything changes: the datapoint exists, is mapped once and is not
	/// owned by another technology; the DPT is supported and produces the datapoint's value type
	/// (or both are numbers); at most KnxMapping::kMaxListen listening addresses and at least one
	/// address; a transmitting or readable datapoint has a sending address; a transmit_only datapoint
	/// has an owner other than this adapter and no other transmitter, transmits, has no listening
	/// address and a sending address no other mapping sends on. A refusal (false, the
	/// reason in `error` and in the log) keeps the previous mapping. After a model load the mapping
	/// must be configured again (the load unbinds every datapoint).
	bool configure(const KnxMapping* mappings, size_t count, const char** error = nullptr);

	/// Removes the mapping (the datapoints stay bound to the adapter until the next model load).
	void clear();

	/// Mapped datapoints.
	size_t size() const { return count_; }

	/// The datapoint whose sending address is `address` (the lowest id if several send there), so
	/// that a component can refuse an address another datapoint already sends on; empty if none.
	std::optional<uint16_t> sender_of(GroupAddress address) const;

	/// Reads still to be sent: from the last tunnel up and those the application asked for.
	size_t reads_pending() const;

	/// Commands held while the tunnel was not up and not yet sent (Datapoint::pending()).
	size_t commands_held() const;

	/// The tunnel and the telegram log (nullptr if none) it was given (the monitor stream sends and
	/// listens there).
	KnxTunnel& tunnel() { return tunnel_; }
	TelegramLog* telegram_log() { return log_; }

	// Technology
	const char* name() const override { return "knx"; }
	bool command(const Datapoint& datapoint, const Value& value) override;
	bool read(const Datapoint& datapoint) override;
	bool holds_commands() const override { return true; }

	// KnxTunnelListener
	void tunnel_state_changed(const KnxTunnel& tunnel, TunnelState previous) override;
	void tunnel_frame(const KnxTunnel& tunnel, const LData& frame) override;
	void tunnel_sent(const KnxTunnel& tunnel, uint32_t token, const LData& frame, SendOutcome outcome) override;

private:
	struct Entry {
		uint16_t datapoint;
		Dpt dpt;
		bool has_send;
		GroupAddress send;
		GroupAddress first_listen;
		bool transmit_only;
		bool read_wanted; ///< A group read waits in the queue.
	};

	struct Listen {
		GroupAddress address;
		uint16_t entry;
	};

	struct PendingSend {
		uint32_t token;
		uint16_t datapoint;
	};

	const Entry* entry_of(uint16_t datapoint) const;
	/// First index of `address` in listens_ (listen_count_ if none).
	size_t first_listen(GroupAddress address) const;
	void deliver(const LData& frame, const GroupData& data, uint16_t except);
	void answer_read(GroupAddress address);
	void log_telegram(TelegramRecord::Direction direction, const LData& frame, const GroupData& data);
	void schedule_read();
	void read_due();
	/// Queues a group read of every read-on-start datapoint (tunnel up, new mapping).
	void want_reads_on_start();
	void free_tables();
	/// Sends the held commands from held_cursor_ on while the send path has room (tunnel up).
	void send_held();
	/// Holds the command `value` of `datapoint` (entry `index`) to send later (the model stores the
	/// value); logs the first one of an outage.
	void hold(size_t index, const Datapoint& datapoint, const Value& value);
	/// Whether a write can be queued now: the tunnel is up, fewer than kPendingCapacity sends wait for
	/// their confirmation, the tunnel's queue has room.
	bool can_send() const;

	EventLoop& loop_;
	const Clock& clock_;
	DatapointModel& model_;
	KnxTunnel& tunnel_;
	TelegramLog* log_;
	bool started_ = false;
	Entry* entries_ = nullptr;   ///< Sorted by datapoint id.
	size_t count_ = 0;
	Listen* listens_ = nullptr;  ///< Sorted by address.
	size_t listen_count_ = 0;
	size_t read_cursor_ = 0;     ///< Next entry to consider for a read (count_: none waits).
	size_t held_cursor_ = 0;     ///< Next entry to consider for a held command (count_: none waits).
	bool hold_logged_ = false;   ///< The first held command since the tunnel was last up is logged.
	TimerHandle read_timer_;
	PendingSend pending_[kPendingCapacity] = {};
	size_t pending_count_ = 0;
};

} // namespace knx
} // namespace hc

#endif // HC_KNX_ADAPTER_H
