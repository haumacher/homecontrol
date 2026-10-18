/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_MQTT_H
#define HC_MQTT_H

#include <cstddef>
#include <cstdint>

#include "hc/backoff.h"
#include "hc/connectivity.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/history.h"
#include "hc/json.h"
#include "hc/log.h"
#include "hc/platform/clock.h"
#include "hc/platform/mqtt_client.h"
#include "hc/platform/settings.h"

namespace hc {

/// The setting keys of the MQTT client (doc/mqtt.md). Persisted names: never rename.
namespace setting {
/// The broker password: a device setting, never part of the runtime image.
constexpr const char* kMqttPassword = "mqtt.password";
/// The user name when the image names none.
constexpr const char* kMqttUser = "mqtt.user";
} // namespace setting

/// Who this controller is towards the broker and Home Assistant.
struct MqttIdentity {
	/// "homecontrol-a1b2c3" (the mDNS host name): the client id, Home Assistant's node id and device
	/// identifier, and (without its "homecontrol-" prefix) the default base topic's last level.
	/// 1..48 characters of [A-Za-z0-9_-].
	const char* node_id = "homecontrol";
	/// The device's software version in the discovery's device block (hc::kVersion).
	const char* sw_version = "";
};

/// The members of a runtime image the MQTT client reads (spans into the image text; an absent
/// member has kind invalid). The client copies what it keeps.
struct MqttImage {
	JsonSpan mqtt;       ///< The `mqtt` member (spec/mqtt.md).
	JsonSpan datapoints; ///< `datapoints`: units, history retention and spacing.
	JsonSpan functions;  ///< `functions`: the Home Assistant entities.
	JsonSpan name;       ///< `image.name`: the device name in Home Assistant.
};

/// Where the client stands.
enum class MqttState : uint8_t {
	disabled,   ///< No configuration, or `enabled: false`.
	waiting,    ///< Configured; the network is down (checked every kReadyPollMs).
	backoff,    ///< A connection failed or was lost; the next attempt is due at next_attempt_ms().
	connecting, ///< connect() was called; waiting for the broker.
	connected,
	retiring,   ///< Publishing `offline` and the removals before an orderly disconnect.
};

/// "disabled", "waiting", "backoff", "connecting", "connected", "retiring".
const char* mqtt_state_name(MqttState state);

/// What the console shows.
struct MqttStatus {
	MqttState state = MqttState::disabled;
	const char* broker = "";
	const char* username = "";
	bool password_set = false;
	const char* base_topic = "";
	bool discovery = false;
	const char* discovery_prefix = "";
	size_t functions = 0;
	uint32_t connects = 0;        ///< Successful connections since configure.
	uint32_t failures = 0;        ///< Consecutive failed attempts.
	uint64_t next_attempt_ms = 0; ///< Monotonic time of the next attempt (backoff, waiting).
	const char* last_error = "";  ///< Why the last attempt failed or the connection ended.
	uint32_t published = 0;       ///< Messages accepted by the client since construction.
	uint32_t commands = 0;        ///< Commands executed.
	uint32_t refusals = 0;        ///< Commands refused (published to the error topic).
	size_t dirty = 0;             ///< Datapoint states waiting to be published.
	size_t pending = 0;           ///< The client's outgoing queue.
	bool history = false;
	uint32_t history_interval_min = 0;
	uint32_t history_messages = 0; ///< History messages published since construction.
};

/// The MQTT client (issue #18, spec/mqtt.md, doc/mqtt.md): publishes every datapoint's state,
/// executes commands from the broker, announces the runtime image's functions to Home Assistant
/// (MQTT discovery) and exports the history rings for long-term storage (InfluxDB, Telegraf).
/// The core decides everything; the platform's MqttClient moves the bytes.
///
/// - **Connection.** Connects when configured and the network is up; a failed attempt or a lost
///   connection is retried after a Backoff of kRetryInitialMs doubling up to kRetryCapMs; an attempt
///   without an answer after kConnectTimeoutMs counts as failed. The last will publishes `offline`
///   on `<base>/status`; after the CONNACK the client publishes `online` there (retained).
/// - **State.** `<base>/dp/<name>/state` carries `{"value": ..., "quality": ..., "time": ...}`
///   (retained by default), published on every change of value or quality; changes within one loop
///   turn coalesce into one message with the latest value; a full outgoing queue keeps the
///   datapoint marked and retries (never a stale value after a newer one). All states are
///   published again after every connect and every image load.
/// - **Commands.** `<base>/dp/<name>/set` (a JSON value) goes to DatapointModel::command(); a
///   refusal goes to `<base>/dp/<name>/error` and the log. `<base>/fn/<function>/set` takes
///   OPEN/CLOSE/STOP for a blind. Retained commands are refused (they would repeat at every connect).
/// - **Discovery.** For every function a retained config under
///   `<prefix>/<component>/<node id>/<function id>/config`, published after every connect and image
///   load; after each, a sweep subscribes to this node's configs and state topics for kSweepMs and
///   clears (empty retained payload) every retained one that the image no longer has: removal works
///   across image swaps and reboots alike.
/// - **History.** At connect and every `interval` minutes the samples of every history ring since
///   the last export go to `<base>/history/<name>` as `[[t, value], ...]`, at most kHistoryBatch
///   per message; the cursor (per datapoint, by name across image swaps, not across reboots)
///   advances only with an accepted message.
///
/// Memory: configure() allocates one arena (the functions, the per-datapoint table) and, with the
/// first configuration, the payload and history buffers (about 3.6 KB), which go when the
/// configuration does (an image without `mqtt` costs nothing beyond the object, issue #44). A
/// configuration without the memory for them is refused. Nothing is allocated in steady state. One event-loop timer, one
/// datapoint listener (while configured). Threading: the event loop's thread.
class Mqtt : public DatapointListener, public MqttClientListener {
public:
	static constexpr uint32_t kRetryInitialMs = 5000;
	static constexpr uint32_t kRetryCapMs = 300000;
	static constexpr uint32_t kConnectTimeoutMs = 30000;
	/// How often the network is checked while it is down.
	static constexpr uint32_t kReadyPollMs = 5000;
	/// How long the retained configs and states are collected after a (re)publication.
	static constexpr uint32_t kSweepMs = 10000;
	/// Retry of a publication the client refused (its queue was full).
	static constexpr uint32_t kRetryPublishMs = 250;
	/// Most messages handed to the client in one loop turn (the rest follows in the next turn).
	static constexpr size_t kPublishesPerTurn = 16;
	/// Most samples in one history message.
	static constexpr size_t kHistoryBatch = 32;
	/// Most history-ring queries in one loop turn.
	static constexpr size_t kHistoryQueriesPerTurn = 64;
	/// Waiting for the outgoing queue to drain before an orderly disconnect.
	static constexpr uint32_t kRetireMs = 3000;
	static constexpr uint32_t kRetirePollMs = 100;
	static constexpr uint16_t kKeepaliveS = 60;

