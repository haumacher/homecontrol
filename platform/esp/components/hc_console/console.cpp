/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "console.h"
#include "console_output.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hc/diagnostics.h"
#include "hc/log_ring.h"
#include "hc/network_settings.h"
#include "linenoise/linenoise.h"
#include "nvs_flash.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.console";

/// How long the REPL waits for a command to run on the event loop before it gives up waiting
/// (the command still runs and prints when the loop gets to it).
constexpr TickType_t kCommandTimeout = pdMS_TO_TICKS(10000);

ConsoleContext* g_context = nullptr;

// ---- Commands (event-loop task) --------------------------------------------------------------

int usage(const char* text) {
	std::printf("usage: %s\n", text);
	return 1;
}

/// The reset reason and the platform's boot facts, one line.
void print_boot(ConsoleContext& c) {
	const Diagnostics* diagnostics = c.controller.diagnostics();
	if (diagnostics == nullptr) {
		std::printf("boot      reset reason unknown (no diagnostics)\n");
		return;
	}
	char facts[160];
	format_boot_facts(*diagnostics, facts, sizeof(facts));
	std::printf("boot      %s%s%s\n", reset_reason_name(diagnostics->reset_reason()), facts[0] != '\0' ? "; " : "", facts);
}

/// Name, priority and stack high-water mark (bytes never used) of every task.
void print_tasks() {
#if configUSE_TRACE_FACILITY
	// On the heap for the command only (a console command is not a steady-state path).
	constexpr UBaseType_t kMaxTasks = 40;
	std::unique_ptr<TaskStatus_t[]> tasks(new (std::nothrow) TaskStatus_t[kMaxTasks]);
	if (!tasks) {
		std::printf("tasks     not listed: no heap for %u task records\n", static_cast<unsigned>(kMaxTasks));
		return;
	}
	UBaseType_t count = uxTaskGetSystemState(tasks.get(), kMaxTasks, nullptr);
	if (count == 0) {
		std::printf("tasks     more than %u, not listed\n", static_cast<unsigned>(kMaxTasks));
		return;
	}
	std::printf("tasks     %u (name, priority, stack never used)\n", static_cast<unsigned>(count));
	for (UBaseType_t i = 0; i < count; ++i) {
		std::printf("  %-16s %2u %6u B\n", tasks[i].pcTaskName, static_cast<unsigned>(tasks[i].uxCurrentPriority),
			static_cast<unsigned>(tasks[i].usStackHighWaterMark));
	}
#else
	std::printf("tasks     not listed: the build lacks CONFIG_FREERTOS_USE_TRACE_FACILITY\n");
#endif
}

/// The console output queue (issue #46): what it holds and what it had to drop.
void print_console_output(ConsoleContext& c) {
	ConsoleOutputInfo out;
	const char* reason = nullptr;
	if (!c.controller.system().console_output(out, &reason)) {
		std::printf("console   output queue not running: %s\n", reason);
		return;
	}
	std::printf("console   %llu line(s) dropped (%llu while nobody read the port), %u of %u B queued, USB host %s\n",
		static_cast<unsigned long long>(out.dropped), static_cast<unsigned long long>(out.dropped_unread),
		static_cast<unsigned>(out.queued), static_cast<unsigned>(out.capacity), out.host ? "attached" : "absent");
}

