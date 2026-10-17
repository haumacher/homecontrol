/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The controller as a host executable: the simulator and the single-board-computer fallback.
//
//   homecontrol [--settings <file>] [--image <file>] [--http <port>] [--www <dir>] [--demo]
//
// Runs the controller until SIGINT or SIGTERM. The settings (time zone, ...) live in a text file,
// by default homecontrol.settings in the working directory (format in doc/network.md). The web
// server (doc/web.md) serves the API and the UI files of web/ui/dist on port 8080 (--http 0: off),
// the WebSocket on the port above; --demo loads simulated datapoints for UI development. With the
// setting knx.gateway it keeps a KNXnet/IP tunnel to that gateway (doc/knx.md). --image loads a
// runtime image (spec/runtime-image.md) at start; PUT /api/image replaces it. The blobs (the stored
// image, persisted values, history rings, the UI's language pack) live in a directory beside the
// settings file (doc/persistence.md).

#include <cerrno>
#include <csignal>
#include <poll.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>

#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/demo.h"
#include "hc/external_values.h"
#include "hc/image/image_service.h"
#include "hc/language_packs.h"
#include "hc/mqtt.h"
#include "hc/posix/mqtt_client.h"
#include "hc/version.h"
#include "hc/posix/file_blob_store.h"
#include "hc/posix/httplib_fetcher.h"
#include "hc/image/memory_blob_store.h"
#include "hc/knx/adapter.h"
#include "hc/knx/knx_settings.h"
#include "hc/knx/telegram_log.h"
#include "hc/knx/tunnel.h"
#include "hc/log.h"
#include "hc/posix/diagnostics.h"
#include "hc/posix/file_settings.h"
#include "hc/posix/platform.h"
#include "hc/posix/udp_socket.h"
#include "hc/posix/web_server.h"

namespace {

constexpr const char* kTag = "hc.main";

constexpr const char* kDefaultSettingsFile = "homecontrol.settings";

constexpr unsigned kDefaultHttpPort = 8080;

/// The RAM blob store's capacity (the stand-in for the file store): the image and its persisted
/// state with room to spare.

#ifndef HC_WEB_UI_DIR
#define HC_WEB_UI_DIR "web/ui/dist"
#endif

/// Upper bound for one sleep, so a missed wakeup can never stall the loop for long.
constexpr uint64_t kMaxSleepMs = 1000;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int signal) {
	g_stop = signal;
}

/// Reads the file `path` into `out`; false (logged) if it cannot be read or exceeds the image limit.
bool read_image_file(const char* path, std::string& out) {
	std::FILE* file = std::fopen(path, "rb");
	if (file == nullptr) {
		hc::Log::error(kTag, "cannot open the image %s: %s", path, std::strerror(errno));
		return false;
	}
	char buffer[4096];
	size_t n = 0;
	while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
		out.append(buffer, n);
		if (out.size() > hc::image::kMaxImageBytes) {
			break;
		}
	}
	bool ok = std::ferror(file) == 0;
	std::fclose(file);
	if (!ok) {
		hc::Log::error(kTag, "cannot read the image %s", path);
	}
	return ok;
}

/// "homecontrol-<host name>" with every character outside [A-Za-z0-9_-] replaced by '-', cut to fit.
void mqtt_node_id(char* out, size_t size) {
	char host[64] = "host";
	gethostname(host, sizeof(host) - 1);
	host[sizeof(host) - 1] = '\0';
	for (char* p = host; *p != '\0'; ++p) {
		bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
		if (!ok) {
			*p = '-';
		}
	}
	std::snprintf(out, size, "homecontrol-%s", host);
}

void print_usage(const char* program) {
	std::fprintf(stderr, "usage: %s [--settings <file>] [--image <file>] [--http <port>] [--www <dir>] [--demo]\n",
		program);
}

} // namespace