	static constexpr uint32_t kDefaultHistoryIntervalMin = 15;
	static constexpr uint32_t kMaxHistoryIntervalMin = 1440;

	static constexpr size_t kMaxBroker = 127;
	static constexpr size_t kMaxUser = 63;
	static constexpr size_t kMaxBaseTopic = 64;
	static constexpr size_t kMaxPrefix = 32;
	static constexpr size_t kMaxNodeId = 48;
	/// The longest config payload (a dimmer with every role is about 1 KB).
	static constexpr size_t kMaxPayload = 2048;

	Mqtt(EventLoop& loop, const Clock& clock, const Settings& settings, const Connectivity& connectivity,
		DatapointModel& model, MqttClient& client, const MqttIdentity& identity, const History* history = nullptr);
	~Mqtt();

	Mqtt(const Mqtt&) = delete;
	Mqtt& operator=(const Mqtt&) = delete;

	/// Checks an `mqtt` member (spec/mqtt.md) without changing anything: for the image loader's
	/// validation pass. False with `error` (the member path and the reason).
	static bool check_json(const char* text, size_t length, const char** error);

	/// Configures from a whole runtime image text: finds `mqtt`, `datapoints`, `functions` and
	/// `image.name`. Without an `mqtt` member it is clear(). Call after every model load (the
	/// datapoints are resolved by name here).
	bool configure_image(const char* text, size_t length, const char** error = nullptr);

	/// The same from the members (all spans into one text). Checks everything before anything
	/// changes: a refusal (false, the reason in `error` and in the log) keeps the previous
	/// configuration. When the connection parameters (broker, user, base topic) are unchanged, a
	/// connection stays up and the discovery, the sweep and all states are published again;
	/// otherwise it retires (offline, disconnect) and connects anew.
	bool configure(const MqttImage& image, const char** error = nullptr);

	/// The `mqtt` member alone (no functions, so no discovery entities): the tests and an image
	/// without functions.
	bool configure_json(const char* text, size_t length, const char** error = nullptr);

	/// Forgets the configuration: a connection retires (the discovery configs are cleared,
	/// `offline` published, disconnect).
	void clear();

	/// The datapoint model was loaded again with the same image (a dynamic provider's datapoints
	/// joined or left, doc/image.md): the per-datapoint table follows the model's new size, the
	/// image's datapoints keep their ids, units and rings, the history cursors their names, and
	/// every state is published again. Nothing happens without a configuration.
	bool model_reloaded(const char** error = nullptr);