int cmd_status(ConsoleContext& c, int, char**) {
	Controller& ctl = c.controller;
	const Connectivity& net = ctl.connectivity();
	char time_text[64];
	bool synchronized = ctl.format_wall_time(time_text, sizeof(time_text));
	MemoryInfo memory = ctl.system().memory();
	std::printf("uptime    %llu s\n", static_cast<unsigned long long>(ctl.uptime_ms() / 1000));
	std::printf("network   %s%s%s\n", link_state_name(net.state()), net.is_up() ? " " : "", net.ip());
	std::printf("led       %s\n", led_status_name(ctl.led_status()));
	std::printf("time      %s, %s\n", time_text, synchronized ? "synchronized" : "NOT synchronized");
	if (net.mode() != NetworkMode::normal) {
		std::printf("mode      %s\n", network_mode_name(net.mode()));
	}
	if (net.pairing_active()) {
		std::printf("wifi      WPS active\n");
	}
	std::printf("heap      %u B free (min %u B, largest block %u B)\n", static_cast<unsigned>(memory.free_bytes),
		static_cast<unsigned>(memory.min_free_bytes),
		static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
	print_boot(c);
	print_console_output(c);
	print_tasks();
	const Improv& improv = ctl.improv();
	if (improv.active()) {
		uint64_t now = ctl.clock().monotonic_ms();
		uint64_t ends = improv.ends_at();
		std::printf("improv    active (%s), ends in %llu s\n", improv::state_name(improv.state()),
			static_cast<unsigned long long>(ends > now ? (ends - now) / 1000 : 0));
	} else {
		std::printf("improv    ended\n");
	}
	return 0;
}

int wifi_status(ConsoleContext& c) {
	const Connectivity& net = c.controller.connectivity();
	char ssid[setting::kMaxSsidLength + 1];
	SettingsStatus status = c.controller.settings().get_string(setting::kWifiSsid, ssid, sizeof(ssid));
	if (status == SettingsStatus::ok) {
		std::printf("ssid      \"%s\"\n", ssid);
	} else {
		std::printf("ssid      none stored (%s): wifi set <ssid> <psk>\n", settings_status_name(status));
	}
	std::printf("state     %s%s%s%s\n", link_state_name(net.state()), net.is_up() ? " " : "", net.ip(),
		net.pairing_active() ? ", WPS active" : "");
	std::printf("mode      %s%s\n", network_mode_name(net.mode()), net.paused() ? " (paused for a scan)" : "");
	if (net.reason()[0] != '\0') {
		std::printf("reason    %s\n", net.reason());
	}
	int8_t rssi = 0;
	if (c.wifi.rssi(rssi)) {
		std::printf("rssi      %d dBm\n", static_cast<int>(rssi));
	}
	std::printf("failures  %u\n", static_cast<unsigned>(net.failures()));
	if (net.next_action_at() != EventLoop::kNever) {
		uint64_t now = c.controller.clock().monotonic_ms();
		std::printf("next step in %llu ms\n",
			static_cast<unsigned long long>(net.next_action_at() > now ? net.next_action_at() - now : 0));
	}
	std::printf("hostname  %s.local (mDNS %s)\n", c.wifi.hostname(), c.services.mdns_running() ? "announced" : "not yet");
	std::printf("ntp       %s\n", c.services.sntp_running() ? c.services.ntp_server() : "not started yet");
	return 0;
}

int cmd_wifi(ConsoleContext& c, int argc, char** argv) {
	const char* kUsage = "wifi wps | wifi set <ssid> [<psk>] | wifi clear | wifi status | wifi ps [none|min|max]";
	if (argc < 2) {
		return usage(kUsage);
	}
	Settings& settings = c.controller.settings();
	if (std::strcmp(argv[1], "status") == 0 && argc == 2) {
		return wifi_status(c);
	}
	if (std::strcmp(argv[1], "clear") == 0 && argc == 2) {
		SettingsStatus a = settings.erase(setting::kWifiSsid);
		SettingsStatus b = settings.erase(setting::kWifiPsk);
		if ((a != SettingsStatus::ok && a != SettingsStatus::not_found) ||
			(b != SettingsStatus::ok && b != SettingsStatus::not_found)) {
			std::printf("error: cannot erase the credentials (%s)\n",
				settings_status_name(a != SettingsStatus::ok && a != SettingsStatus::not_found ? a : b));
			return 1;
		}
		c.controller.connectivity().request_connect("WiFi credentials cleared");
		std::printf("WiFi credentials cleared, disconnected; listening for WPS\n");
		return 0;
	}
	if (std::strcmp(argv[1], "ps") == 0 && (argc == 2 || argc == 3)) {
		if (argc == 2) {
			std::printf("WiFi power save %s (setting %s: none | min | max; doc/network.md)\n",
				wifi_power_save_name(read_wifi_power_save(settings, "console")), setting::kWifiPowerSave);
			return 0;
		}
		const char* error = nullptr;
		if (!store_wifi_power_save(settings, argv[2], &error)) {
			std::printf("error: %s\n", error);
			return 1;
		}
		c.controller.connectivity().driver().power_save_changed();
		std::printf("WiFi power save %s, applied to the running link\n", argv[2]);
		return 0;
	}
	if (std::strcmp(argv[1], "wps") == 0 && argc == 2) {
		if (!c.controller.connectivity().request_pairing("console")) {
			std::printf("error: WPS not started (see the log line above)\n");
			return 1;
		}
		std::printf("WPS requested: press the WPS button on the router within 2 minutes\n");
		return 0;
	}
	if (std::strcmp(argv[1], "set") == 0 && (argc == 3 || argc == 4)) {
		const char* ssid = argv[2];
		const char* psk = argc == 4 ? argv[3] : nullptr;
		const char* error = nullptr;
		if (!store_wifi_credentials(settings, ssid, psk, &error)) {
			std::printf("error: credentials refused: %s\n", error); // never echo the key itself
			return 1;
		}
		std::printf("stored SSID \"%s\" and %s; connecting\n", ssid,
			psk != nullptr ? "the key (not shown)" : "no key (open network)");
		c.controller.connectivity().request_connect("new WiFi credentials");
		return 0;
	}
	return usage(kUsage);
}

int cmd_tz(ConsoleContext& c, int argc, char** argv) {
	if (argc != 3 || std::strcmp(argv[1], "set") != 0) {
		std::printf("time zone %s\n", c.controller.time_zone());
		return usage("tz set <posix-tz>   e.g. tz set CET-1CEST,M3.5.0,M10.5.0/3");
	}
	const char* error = nullptr;
	if (!c.controller.set_time_zone(argv[2], &error)) {
		std::printf("error: %s\n", error);
		return 1;
	}
	char now[64];
	c.controller.format_wall_time(now, sizeof(now));
	std::printf("time zone %s, local time %s\n", c.controller.time_zone(), now);
	return 0;
}

int cmd_ntp(ConsoleContext& c, int argc, char** argv) {
	if (argc != 3 || std::strcmp(argv[1], "set") != 0) {
		return usage("ntp set <server>");
	}
	const char* error = nullptr;
	if (!host_name_valid(argv[2], &error)) {
		std::printf("error: %s\n", error);
		return 1;
	}
	SettingsStatus status = c.controller.settings().set_string(setting::kNtpServer, argv[2]);
	if (status != SettingsStatus::ok) {
		std::printf("error: cannot store the server (%s)\n", settings_status_name(status));
		return 1;
	}
	ESP_LOGI(kTag, "NTP server set to %s", argv[2]);
	if (c.services.sntp_running()) {
		c.services.restart_sntp();
		std::printf("NTP server %s, SNTP restarted\n", argv[2]);
	} else {
		std::printf("NTP server %s, used when the network comes up\n", argv[2]);
	}
	return 0;
}

struct SettingsPrinter : SettingsVisitor {
	explicit SettingsPrinter(const Settings& settings_) : settings(settings_) {}

	void entry(const char* key, SettingType type) override {
		++count;
		if (setting_is_secret(key)) {
			std::printf("%-15s = ******** (%s, hidden)\n", key, setting_type_name(type));
			return;
		}
		switch (type) {
		case SettingType::string: {
			char value[Settings::kMaxStringLength + 1];
			SettingsStatus status = settings.get_string(key, value, sizeof(value));
			if (status == SettingsStatus::ok) {
				std::printf("%-15s = \"%s\"\n", key, value);
			} else {
				std::printf("%-15s : %s\n", key, settings_status_name(status));
			}
			break;
		}
		case SettingType::integer: {
			int32_t value = 0;
			settings.get_int(key, value);
			std::printf("%-15s = %ld\n", key, static_cast<long>(value));
			break;
		}
		case SettingType::boolean: {
			bool value = false;
			settings.get_bool(key, value);
			std::printf("%-15s = %s\n", key, value ? "true" : "false");
			break;
		}
		}
	}

	const Settings& settings;
	unsigned count = 0;
};

int cmd_settings(ConsoleContext& c, int argc, char** argv) {
	if (argc != 2 || std::strcmp(argv[1], "list") != 0) {
		return usage("settings list");
	}
	SettingsPrinter printer(c.controller.settings());
	c.controller.settings().visit(printer);
	std::printf("%u setting(s); unset: tz defaults to %s, ntp.server to %s\n", printer.count,
		setting::kDefaultTimeZone, setting::kDefaultNtpServer);
	return 0;
}

int cmd_update(ConsoleContext& c, int argc, char** argv) {
	const char* kUsage = "update status | update check | update now [<url>] | update url set <url> | update url clear";
	if (argc < 2) {
		return usage(kUsage);
	}
	const char* error = nullptr;
	if (std::strcmp(argv[1], "status") == 0 && argc == 2) {
		c.ota.print_status();
		return 0;
	}
	if ((std::strcmp(argv[1], "check") == 0 && argc == 2) || (std::strcmp(argv[1], "now") == 0 && argc <= 3)) {
		bool force = argv[1][0] == 'n';
		if (!c.ota.check(force, argc == 3 ? argv[2] : nullptr, &error)) {
			std::printf("error: %s\n", error);
			return 1;
		}
		std::printf("%s; the result follows in the log\n",
			force ? "fetching the manifest, installing its image whatever its version" : "checking for an update");
		return 0;
	}
	if (std::strcmp(argv[1], "url") == 0 && argc == 4 && std::strcmp(argv[2], "set") == 0) {
		if (!c.ota.set_url(argv[3], &error)) {
			std::printf("error: %s\n", error);
			return 1;
		}
		std::printf("update source %s (manifest at <url>/manifest.json, checked daily)\n", argv[3]);
		return 0;
	}
	if (std::strcmp(argv[1], "url") == 0 && argc == 3 && std::strcmp(argv[2], "clear") == 0) {
		if (!c.ota.set_url(nullptr, &error)) {
			std::printf("error: cannot erase update.url (%s)\n", error);
			return 1;
		}
		std::printf("update source cleared: periodic check disabled\n");
		return 0;
	}
	return usage(kUsage);
}

int cmd_restart(ConsoleContext&, int, char**) {
	ESP_LOGW(kTag, "restart requested on the console");
	std::printf("restarting\n");
	console_output().drain(500);
	esp_restart();
	return 0;
}

int cmd_uptime(ConsoleContext& c, int, char**) {
	uint64_t seconds = c.controller.uptime_ms() / 1000;
	const Diagnostics* diagnostics = c.controller.diagnostics();
	std::printf("up %llu d %02u:%02u:%02u (%llu s)", static_cast<unsigned long long>(seconds / 86400),
		static_cast<unsigned>(seconds / 3600 % 24), static_cast<unsigned>(seconds / 60 % 60),
		static_cast<unsigned>(seconds % 60), static_cast<unsigned long long>(seconds));
	if (c.diagnostics != nullptr && c.diagnostics->boot_count() != 0) {
		std::printf(", boot %u", static_cast<unsigned>(c.diagnostics->boot_count()));
	}
	std::printf(", reset reason: %s\n",
		reset_reason_name(diagnostics != nullptr ? diagnostics->reset_reason() : ResetReason::unknown));
	return 0;
}

esp_log_level_t esp_level(LogLevel level) {
	switch (level) {
	case LogLevel::error: return ESP_LOG_ERROR;
	case LogLevel::warn: return ESP_LOG_WARN;
	case LogLevel::info: return ESP_LOG_INFO;
	case LogLevel::debug: return ESP_LOG_DEBUG;
	}
	return ESP_LOG_INFO;
}

/// Prints the ring's lines like the monitor does: "<seq> <ms> <L> <tag>: <message>".
struct TailPrinter : LogLineVisitor {
	bool line(const LogLine& line) override {
		std::printf("%5u %9llu %s %s: %s\n", static_cast<unsigned>(line.seq), static_cast<unsigned long long>(line.ms),
			log_level_letter(line.level), line.tag, line.message);
		return true;
	}
};

int cmd_log(ConsoleContext& c, int argc, char** argv) {
	const char* kUsage = "log level [<tag|*> <error|warn|info|debug>] | log tail [<n>]";
	if (argc >= 2 && std::strcmp(argv[1], "level") == 0 && argc == 2) {
		std::printf("*               %s (the core's threshold for tags without their own)\n", log_level_name(Log::level()));
		for (size_t i = 0; i < Log::tag_level_count(); ++i) {
			const char* tag = nullptr;
			LogLevel level = LogLevel::info;
			if (Log::tag_level_at(i, tag, level)) {
				std::printf("%-15s %s\n", tag, log_level_name(level));
			}
		}
		return 0;
	}
	if (argc == 4 && std::strcmp(argv[1], "level") == 0) {
		LogLevel level = LogLevel::info;
		if (!log_level_parse(argv[3], level)) {
			std::printf("error: unknown level \"%s\" (error, warn, info, debug)\n", argv[3]);
			return 1;
		}
		const char* tag = argv[2];
		bool core_tag = std::strcmp(tag, "*") == 0 || std::strncmp(tag, "hc.", 3) == 0;
		if (core_tag) {
			const char* error = nullptr;
			if (!Log::set_tag_level(tag, level, &error)) {
				std::printf("error: %s\n", error);
				return 1;
			}
		}
		esp_log_level_set(tag, esp_level(level));
		ESP_LOGI(kTag, "log level of %s set to %s on the console", tag, log_level_name(level));
		std::printf("%s: %s%s\n", tag, log_level_name(level),
			level == LogLevel::debug && !core_tag
				? " (IDF components print debug lines only in a build with CONFIG_LOG_MAXIMUM_LEVEL_DEBUG)"
				: "");
		return 0;
	}
	if (argc >= 2 && argc <= 3 && std::strcmp(argv[1], "tail") == 0) {
		unsigned long n = 20;
		if (argc == 3) {
			char* end = nullptr;
			n = std::strtoul(argv[2], &end, 10);
			if (*end != '\0' || n == 0) {
				return usage(kUsage);
			}
		}
		// A consistent copy (a writer on another task may hold the ring for a moment), on the heap for
		// the command only.
		std::unique_ptr<LogRing> ring(new (std::nothrow) LogRing);
		if (!ring) {
			std::printf("error: no heap for a copy of the log ring (%u B)\n", static_cast<unsigned>(sizeof(LogRing)));
			return 1;
		}
		LogRing& copy = *ring;
		bool copied = false;
		for (int attempt = 0; attempt < 10 && !copied; ++attempt) {
			copied = c.controller.log_ring().copy_to(copy);
			if (!copied) {
				vTaskDelay(1);
			}
		}
		if (!copied) {
			std::printf("error: the log ring stayed busy; try again\n");
			return 1;
		}
		TailPrinter printer;
		size_t shown = copy.read_tail(n, printer);
		std::printf("%u of %u line(s) held (%u dropped while busy); the ring keeps the last %u bytes\n",
			static_cast<unsigned>(shown), static_cast<unsigned>(copy.count()), static_cast<unsigned>(copy.dropped()),
			static_cast<unsigned>(LogRing::kCapacity));
		return 0;
	}
	return usage(kUsage);
}

int cmd_coredump(ConsoleContext& c, int argc, char** argv) {
	const char* kUsage = "coredump status | coredump clear";
	if (c.diagnostics == nullptr) {
		std::printf("error: no diagnostics in this build\n");
		return 1;
	}
	if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
		c.diagnostics->print_coredump_status();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "clear") == 0) {
		if (!c.diagnostics->coredump_present()) {
			std::printf("no core dump stored; nothing to clear\n");
			return 0;
		}
		if (!c.diagnostics->coredump_clear()) {
			std::printf("error: the core dump could not be erased (see the log line above)\n");
			return 1;
		}
		std::printf("core dump erased\n");
		return 0;
	}
	return usage(kUsage);
}

