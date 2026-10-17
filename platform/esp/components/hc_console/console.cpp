/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "console.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hc/network_settings.h"
#include "linenoise/linenoise.h"

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
	std::printf("heap      %u B free (min %u B)\n", static_cast<unsigned>(memory.free_bytes),
		static_cast<unsigned>(memory.min_free_bytes));
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
	std::printf("mode      %s\n", network_mode_name(net.mode()));
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
	const char* kUsage = "wifi wps | wifi set <ssid> [<psk>] | wifi clear | wifi status";
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

int cmd_restart(ConsoleContext&, int, char**) {
	ESP_LOGW(kTag, "restart requested on the console");
	std::printf("restarting\n");
	std::fflush(stdout);
	esp_restart();
	return 0;
}

// ---- The command table: the one place where commands are registered --------------------------

struct Command {
	const char* name;
	const char* hint;
	const char* help;
	int (*run)(ConsoleContext& context, int argc, char** argv);
};

const Command kCommands[] = {
	{"status", nullptr, "Uptime, network state and address, local time and whether it is synchronized, free heap",
		&cmd_status},
	{"wifi", "wps | set <ssid> [<psk>] | clear | status",
		"WiFi: WPS push-button provisioning, credentials by hand (stored in NVS; the key is never shown; quote "
		"an SSID with spaces), link state",
		&cmd_wifi},
	{"tz", "set <posix-tz>", "Time zone as a POSIX TZ string (default CET-1CEST,M3.5.0,M10.5.0/3)", &cmd_tz},
	{"ntp", "set <server>", "SNTP server (default pool.ntp.org)", &cmd_ntp},
	{"settings", "list", "All stored settings (wifi.psk masked)", &cmd_settings},
	{"restart", nullptr, "Restart the controller", &cmd_restart},
};

// ---- Running a command on the event loop -----------------------------------------------------

constexpr int kMaxArgs = 8;
constexpr size_t kMaxLine = 256;

/// The one command in flight: the REPL handles a line at a time, so a single slot suffices. If
/// a command outlives kCommandTimeout, the slot stays busy and the next command is refused until
/// it finishes.
struct PendingCommand {
	const Command* command = nullptr;
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
	g_pending.result = g_pending.command->run(*g_context, g_pending.argc, g_pending.argv);
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

/// REPL task: copies the arguments, hands the command to the event loop and waits for it.
int dispatch(void* context, int argc, char** argv) {
	const Command* command = static_cast<const Command*>(context);
	if (std::strcmp(command->name, "wifi") == 0 && argc >= 2 && std::strcmp(argv[1], "set") == 0) {
		forget_history();
	}
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

} // namespace

bool console_start(ConsoleContext& context) {
	g_context = &context;
	g_done = xSemaphoreCreateBinaryStatic(&g_done_storage);

	esp_console_repl_t* repl = nullptr;
	esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
	repl_config.prompt = "hc>";
	repl_config.max_cmdline_length = kMaxLine;
	esp_console_dev_usb_serial_jtag_config_t device_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
	esp_err_t err = esp_console_new_repl_usb_serial_jtag(&device_config, &repl_config, &repl);
	if (err == ESP_OK) {
		err = esp_console_register_help_command();
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
	if (err == ESP_OK) {
		err = esp_console_start_repl(repl);
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "console not started: %s", esp_err_to_name(err));
		return false;
	}
	ESP_LOGI(kTag, "console ready on USB Serial/JTAG: type 'help'");
	return true;
}

} // namespace esp
} // namespace hc
