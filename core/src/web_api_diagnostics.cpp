/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// GET /api/system/diagnostics (issue #42, spec/api.md "System diagnostics"): what the System page
// shows - everything the console's `status` prints and what else helps with a complaint. The core
// composes it from the platform's SystemInfo, NetworkDriver and Diagnostics and from its own
// counters (the event loop, the connectivity, the KNX tunnel, the Zigbee adapter).

#include <cstdio>
#include <cstring>

#include "hc/diagnostics.h"
#include "hc/knx/telegram_log.h"
#include "hc/knx/tunnel.h"
#include "hc/version.h"
#include "hc/web_api.h"

namespace hc {

namespace {

/// Bytes kept free for what follows the task list (the loop member and the closing).
constexpr size_t kTaskListReserve = 400;

/// Writes the facts of a visitor as members (with nullptr: only counts them, so that an empty
/// report can be written as "available": false before anything is written).
class FactWriter : public DiagnosticsVisitor {
public:
	explicit FactWriter(JsonWriter* json) : json_(json) {}

	void number(const char* key, uint64_t value) override {
		if (json_ != nullptr) {
			json_->member(key, value);
		}
		++count;
	}
	void text(const char* key, const char* value) override {
		if (json_ != nullptr) {
			json_->member(key, value);
		}
		++count;
	}
	void flag(const char* key, bool value) override {
		if (json_ != nullptr) {
			json_->member(key, value);
		}
		++count;
	}

	size_t count = 0;

private:
	JsonWriter* json_;
};

/// Writes the tasks as array elements until the buffer's reserve is reached; remembers the calling
/// task's stack (the event loop's).
class TaskWriter : public TaskVisitor {
public:
	TaskWriter(JsonWriter& json, size_t capacity) : json_(json), capacity_(capacity) {}

	void task(const TaskInfo& info) override {
		++count;
		if (info.current) {
			loop_stack_free = info.stack_free;
		}
		if (info.cpu_permille >= 0) {
			cpu = true;
		}
		if (json_.length() + kTaskListReserve >= capacity_) {
			++omitted;
			return;
		}
		json_.begin_object().member("name", info.name).member("state", task_state_name(info.state));
		if (info.priority >= 0) {
			json_.member("priority", info.priority);
		}
		if (info.stack_free >= 0) {
			json_.member("stack_free", info.stack_free);
		}
		if (info.cpu_permille >= 0) {
			json_.member("cpu_permille", info.cpu_permille);
		}
		if (info.core >= 0) {
			json_.member("core", info.core);
		}
		if (info.current) {
			json_.member("current", true);
		}
		json_.end_object();
	}