int cmd_crash(ConsoleContext&, int argc, char** argv) {
	if (argc == 2 && std::strcmp(argv[1], "yes") == 0) {
		ESP_LOGW(kTag, "crash requested on the console: storing to address 0 (panic, core dump, restart)");
		std::fflush(stdout);
		vTaskDelay(pdMS_TO_TICKS(100)); // let the log line out
		volatile uint32_t* volatile address = nullptr; // volatile: the compiler cannot see it is null
		*address = 0xdead;
		return 0;
	}
	if (argc == 3 && std::strcmp(argv[1], "stall") == 0 && std::strcmp(argv[2], "yes") == 0) {
		ESP_LOGW(kTag, "stall requested on the console: the event loop spins until the task watchdog resets");
		std::fflush(stdout);
		vTaskDelay(pdMS_TO_TICKS(100));
		for (;;) {
			// A stuck loop, without feeding the watchdog or yielding.
		}
	}
	return usage("crash yes | crash stall yes   (tests the core dump resp. the task watchdog: the device restarts)");
}

/// Collects up to kBatch blob names (the visitor must not remove while visiting).
struct BlobNames : BlobVisitor {
	static constexpr size_t kBatch = 16;
	void blob(const char* name, size_t) override {
		if (count < kBatch) {
			std::snprintf(names[count++], sizeof(names[0]), "%s", name);
		}
	}
	char names[kBatch][BlobStore::kMaxNameLength + 1] = {};
	size_t count = 0;
};

