/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_ADAPTER_H
#define HC_KNX_ADAPTER_H

#include <cstddef>
#include <cstdint>

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
};

/// The KNX technology adapter: translates between group telegrams of a KnxTunnel and the
/// datapoints bound to it (doc/knx.md).
///
/// - A group write or response on a listening address is decoded with the mapping's DPT and
///   reported; a decoding refusal is logged with the address and the octets, the datapoint stays.
/// - A group read of a readable datapoint's sending address is answered with a group response of
///   the current value if its quality is good (otherwise not answered, logged).
/// - A command of a transmitting datapoint is sent as a group write (refused while the tunnel is
///   down: the value stays). When the gateway confirms it, the other datapoints listening on the
///   address take the value too, as the bus devices do.
/// - Tunnel down: every datapoint becomes stale. Tunnel up: the read-on-start datapoints are read,
///   one every kReadSpacingMs, so a hundred reads do not burst onto the bus.
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

	/// Replaces the mapping by `count` mappings and binds their datapoints to this adapter. Every
	/// mapping is checked before anything changes: the datapoint exists, is mapped once and is not
	/// owned by another technology; the DPT is supported and produces the datapoint's value type
	/// (or both are numbers); at most KnxMapping::kMaxListen listening addresses and at least one
	/// address; a transmitting or readable datapoint has a sending address. A refusal (false, the
	/// reason in `error` and in the log) keeps the previous mapping. After a model load the mapping
	/// must be configured again (the load unbinds every datapoint).
	bool configure(const KnxMapping* mappings, size_t count, const char** error = nullptr);

	/// Removes the mapping (the datapoints stay bound to the adapter until the next model load).
	void clear();

	/// Mapped datapoints.
	size_t size() const { return count_; }

	/// Reads still to be sent from the last tunnel up.
	size_t reads_pending() const;

	// Technology
	const char* name() const override { return "knx"; }
	bool command(const Datapoint& datapoint, const Value& value) override;

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
	void free_tables();

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
	size_t read_cursor_ = 0;     ///< Next entry to consider for a read (count_: done).
	TimerHandle read_timer_;
	PendingSend pending_[kPendingCapacity] = {};
	size_t pending_count_ = 0;
};

} // namespace knx
} // namespace hc

#endif // HC_KNX_ADAPTER_H