/// Sleeps up to `ms` or until the web executor has a job or the KNX socket a datagram, then runs
/// the jobs and delivers the datagrams on this (the loop's) thread.
void sleep_until_work(hc::posix::LoopExecutor& executor, hc::posix::PosixUdpSocket& knx_socket, uint64_t ms) {
	pollfd entries[2];
	nfds_t count = 0;
	if (executor.wake_fd() >= 0) {
		entries[count].fd = executor.wake_fd();
		entries[count].events = POLLIN;
		entries[count].revents = 0;
		++count;
	}
	if (knx_socket.fd() >= 0) {
		entries[count].fd = knx_socket.fd();
		entries[count].events = POLLIN;
		entries[count].revents = 0;
		++count;
	}
	if (count == 0) {
		executor.wait(ms);
		return;
	}
	int ready = poll(entries, count, static_cast<int>(ms));
	if (ready <= 0) {
		executor.run_pending(); // a job may have slipped in right after the wait ended
		return;
	}
	for (nfds_t i = 0; i < count; ++i) {
		if ((entries[i].revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
			continue;
		}
		if (entries[i].fd == knx_socket.fd()) {
			knx_socket.drain();
		} else {
			executor.run_pending();
		}
	}
}

int main(int argc, char** argv) {
	hc::posix::PosixClock clock;
	hc::posix::StderrLogSink sink;
	hc::posix::PosixSystemInfo system;
	hc::posix::PosixTimeZone time_zone;
	hc::posix::PosixNetworkDriver network;
	const char* settings_path = kDefaultSettingsFile;
	unsigned http_port = kDefaultHttpPort;
	const char* www_dir = HC_WEB_UI_DIR;
	bool demo = false;
	const char* image_path = nullptr;

	// Log through the sink while the arguments are parsed (the Controller installs it for good).
	hc::Log::set_sink(&sink);
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--settings") == 0) {
			if (i + 1 >= argc) {
				hc::Log::error(kTag, "--settings needs a file argument");
				print_usage(argv[0]);
				return 2;
			}
			settings_path = argv[++i];
			continue;
		}
		if (std::strcmp(argv[i], "--image") == 0) {
			if (i + 1 >= argc) {
				hc::Log::error(kTag, "--image needs a file argument");
				print_usage(argv[0]);
				return 2;
			}
			image_path = argv[++i];
			continue;
		}
		if (std::strcmp(argv[i], "--http") == 0 || std::strcmp(argv[i], "--www") == 0) {
			if (i + 1 >= argc) {
				hc::Log::error(kTag, "%s needs an argument", argv[i]);
				print_usage(argv[0]);
				return 2;
			}
			if (argv[i][2] == 'w') {
				www_dir = argv[++i];
				continue;
			}
			char* end = nullptr;
			unsigned long port = std::strtoul(argv[++i], &end, 10);
			if (*end != '\0' || port > 65534) {
				hc::Log::error(kTag, "--http takes a port 0..65534 (0: no web server; the WebSocket uses the next port)");
				return 2;
			}
			http_port = static_cast<unsigned>(port);
			continue;
		}
		if (std::strcmp(argv[i], "--demo") == 0) {
			demo = true;
			continue;
		}
		if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
			print_usage(argv[0]);
			return 0;
		}
		hc::Log::error(kTag, "unknown argument: %s", argv[i]);
		print_usage(argv[0]);
		return 2;
	}

	if (demo && image_path != nullptr) {
		hc::Log::error(kTag, "--demo and --image both fill the datapoint model: choose one");
		return 2;
	}

	hc::posix::FileSettings settings(settings_path);
	settings.load();

	// The run marker beside the settings: how the previous run ended, this run's PID and start time
	// (doc/diagnostics.md).
	hc::posix::PosixDiagnostics diagnostics(hc::posix::PosixDiagnostics::marker_for_settings(settings_path));
	diagnostics.start();

	hc::Controller controller(hc::Platform{clock, sink, system, settings, time_zone, network,
		nullptr, nullptr, nullptr, &diagnostics});
	network.bind(controller.loop(), controller.connectivity());
	hc::DatapointModel model(clock);
	hc::DemoTechnology demo_technology(model, controller.loop(), clock);
	hc::posix::LoopExecutor executor;
	std::unique_ptr<hc::posix::WebServer> web;

	// KNX: the tunnel runs when knx.gateway is set. The runtime image maps datapoints to group
	// addresses; without one the telegram log still records what the bus says.
	hc::knx::GatewayAddress gateway;
	bool nat = false;
	hc::knx::read_gateway_settings(settings, gateway, nat);
	hc::posix::PosixUdpSocket knx_socket(controller.loop());
	hc::knx::TelegramLog telegrams(clock);
	hc::knx::KnxTunnel tunnel(controller.loop(), clock, controller.connectivity(), knx_socket);
	hc::knx::KnxAdapter adapter(controller.loop(), clock, model, tunnel, &telegrams);
	hc::posix::FileBlobStore blobs(hc::posix::FileBlobStore::directory_for_settings(settings_path));
	hc::image::ImageService image(controller, model, blobs, adapter, &tunnel);
	hc::posix::HttplibFetcher fetcher(executor);
	hc::ExternalValues external(controller.loop(), controller.time(), settings, controller.connectivity(), model, fetcher);
	image.set_external(&external);
	// The web UI's language pack for ui.lang, fetched from the update server (doc/i18n.md).
	hc::LanguagePacks packs(controller.loop(), controller.time(), settings, controller.connectivity(), fetcher, blobs,
		hc::kVersion);
	// MQTT (doc/mqtt.md): the node id is "homecontrol-<host name>" as the device's is "homecontrol-<MAC>".
	char node_id[hc::Mqtt::kMaxNodeId + 1];
	mqtt_node_id(node_id, sizeof(node_id));
	hc::MqttIdentity mqtt_identity;
	mqtt_identity.node_id = node_id;
	mqtt_identity.sw_version = hc::kVersion;
	hc::posix::PosixMqttClient mqtt_client(executor);
	hc::Mqtt mqtt(controller.loop(), clock, settings, controller.connectivity(), model, mqtt_client, mqtt_identity,
		&image.history());
	image.set_mqtt(&mqtt);
	bool knx = !gateway.empty();
	if (knx) {
		adapter.start();
		tunnel.set_gateway(&gateway, nat);
	} else {
		hc::Log::info(kTag, "no KNX gateway (setting %s): the KNX tunnel is off", hc::knx::setting::kGateway);
	}

	struct sigaction action;
	std::memset(&action, 0, sizeof(action));
	action.sa_handler = on_signal;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, nullptr);
	sigaction(SIGTERM, &action, nullptr);

	controller.start();
	if (demo) {
		demo_technology.load(); // without the image service: /api/image answers image.unavailable
	} else {
		blobs.start();
		image.start();
		packs.start();
		if (image_path != nullptr) {
			std::string text;
			if (!read_image_file(image_path, text)) {
				return 1;
			}
			if (!image.load(text.data(), text.size(), hc::image::Source::file, true)) {
				hc::Log::error(kTag, "the image %s was refused (the log above names each problem)", image_path);
				return 1;
			}
		}
	}
	if (http_port != 0) {
		web.reset(new hc::posix::WebServer(controller, model, executor, static_cast<uint16_t>(http_port), www_dir));
		if (!demo) {
			web->set_image(&image);
			web->set_language_packs(&packs, &blobs);
			web->set_mqtt(&mqtt);
		}
		if (!web->start()) {
			return 1;
		}
	}
	if (knx) {
		tunnel.start();
	}
	hc::Log::info(kTag, "mDNS is not implemented on POSIX: _homecontrol._tcp is not announced (the host's wall clock "
		"needs no SNTP either; ntp.server is not used here)");
	while (!g_stop) {
		controller.run_once();
		uint64_t now = clock.monotonic_ms();
		uint64_t deadline = controller.next_deadline();
		uint64_t wait = deadline <= now ? 0 : deadline - now;
		// Sleeps until the deadline, a job from a web server thread, or a datagram for the KNX tunnel
		// (a signal interrupts the sleep; the loop then checks g_stop).
		sleep_until_work(executor, knx_socket, wait < kMaxSleepMs ? wait : kMaxSleepMs);
	}
	mqtt.stop(); // offline on the status topic; the retained configs stay for the next run
	for (int i = 0; i < 30 && mqtt.state() != hc::MqttState::disabled; ++i) {
		controller.run_once(); // the retirement drains the queue and disconnects (at most kRetireMs)
		sleep_until_work(executor, knx_socket, 100);
	}
	executor.stop();
	if (web) {
		web->stop();
	}
	image.flush(); // pending value and history writes
	tunnel.stop(); // DISCONNECT_REQUEST, so the gateway frees the tunnel at once
	hc::Log::info(kTag, "stopping on %s after %llu s", hc::posix::PosixDiagnostics::signal_name(g_stop).c_str(),
		static_cast<unsigned long long>(controller.uptime_ms() / 1000));
	diagnostics.stopped(g_stop);
	return 0;
}