/// Erases one data partition by label; absent partitions are skipped.
void erase_partition(const char* label) {
	const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
	if (partition == nullptr) {
		return;
	}
	esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
	std::printf("%-10s %s\n", label, err == ESP_OK ? "erased" : esp_err_to_name(err));
}

int cmd_factory(ConsoleContext& c, int argc, char** argv) {
	if (argc != 3 || std::strcmp(argv[1], "reset") != 0 || std::strcmp(argv[2], "yes") != 0) {
		return usage("factory reset yes   (erases the settings with the WiFi credentials, the stored runtime image and "
			"its state, the Zigbee network; then restarts)");
	}
	factory_reset_and_restart(c.blobs, "on the console");
}

// ---- The command table: the one place where commands are registered --------------------------

struct Command {
	const char* group;
	const char* name;
	const char* hint;
	const char* help;
	int (*run)(ConsoleContext& context, int argc, char** argv);
};

/// In the order `help` lists them; commands of a group stand together.
const Command kCommands[] = {
	{"system", "status", nullptr,
		"Uptime, boot record, network state and address, local time and whether it is synchronized, heap, "
		"tasks with their stack reserve",
		&cmd_status},
	{"system", "uptime", nullptr, "Uptime, boot count and reset reason in one line", &cmd_uptime},
	{"system", "settings", "list", "All stored settings (wifi.psk masked)", &cmd_settings},
	{"system", "restart", nullptr, "Restart the controller", &cmd_restart},
	{"system", "factory", "reset yes",
		"Erase the settings (WiFi credentials included), the stored runtime image and its state, and the Zigbee "
		"network, then restart",
		&cmd_factory},
	{"network", "wifi", "wps | set <ssid> [<psk>] | clear | status | ps [none|min|max]",
		"WiFi: WPS push-button provisioning, credentials by hand (stored in NVS; the key is never shown; quote "
		"an SSID with spaces), link state, power save (none: lowest latency; min: the 802.15.4 radio gets the air "
		"while WiFi sleeps)",
		&cmd_wifi},
	{"network", "ntp", "set <server>", "SNTP server (default pool.ntp.org)", &cmd_ntp},
	{"time", "tz", "set <posix-tz>", "Time zone as a POSIX TZ string (default CET-1CEST,M3.5.0,M10.5.0/3)", &cmd_tz},
	{"update", "update", "status | check | now [<url>] | url set <url> | url clear",
		"Firmware update (doc/ota.md): state, check the update source now, install from a source whatever the "
		"version (development), set or clear the update source (update.url)",
		&cmd_update},
	{"diagnostics", "log", "level [<tag|*> <level>] | tail [<n>]",
		"Log levels per tag at run time (* sets all and drops the per-tag levels; hc.* tags also filter the "
		"log ring), the last n lines of the log ring (default 20)",
		&cmd_log},
	{"diagnostics", "coredump", "status | clear",
		"The core dump of the last crash (doc/diagnostics.md): summary, erase; download it with GET /api/coredump",
		&cmd_coredump},
	{"diagnostics", "crash", "yes | stall yes",
		"Test the crash pipeline: a panic (core dump, restart) or a stuck event loop (task watchdog reset)",
		&cmd_crash},
};

