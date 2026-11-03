/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_MONITOR_FEED_H
#define HC_MONITOR_FEED_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/json.h"
#include "hc/knx/telegram_log.h"
#include "hc/platform/clock.h"
#include "hc/rules/engine.h"
#include "hc/zigbee/adapter.h"

namespace hc {

struct WebRequest;
struct WebResponse;

namespace knx {
class KnxTunnel;
}

namespace image {
class ImageService;
}

/// One entry of the monitor (spec/api.md, "Monitor"): what the controller saw on a bus or did
/// itself. Fixed size (76 bytes, issue #45), stored in MonitorFeed's ring. Compact without losing
/// anything the JSON shows: the monotonic time in 48 bits (8900 years), the wall time in 32 bits
/// (seconds since the epoch, until 2106), the model generation in 8 bits (exact for every event the
/// ring holds, see MonitorFeed::is_current()), the IEEE address as two halves (no 8-byte alignment),
/// and an image event's revision and source in the place of the value it never carries.
struct MonitorEvent {
	static constexpr uint16_t kNoDatapoint = 0xFFFF;
	/// Bytes of a copied text (a rule id, an image's project id) including the NUL; longer ones are cut.
	static constexpr size_t kMaxText = 28;

	enum class Kind : uint8_t {
		knx,     ///< A group telegram (sub: KnxDirection).
		zigbee,  ///< A Zigbee frame (sub: ZigbeeEvent).
		rule,    ///< A rule commanded or read a datapoint (sub: RuleAction).
		command, ///< A datapoint was commanded from outside the rules (sub: CommandOrigin).
		image,   ///< The model changed (sub: ImageEvent).
	};
	enum class KnxDirection : uint8_t { rx, tx };
	enum class ZigbeeEvent : uint8_t { report, action, command };
	enum class CommandOrigin : uint8_t { app, ws, api };
	/// Who authenticated a `ws` or `api` command: a session, a token, the E-Home connection.
	enum class Via : uint8_t { session, token, ehome };
	enum class RuleAction : uint8_t { command, read };
	enum class ImageEvent : uint8_t { load, reload, unload };

	enum Flags : uint8_t {
		kHasValue = 1,
		kWallTime = 2,   ///< `wall` is meaningful (the clock was synchronized).
		kRepeated = 4,   ///< KNX: the repeat flag.
		kShortForm = 8,  ///< KNX: the value travelled in the APCI octet.
		kFromMonitor = 16, ///< KNX tx: a telegram a monitor client sent.
		kViaToken = 32,  ///< command from the WebSocket: the client authenticated with a token.
		kViaEhome = 64,  ///< command over the E-Home connection (doc/ehome.md).
	};

	struct Knx {
		uint16_t source;
		uint16_t destination;
		uint8_t service;  ///< knx::GroupService
		uint8_t priority; ///< 0 system .. 3 low
		uint8_t hops;
		uint8_t length;
		uint8_t data[knx::GroupData::kMaxData];
	};
	struct Zigbee {
		uint32_t ieee_low;  ///< The IEEE address's lower 32 bits (see ieee()).
		uint32_t ieee_high;
		uint16_t short_addr;
		uint16_t cluster;
		uint16_t code; ///< The attribute of a report, the command id of an action or command.
		uint8_t endpoint;

		uint64_t ieee() const { return static_cast<uint64_t>(ieee_high) << 32 | ieee_low; }
		void set_ieee(uint64_t ieee) {
			ieee_low = static_cast<uint32_t>(ieee);
			ieee_high = static_cast<uint32_t>(ieee >> 32);
		}
	};
	struct Named {
		char text[kMaxText]; ///< The rule id, the image's project id.
	};
	/// An image event's revision and source (it carries no value).
	struct Image {
		uint32_t revision;
		uint8_t source; ///< image::Source
	};

	uint32_t ms_low = 0;  ///< Monotonic milliseconds, lower 32 bits (see ms()).
	uint32_t wall = 0;    ///< Seconds since the epoch, with kWallTime.
	uint16_t ms_high = 0; ///< Bits 32..47 of the monotonic milliseconds.
	uint16_t datapoint = kNoDatapoint;
	uint8_t generation = 0; ///< The model generation `datapoint` refers to, modulo 256.
	Kind kind = Kind::knx;
	uint8_t sub = 0;
	uint8_t flags = 0;
	union {
		Knx knx;
		Zigbee zigbee;
		Named named;
	};
	union {
		Value value;  ///< With kHasValue.
		Image image;  ///< Kind::image.
	};

	MonitorEvent() : named(), value() {}