	uint32_t count = 0;
	uint32_t omitted = 0;
	bool cpu = false;
	int64_t loop_stack_free = -1;

private:
	JsonWriter& json_;
	size_t capacity_;
};

void unavailable(JsonWriter& json, const char* reason) {
	json.member("available", false).member("reason", reason != nullptr ? reason : "");
}

} // namespace

const char* task_state_name(TaskState state) {
	switch (state) {
	case TaskState::running: return "running";
	case TaskState::ready: return "ready";
	case TaskState::blocked: return "blocked";
	case TaskState::suspended: return "suspended";
	case TaskState::deleted: return "deleted";
	case TaskState::unknown: return "unknown";
	}
	return "unknown";
}

void WebApi::diagnostics(const WebRequest& request, WebResponse& response) {
	if (request.method != HttpMethod::get) {
		refuse(response, 405, "api.method_not_allowed", "use GET");
		return;
	}
	Controller& ctl = controller_;
	const SystemInfo& system = ctl.system();
	EventLoop& loop = ctl.loop();
	const Connectivity& net = ctl.connectivity();
	uint64_t now = ctl.clock().monotonic_ms();
	JsonWriter json(response.body, response.capacity);
	json.begin_object();
	json.member("uptime_s", ctl.uptime_ms() / 1000).member("now_ms", now);

	// ---- Chip and firmware ----
	json.key("device").begin_object()
		.member("name", host_.device_name())
		.member("version", kVersion)
		.member("build", build_id())
		.member("ui", host_.ui_version())
		.end_object();
	write_boot_status(json, ctl.diagnostics());
	json.key("update").begin_object();
	host_.write_update_state(json);
	json.end_object();
	json.key("chip").begin_object();
	{
		FactWriter count(nullptr);
		system.visit_chip(count);
		if (count.count > 0) {
			json.member("available", true);
			FactWriter facts(&json);
			system.visit_chip(facts);
		} else {
			unavailable(json, "this platform reports no chip facts");
		}
	}
	json.end_object();

	// ---- What the console's status adds: LED, time, Improv ----
	json.member("led", led_status_name(ctl.led_status()));
	char local[64];
	bool synchronized = ctl.format_wall_time(local, sizeof(local));
	json.key("time").begin_object().member("synchronized", synchronized).member("local", local).end_object();
	const Improv& improv = ctl.improv();
	json.key("improv").begin_object().member("active", improv.active());
	if (improv.active()) {
		uint64_t ends = improv.ends_at();
		json.member("state", improv::state_name(improv.state()));
		json.member("ends_in_s", ends > now ? (ends - now) / 1000 : 0);
	}
	json.end_object();

	// ---- Memory and temperature ----
	MemoryInfo memory = system.memory();
	json.key("memory").begin_object().member("available", memory.free_bytes != 0);
	if (memory.free_bytes != 0) {
		json.member("free", memory.free_bytes).member("min_free", memory.min_free_bytes);
		if (memory.largest_block != 0) {
			json.member("largest", memory.largest_block);
		}
		if (memory.total_bytes != 0) {
			json.member("total", memory.total_bytes);
		}
	} else {
		json.member("reason", "the platform reports no heap figures");
	}
	if (memory.resident_bytes != 0) {
		json.member("resident", memory.resident_bytes);
	}
	json.end_object();
	json.key("temperature").begin_object();
	{
		Temperature temperature;
		const char* reason = nullptr;
		if (system.temperature(temperature, &reason)) {
			json.member("available", true).member("celsius", temperature.celsius);
			if (temperature.accuracy > 0) {
				json.member("accuracy", temperature.accuracy);
			}
			json.member("source", temperature.source);
		} else {
			unavailable(json, reason);
		}
	}
	json.end_object();

	// ---- Network: the core's view, then the driver's link details ----
	json.key("network").begin_object()
		.member("state", link_state_name(net.state()))
		.member("ip", net.is_up() ? net.ip() : "")
		.member("mode", network_mode_name(net.mode()))
		.member("pairing", net.pairing_active())
		.member("reason", net.reason())
		.member("failures", net.failures())
		.member("link_ups", net.link_ups())
		.member("reconnects", net.link_ups() > 0 ? net.link_ups() - 1 : 0);
	if (net.is_up()) {
		json.member("up_for_s", (now - net.up_since_ms()) / 1000);
	}
	json.end_object();
	json.key("wifi").begin_object();
	{
		LinkDetails link;
		const char* reason = nullptr;
		if (ctl.connectivity().driver().link_details(link, &reason)) {
			json.member("available", true).member("ssid", link.ssid);
			if (link.channel != 0) {
				json.member("channel", static_cast<uint32_t>(link.channel));
			}
			if (link.has_rssi) {
				json.member("rssi", static_cast<int32_t>(link.rssi));
			}
			if (link.has_tx_power) {
				json.member("tx_power_dbm", link.tx_power_dbm);
			}
			json.member("power_save", link.power_save).member("mac", link.mac).member("gateway", link.gateway)
				.member("netmask", link.netmask).member("dns", link.dns);
		} else {
			unavailable(json, reason);
		}
	}
	json.end_object();

	// ---- Storage: the blob store's file system and the settings store ----
	json.key("storage").begin_object();
	{
		uint64_t total = 0;
		uint64_t used = 0;
		bool usage = host_.storage_usage(total, used);
		json.member("available", usage || blobs_ != nullptr);
		if (usage) {
			json.member("total", total).member("used", used).member("free", total > used ? total - used : 0);
		}
		if (blobs_ != nullptr) {
			struct Counter : BlobVisitor {
				void blob(const char*, size_t size) override {
					++files;
					bytes += size;
				}
				uint32_t files = 0;
				uint64_t bytes = 0;
			} counter;
			blobs_->visit(counter);
			json.member("files", counter.files).member("bytes", counter.bytes);
		}
		json.key("nvs").begin_object();
		const char* reason = nullptr;
		FactWriter count(nullptr);
		if (system.visit_settings_store(count, &reason)) {
			json.member("available", true);
			FactWriter facts(&json);
			system.visit_settings_store(facts, &reason);
		} else {
			unavailable(json, reason);
		}
		json.end_object();
	}
	json.end_object();

	// ---- Buses and radios ----
	json.key("knx").begin_object();
	if (knx_ != nullptr) {
		json.member("available", true)
			.member("state", knx::tunnel_state_name(knx_->state()))
			.member("reason", knx_->reason())
			.member("connections", knx_->connections())
			.member("reconnects", knx_->connections() > 0 ? knx_->connections() - 1 : 0)
			.member("attempts", knx_->attempts())
			.member("queued", static_cast<uint32_t>(knx_->queued()));
		if (knx_->is_up()) {
			json.member("channel", static_cast<uint32_t>(knx_->channel()));
		}
		if (telegrams_ != nullptr) {
			json.member("telegrams", telegrams_->total());
		}
	} else {
		unavailable(json, "this build has no KNX tunnel");
	}
	json.end_object();
	json.key("zigbee").begin_object();
	if (zigbee_ != nullptr) {
		const zigbee::NetworkStatus& network = zigbee_->network();
		json.member("available", true).member("up", network.up);
		if (network.up) {
			json.member("channel", static_cast<uint32_t>(network.channel))
				.member("pan_id", static_cast<uint32_t>(network.pan_id));
		}
		json.member("devices", static_cast<uint32_t>(zigbee_->devices().size()));
	} else {
		unavailable(json, zigbee_reason_ != nullptr ? zigbee_reason_ : "this build has no Zigbee coordinator");
	}
	json.end_object();

	// ---- The console output queue (issue #46) ----
	json.key("console").begin_object();
	{
		ConsoleOutputInfo console;
		const char* reason = nullptr;
		if (system.console_output(console, &reason)) {
			json.member("available", true)
				.member("host", console.host)
				.member("dropped", console.dropped)
				.member("dropped_unread", console.dropped_unread)
				.member("queued", console.queued)
				.member("capacity", console.capacity);
		} else {
			unavailable(json, reason);
		}
	}
	json.end_object();

	// ---- Tasks (last but one: the list stops at the buffer's reserve) ----
	json.key("tasks").begin_object();
	TaskWriter tasks(json, response.capacity);
	{
		const char* reason = nullptr;
		json.key("list").begin_array();
		bool listed = system.visit_tasks(tasks, &reason);
		json.end_array();
		if (listed) {
			json.member("available", true).member("count", tasks.count).member("cpu", tasks.cpu);
			if (tasks.omitted > 0) {
				json.member("omitted", tasks.omitted);
			}
		} else {
			unavailable(json, reason);
		}
	}
	json.end_object();

	// ---- The event loop's own figures ----
	json.key("loop").begin_object()
		.member("missed", loop.missed_periods())
		.member("longest_turn_ms", loop.longest_turn_ms())
		.member("longest_turn_at_ms", loop.longest_turn_at_ms());
	if (loop.longest_turn_at_ms() != 0 || loop.longest_turn_ms() != 0) {
		json.member("longest_turn_ago_s", (now - loop.longest_turn_at_ms()) / 1000);
	}
	json.member("timers", static_cast<uint32_t>(loop.timer_count()))
		.member("timer_capacity", static_cast<uint32_t>(EventLoop::kTimerCapacity));
	if (tasks.loop_stack_free >= 0) {
		json.member("stack_free", tasks.loop_stack_free);
	}
	json.end_object();

	json.end_object();
	finish(response, json);
}

} // namespace hc