// ---- Running a command on the event loop -----------------------------------------------------

constexpr int kMaxArgs = 8;
constexpr size_t kMaxLine = 256;
constexpr int kHistory = 32;

/// The REPL task: the line editor and the dispatch (command bodies run on the event loop). 1832 B
/// used on the C6 (2026-11-03 and 2026-10-18 alike: the start, its terminal probe and the command
/// registry). 3584 B: the commands print through printf, and a printf of the ROM reserves 1.25 KB at once (_svfprintf_r 1072 + snprintf 176 B), mostly never written, so the high-water mark does not show it (#48). 4096 B (the IDF REPL's
/// default) before issue #45.
constexpr uint32_t kReplStack = 3584;
constexpr UBaseType_t kReplPriority = 2;

/// The one command in flight: the REPL handles a line at a time, so a single slot suffices. If
/// a command outlives kCommandTimeout, the slot stays busy and the next command is refused until
/// it finishes.
struct PendingCommand {
	const Command* command = nullptr;
	const ConsoleCommand* extension = nullptr;
	int argc = 0;
	char* argv[kMaxArgs] = {};
	char storage[kMaxLine] = {};
	int result = 0;
	uint32_t sequence = 0;
	std::atomic<uint32_t> done{0};
	std::atomic<bool> busy{false};
};