	/// Monotonic milliseconds.
	uint64_t ms() const { return static_cast<uint64_t>(ms_high) << 32 | ms_low; }
	void set_ms(uint64_t ms) {
		ms_low = static_cast<uint32_t>(ms);
		ms_high = static_cast<uint16_t>(ms >> 32);
	}
	/// Sets the wall time (seconds since the epoch) and kWallTime.
	void set_wall(int64_t seconds) {
		wall = static_cast<uint32_t>(seconds);
		flags |= kWallTime;
	}
};

static_assert(sizeof(MonitorEvent) == 76, "a MonitorEvent is 76 bytes (issue #45)");

/// The monitor stream (issue #19): a ring of the last kCapacity MonitorEvents with sequence numbers,
/// fed by
///
/// - the KNX TelegramLog (its listener: every group telegram received, and sent once confirmed,
///   with the decoded value of the mapped datapoint: the value the adapter reports right after
///   logging it, or for a sent telegram the datapoint's value),
/// - the Zigbee adapter (its traffic listener: attribute reports, button commands, commands sent),
/// - the rule engine (its observer: a rule's command turns the command event into a rule event),
/// - the datapoint model (its listener: commands, with the origin a caller declared through
///   MonitorOrigin: the WebSocket, the REST API, or the application),
/// - model changes (model_changed(): image load, provider reload, unload).
///
/// Readers follow by sequence number: since() and write_events(). Old events are overwritten; a
/// reader that asks for an overwritten sequence gets a gap marker first. The datapoint of an event
/// is valid for the model generation it was recorded in: after a model change the name and id are
/// left out.
///
/// It also carries the monitor's KNX sending (knx_send(), knx_read()): the console's `knx write`
/// and `knx read` for E-Home's typed sending.
///
/// Memory: the ring is part of the object (kCapacity * sizeof(MonitorEvent)); nothing is allocated.
/// Threading: the event loop's thread.
class MonitorFeed : public DatapointListener,
	public knx::TelegramListener,
	public zigbee::ZigbeeTrafficListener,
	public rules::EngineObserver {
public:
	static constexpr size_t kCapacity = 128;
	static_assert(kCapacity <= 256, "MonitorEvent::generation (8 bits) must outlast the ring");
	/// Telegrams sent from the monitor that are remembered until the gateway confirms them (their
	/// tx event carries "origin": "monitor").
	static constexpr size_t kPendingSends = 4;
	static constexpr uint32_t kPendingSendMs = 5000;
	/// Room kept free for the closing of a batch.
	static constexpr size_t kClosingReserve = 64;

	MonitorFeed(const Clock& clock, DatapointModel& model);
	~MonitorFeed();

	MonitorFeed(const MonitorFeed&) = delete;
	MonitorFeed& operator=(const MonitorFeed&) = delete;

	/// Registers with the model (false and logged when it has no listener slot).
	bool start();

	/// The KNX sources: the telegram log (becomes its listener) and the tunnel the monitor sends to.
	/// Either may be nullptr.
	void attach_knx(knx::TelegramLog* log, knx::KnxTunnel* tunnel);
	void attach_zigbee(zigbee::ZigbeeAdapter* adapter);
	void attach_rules(rules::Engine* engine);
	/// Attaches what the image service reaches: its KNX adapter's log and tunnel, its rule engine and
	/// the Zigbee adapter among its providers. Repeatable (after every model change).
	void attach_image(image::ImageService& image);

	/// The model was replaced: older events lose their datapoint references, and an image event is
	/// recorded (load, reload or unload, from `image`; reload without one).
	void model_changed(const image::ImageService* image);

	/// Called (once per append) when an event was recorded; the WebSocket feed posts its flush.
	void set_appended(Callback callback) { appended_ = callback; }

	/// The sequence number the next event gets.
	uint32_t next() const { return total_; }
	/// The sequence number of the oldest event held.
	uint32_t first() const { return total_ - static_cast<uint32_t>(count_); }
	size_t size() const { return count_; }
	/// The event with `sequence`; nullptr when overwritten or not written yet.
	const MonitorEvent* find(uint32_t sequence) const;
	uint16_t generation() const { return generation_; }
	/// Whether `event` was recorded in the current model generation. The event keeps the generation
	/// modulo 256, which is exact for every event the ring holds: each model change appends an event,
	/// so an event still held is at most kCapacity - 1 generations old.
	bool is_current(const MonitorEvent& event) const { return event.generation == static_cast<uint8_t>(generation_); }

	/// Where a reader that asks for `since` starts: `since` itself, or first() when `since` lies
	/// beyond next() (a sequence of an earlier run; `reset` is set then).
	uint32_t start_of(uint32_t since, bool& reset) const;

	/// Writes events from `cursor` into the open array of `json`: a gap marker first when `cursor`
	/// was overwritten, then the events while they fit (the writer's reserve is the caller's) and at
	/// most `limit`. Returns the cursor after the last event written.
	uint32_t write_events(JsonWriter& json, uint32_t cursor, size_t limit) const;

	/// One event as JSON (spec/api.md, "Monitor").
	void write_event(JsonWriter& json, const MonitorEvent& event, uint32_t sequence) const;

	/// A send or read request of a monitor client ({"knx": {...}}). False with the refusal's code and
	/// reason; true when the tunnel took the telegram (its tx event follows when confirmed).
	bool send(const JsonSpan& spec, const char** code, const char** reason);
	bool read(const JsonSpan& spec, const char** code, const char** reason);

	/// While an origin is set, commands are recorded with it (MonitorOrigin).
	void set_origin(MonitorEvent::CommandOrigin origin, MonitorEvent::Via via) {
		origin_ = origin;
		origin_via_ = via;
	}

	// DatapointListener
	void datapoint_updated(const DatapointUpdate& update) override;
	// knx::TelegramListener
	void telegram_logged(const knx::TelegramRecord& record, uint64_t sequence) override;
	// zigbee::ZigbeeTrafficListener
	void zigbee_traffic(const zigbee::ZigbeeTraffic& traffic) override;
	// rules::EngineObserver
	void rule_commanded(const char* rule, const Datapoint& datapoint, const Value& value) override;
	void rule_read(const char* rule, const Datapoint& datapoint) override;

	/// The names of the JSON encoding.
	static const char* kind_name(MonitorEvent::Kind kind);

private:
	struct PendingSend {
		bool used;
		uint16_t destination;
		uint8_t service;
		uint8_t length;
		bool short_form;
		uint8_t data[knx::GroupData::kMaxData];
		uint64_t until_ms;
	};

	MonitorEvent& append(MonitorEvent::Kind kind, uint8_t sub);
	MonitorEvent* newest();
	bool knx_target(const JsonSpan& spec, JsonSpan& knx, const char** code, const char** reason);
	void remember_send(uint16_t destination, uint8_t service, const uint8_t* data, uint8_t length, bool short_form);
	bool take_pending(const knx::TelegramRecord& record);
	bool tunnel_ready(const char** code, const char** reason) const;

	const Clock& clock_;
	DatapointModel& model_;
	bool listening_ = false;
	knx::TelegramLog* log_ = nullptr;
	knx::KnxTunnel* tunnel_ = nullptr;
	zigbee::ZigbeeAdapter* zigbee_ = nullptr;
	rules::Engine* engine_ = nullptr;
	Callback appended_;
	MonitorEvent events_[kCapacity];
	size_t head_ = 0;
	size_t count_ = 0;
	uint32_t total_ = 0;
	uint16_t generation_ = 0;
	/// The newest event waits for the value its datapoint is reported with (a received telegram).
	bool awaiting_value_ = false;
	MonitorEvent::CommandOrigin origin_ = MonitorEvent::CommandOrigin::app;
	MonitorEvent::Via origin_via_ = MonitorEvent::Via::session;
	PendingSend pending_[kPendingSends] = {};
	bool image_known_ = false;
	uint64_t image_loaded_ms_ = 0;
};

/// GET /api/monitor?since=n&limit=m (spec/api.md, "Monitor"), called by WebApi; `feed` nullptr: the
/// server has no monitor stream (404 monitor.unavailable).
void handle_monitor_request(const MonitorFeed* feed, const WebRequest& request, WebResponse& response);

/// Declares the origin of the commands made in its scope (the WebSocket's and the REST API's
/// command handlers). `feed` may be nullptr.
class MonitorOrigin {
public:
	MonitorOrigin(MonitorFeed* feed, MonitorEvent::CommandOrigin origin,
		MonitorEvent::Via via = MonitorEvent::Via::session)
		: feed_(feed) {
		if (feed_ != nullptr) {
			feed_->set_origin(origin, via);
		}
	}
	~MonitorOrigin() {
		if (feed_ != nullptr) {
			feed_->set_origin(MonitorEvent::CommandOrigin::app, MonitorEvent::Via::session);
		}
	}
	MonitorOrigin(const MonitorOrigin&) = delete;
	MonitorOrigin& operator=(const MonitorOrigin&) = delete;

private:
	MonitorFeed* feed_;
};

} // namespace hc

#endif // HC_MONITOR_FEED_H