	/// Drops the connection and connects again now (a new password).
	void reconnect(const char* why);

	/// The process is ending (the host build's SIGINT): `offline` and an orderly disconnect, the
	/// discovery configs kept for the next run (clear() removes them). Ends in state disabled.
	void stop();

	/// Publishes every discovery config again and sweeps. Refused when not connected or discovery is
	/// off.
	bool publish_discovery(const char** error = nullptr);

	MqttState state() const { return state_; }
	/// Whether the payload and history buffers are allocated (only while configured or retiring).
	bool has_buffers() const { return buffers_ != nullptr; }
	void status(MqttStatus& out) const;
	/// The base topic in effect ("" when not configured).
	const char* base_topic() const;

	// DatapointListener
	void datapoint_updated(const DatapointUpdate& update) override;

	// MqttClientListener
	void mqtt_connected() override;
	void mqtt_disconnected(const char* reason) override;
	void mqtt_message(const MqttMessage& message) override;

private:
	struct Table;
	struct Function;
	struct Point;

	struct Buffers;

	bool build(Table& table, const MqttImage& image, const char** error);
	bool ensure_buffers(const char** error);
	/// Frees the buffers when neither a configuration nor a retiring one is left.
	void release_buffers_if_idle();
	bool commit(Table* table, const char** error);
	bool refuse(const char** error, const char* fmt, ...) HC_PRINTF_FORMAT(3, 4);

	void set_state(MqttState state);
	void attempt();
	void fail(const char* reason);
	void begin_retire(Table* next);
	void finish_retire();
	void start_session_work();
	void start_sweep();

	void request_pump();
	void pump();
	enum class Step : uint8_t { done, budget, blocked };
	Step pump_discovery(size_t& budget);
	Step pump_states(size_t& budget);
	Step pump_history(size_t& budget);
	void pump_retire();
	bool send(const char* topic, const char* payload, size_t length, bool retain, uint8_t qos);

	void timer_due();
	void pump_due();
	void arm();

	bool write_config(const Function& function);
	size_t config_topic(char* out, size_t size, const char* component, const char* id) const;
	bool write_state(const Datapoint& datapoint);
	size_t point_topic(char* out, size_t size, const char* name, const char* leaf) const;
	void handle_command(const char* name, const MqttMessage& message);
	void handle_function_command(const char* id, const MqttMessage& message);
	void handle_sweep(const MqttMessage& message);
	void end_sweep();
	void publish_error(const char* level, const char* name, const char* reason, const MqttMessage& message);
	const char* component_of(const Function& function) const;
	const Function* find_function(const char* id) const;

	EventLoop& loop_;
	const Clock& clock_;
	const Settings& settings_;
	const Connectivity& connectivity_;
	DatapointModel& model_;
	MqttClient& client_;
	const History* history_;
	char node_id_[kMaxNodeId + 1] = {};
	char sw_version_[24] = {};

	Table* table_ = nullptr;
	/// The configuration waiting for a retirement to finish (nullptr: none, or clear()).
	Table* next_ = nullptr;
	bool listening_ = false;
	/// Retiring: clear the old table's discovery configs first; the next one to clear; offline sent.
	bool remove_configs_ = false;
	size_t retire_index_ = 0;
	bool offline_sent_ = false;

	MqttState state_ = MqttState::disabled;
	Backoff backoff_;
	TimerHandle timer_;
	uint64_t attempt_at_ = 0;
	uint64_t connect_deadline_ = 0;
	uint64_t sweep_end_ = 0;
	uint64_t history_due_ = 0;
	uint64_t retry_publish_at_ = 0;
	uint64_t retire_deadline_ = 0;
	uint64_t retire_poll_at_ = 0;
	bool pump_posted_ = false;

	// Session work (reset at every connect).
	bool online_sent_ = false;
	bool subscribed_ = false;
	bool sweeping_ = false;
	bool sweep_again_ = false;
	size_t discovery_next_ = 0;
	size_t dirty_ = 0;
	size_t dirty_scan_ = 0;
	bool export_active_ = false;
	size_t export_index_ = 0;
	int64_t export_to_ = 0;

	uint32_t connects_ = 0;
	uint32_t published_ = 0;
	uint32_t commands_ = 0;
	uint32_t refusals_ = 0;
	uint32_t history_messages_ = 0;
	bool clock_warned_ = false;
	char last_error_[96] = {};
	char error_[160] = {};

	char topic_[MqttClient::kMaxTopic + 1] = {};
	/// The payload of the message being written and the samples of a history export: allocated with
	/// the configuration (nullptr while there is none).
	Buffers* buffers_ = nullptr;
};

} // namespace hc

#endif // HC_MQTT_H