PendingCommand g_pending;
StaticSemaphore_t g_done_storage;
SemaphoreHandle_t g_done = nullptr;

void run_pending(void*, const LoopMessage&) {
	g_pending.result = g_pending.command != nullptr
		? g_pending.command->run(*g_context, g_pending.argc, g_pending.argv)
		: g_pending.extension->run(g_pending.extension->context, g_pending.argc, g_pending.argv);
	std::fflush(stdout);
	g_pending.done.store(g_pending.sequence);
	g_pending.busy.store(false);
	xSemaphoreGive(g_done);
}

/// Removes the line just entered from the line editor's history, so that the up arrow cannot
/// bring back a WiFi key. (linenoiseHistoryFree() would leave a stale length behind.)
void forget_history() {
	linenoiseHistorySetMaxLen(1);
	linenoiseHistoryAdd("wifi status");
	linenoiseHistorySetMaxLen(32);
}

/// REPL task: copies the arguments, hands the command (built-in or extension) to the event loop and
/// waits for it.
int submit(const Command* command, const ConsoleCommand* extension, int argc, char** argv) {
	if (argc > kMaxArgs) {
		std::printf("error: at most %d words per command\n", kMaxArgs);
		return 1;
	}
	if (g_pending.busy.load()) {
		std::printf("error: the previous command is still running on the event loop; try again\n");
		return 1;
	}
	size_t used = 0;
	for (int i = 0; i < argc; ++i) {
		size_t length = std::strlen(argv[i]) + 1;
		if (used + length > kMaxLine) {
			std::printf("error: command line longer than %u bytes\n", static_cast<unsigned>(kMaxLine));
			return 1;
		}
		std::memcpy(g_pending.storage + used, argv[i], length);
		g_pending.argv[i] = g_pending.storage + used;
		used += length;
	}
	g_pending.command = command;
	g_pending.extension = extension;
	g_pending.argc = argc;
	uint32_t sequence = ++g_pending.sequence;
	g_pending.busy.store(true);
	if (!g_context->queue.post(&run_pending, nullptr, 0, nullptr, "console command")) {
		g_pending.busy.store(false);
		std::printf("error: the event loop queue is full; try again\n");
		return 1;
	}
	TickType_t start = xTaskGetTickCount();
	while (g_pending.done.load() != sequence) {
		TickType_t waited = xTaskGetTickCount() - start;
		if (waited >= kCommandTimeout ||
			xSemaphoreTake(g_done, kCommandTimeout - waited) != pdTRUE) {
			if (g_pending.done.load() == sequence) {
				break;
			}
			std::printf("error: the event loop did not run the command within %u s\n",
				static_cast<unsigned>(pdTICKS_TO_MS(kCommandTimeout) / 1000));
			return 1;
		}
	}
	return g_pending.result;
}

int dispatch(void* context, int argc, char** argv) {
	const Command* command = static_cast<const Command*>(context);
	if (std::strcmp(command->name, "wifi") == 0 && argc >= 2 && std::strcmp(argv[1], "set") == 0) {
		forget_history();
	}
	return submit(command, nullptr, argc, argv);
}

int dispatch_extension(void* context, int argc, char** argv) {
	return submit(nullptr, static_cast<const ConsoleCommand*>(context), argc, argv);
}

/// Prints one command: name and arguments, then its help indented.
void print_command(const char* name, const char* hint, const char* help) {
	std::printf("  %s%s%s\n      %s\n", name, hint != nullptr ? " " : "", hint != nullptr ? hint : "", help);
}

/// `help [<command>]`: the commands grouped by component (runs in the REPL task, reads only
/// constant tables).
int cmd_help(int argc, char** argv) {
	const char* only = argc >= 2 ? argv[1] : nullptr;
	bool found = false;
	const char* group = nullptr;
	for (const Command& command : kCommands) {
		if (only != nullptr && std::strcmp(only, command.name) != 0) {
			continue;
		}
		if (only == nullptr && (group == nullptr || std::strcmp(group, command.group) != 0)) {
			group = command.group;
			std::printf("%s\n", group);
		}
		print_command(command.name, command.hint, command.help);
		found = true;
	}
	bool heading = false;
	for (size_t i = 0; i < g_context->extension_count; ++i) {
		const ConsoleCommand& command = g_context->extensions[i];
		if (only != nullptr && std::strcmp(only, command.name) != 0) {
			continue;
		}
		if (only == nullptr && !heading) {
			std::printf("components\n");
			heading = true;
		}
		print_command(command.name, command.hint, command.help);
		found = true;
	}
	if (!found) {
		std::printf("no command \"%s\"; 'help' lists them all\n", only);
		return 1;
	}
	if (only == nullptr) {
		std::printf("'help <command>' shows one; commands run on the event loop (doc/diagnostics.md)\n");
	}
	return 0;
}

/// The registry and the line editor (the REPL task, before its first prompt).
bool console_init() {
	esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
	console_config.max_cmdline_length = kMaxLine;
#if CONFIG_LOG_COLORS
	console_config.hint_color = 36; // cyan
#else
	console_config.hint_color = -1;
#endif
	esp_err_t err = esp_console_init(&console_config);
	if (err == ESP_OK) {
		esp_console_cmd_t descriptor = {};
		descriptor.command = "help";
		descriptor.help = "The commands, grouped by component; 'help <command>' shows one";
		descriptor.hint = "[<command>]";
		descriptor.func = &cmd_help;
		err = esp_console_cmd_register(&descriptor);
	}
	for (const Command& command : kCommands) {
		if (err != ESP_OK) {
			break;
		}
		esp_console_cmd_t descriptor = {};
		descriptor.command = command.name;
		descriptor.help = command.help;
		descriptor.hint = command.hint;
		descriptor.func_w_context = &dispatch;
		descriptor.context = const_cast<Command*>(&command);
		err = esp_console_cmd_register(&descriptor);
	}
	for (size_t i = 0; i < g_context->extension_count && err == ESP_OK; ++i) {
		const ConsoleCommand& command = g_context->extensions[i];
		esp_console_cmd_t descriptor = {};
		descriptor.command = command.name;
		descriptor.help = command.help;
		descriptor.hint = command.hint;
		descriptor.func_w_context = &dispatch_extension;
		descriptor.context = const_cast<ConsoleCommand*>(&command);
		err = esp_console_cmd_register(&descriptor);
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "console not started: %s", esp_err_to_name(err));
		return false;
	}
	linenoiseSetMultiLine(1);
	linenoiseSetCompletionCallback(&esp_console_get_completion);
	linenoiseSetHintsCallback(reinterpret_cast<linenoiseHintsCallback*>(&esp_console_get_hint));
	linenoiseHistorySetMaxLen(kHistory);
	linenoiseSetMaxLineLen(kMaxLine);
	return true;
}

/// The REPL (the IDF's esp_console_repl_task, reading the shared port's console device). Sets the
/// console up first, so none of that runs on the event loop.
void repl_task(void*) {
	if (!console_init()) {
		vTaskDelete(nullptr);
		return;
	}
	// stdin is per task: only the REPL reads the console device.
	stdin = std::fopen(g_context->input_device, "r");
	if (stdin == nullptr) {
		ESP_LOGE(kTag, "console not started: cannot open %s", g_context->input_device);
		vTaskDelete(nullptr);
		return;
	}
	setvbuf(stdin, nullptr, _IONBF, 0);
	const char* prompt = LOG_COLOR_I "hc> " LOG_RESET_COLOR;
	// Does the terminal answer escape sequences? Otherwise no line editing (and no colors). Waits up
	// to 500 ms for the answer - here, not on the event loop.
	if (linenoiseProbe() != 0) {
		linenoiseSetDumbMode(1);
		prompt = "hc> ";
	}
	ESP_LOGI(kTag, "console ready on USB Serial/JTAG: type 'help'");
	for (;;) {
		char* line = linenoise(prompt);
		if (line == nullptr) {
			continue; // empty line
		}
		linenoiseHistoryAdd(line);
		int result = 0;
		esp_err_t err = esp_console_run(line, &result);
		if (err == ESP_ERR_NOT_FOUND) {
			std::printf("Unrecognized command ('help' lists them)\n");
		} else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) { // INVALID_ARG: empty command
			std::printf("Internal error: %s\n", esp_err_to_name(err));
		}
		linenoiseFree(line);
	}
}

} // namespace

void factory_reset_and_restart(BlobStore* blobs, const char* origin) {
	ESP_LOGW(kTag, "factory reset requested %s", origin);
	if (blobs != nullptr) {
		unsigned removed = 0;
		unsigned failed = 0;
		for (;;) {
			BlobNames batch;
			blobs->visit(batch);
			if (batch.count == 0) {
				break;
			}
			unsigned before = removed;
			for (size_t i = 0; i < batch.count; ++i) {
				if (blobs->remove(batch.names[i]) == BlobStatus::ok) {
					++removed;
				} else {
					++failed;
				}
			}
			if (removed == before) {
				break; // nothing removable left
			}
		}
		std::printf("state      %u blob(s) removed%s\n", removed, failed != 0 ? ", some could not be removed" : "");
	} else {
		std::printf("state      kept: no blob store in this build\n");
	}
	erase_partition("zb_storage"); // the Zigbee stack's NVRAM: raw
	if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "zb_db") != nullptr) {
		esp_err_t zb = nvs_flash_erase_partition("zb_db"); // an NVS partition: closed before the erase
		std::printf("zb_db      %s\n", zb == ESP_OK ? "erased" : esp_err_to_name(zb));
	}
	esp_err_t err = nvs_flash_erase(); // closes the open handles first (settings, boot record)
	std::printf("nvs        %s\n", err == ESP_OK ? "erased" : esp_err_to_name(err));
	std::printf("restarting\n");
	console_output().drain(500);
	vTaskDelay(pdMS_TO_TICKS(100));
	esp_restart();
}

bool console_start(ConsoleContext& context) {
	g_context = &context;
	g_done = xSemaphoreCreateBinaryStatic(&g_done_storage);
	if (xTaskCreate(&repl_task, "hc_console", kReplStack, nullptr, kReplPriority, nullptr) != pdPASS) {
		ESP_LOGE(kTag, "console not started: cannot create its task (stack %u B)", static_cast<unsigned>(kReplStack));
		return false;
	}
	return true;
}

} // namespace esp
} // namespace hc
