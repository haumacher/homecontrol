/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The controller as a host executable: the simulator and the single-board-computer fallback.
//
//   homecontrol [--settings <file>] [--image <file>] [--http <port>] [--bind <address>] [--www <dir>] [--demo]
//               [--version]
//
// Runs the controller until SIGINT or SIGTERM. The settings (time zone, ...) live in a text file,
// by default homecontrol.settings in the working directory (format in doc/network.md). The web
// server (doc/web.md) serves the API and the UI files of web/ui/dist (the package's
// /usr/share/homecontrol/www) on port 8081 (--http 0: off), the WebSocket on the port above, both on
// every IPv4 address (--bind); the environment variables HOMECONTROL_HTTP_PORT and
// HOMECONTROL_HTTP_BIND set both for the systemd unit (doc/linux.md), the command line wins. --demo
// loads simulated datapoints for UI development. With the
// setting knx.gateway it keeps a KNXnet/IP tunnel to that gateway (doc/knx.md). --image loads a
// binary runtime image (spec/runtime-image.md 9, compiled by E-Home, its signature inside) at
// start; PUT /api/image replaces it. The image slot is the file `<settings stem>.image.bin`.
// The blobs (the stored image, persisted values, history rings) live in a directory beside the
// settings file (doc/persistence.md). The language packs are the UI's own (www/lang): nothing is
// fetched from the update server, and the package manager updates the controller (doc/linux.md).

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
#include <vector>

#include "hc/blob_queue.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/demo.h"
#include "hc/ehome_link.h"
#include "hc/external_values.h"
#include "hc/fetch_queue.h"
#include "hc/image/image_service.h"
#include "hc/posix/file_image_slot.h"
#include "hc/posix/thread_worker.h"
#include "hc/mqtt.h"
#include "hc/signature.h"
#include "hc/posix/mqtt_client.h"
#include "hc/version.h"
#include "hc/posix/file_blob_store.h"
#include "hc/posix/httplib_fetcher.h"
#include "hc/posix/websocket_client.h"
#include "hc/image/memory_blob_store.h"
#include "hc/knx/adapter.h"
#include "hc/knx/gateway_search.h"
#include "hc/knx/knx_settings.h"
#include "hc/knx/telegram_log.h"
#include "hc/knx/tunnel.h"
#include "hc/log.h"
#include "hc/posix/diagnostics.h"
#include "hc/posix/options.h"
#include "hc/posix/file_settings.h"
#include "hc/posix/platform.h"
#include "hc/posix/udp_socket.h"
#include "hc/posix/web_server.h"

namespace {

constexpr const char* kTag = "hc.main";

#ifndef HC_WEB_UI_DIR
#define HC_WEB_UI_DIR "web/ui/dist"
#endif

/// Upper bound for one sleep, so a missed wakeup can never stall the loop for long.
constexpr uint64_t kMaxSleepMs = 1000;

volatile std::sig_atomic_t g_stop = 0;
/// The stop is a restart (or factory reset) requested over the API, not a signal.
bool g_restart = false;

/// Erases every setting and every blob (the factory reset over the API).
void erase_all(hc::Settings& settings, hc::BlobStore& blobs) {
	struct Keys : hc::SettingsVisitor {
		void entry(const char* key, hc::SettingType) override { names.push_back(key); }
		std::vector<std::string> names;
	} keys;
	settings.visit(keys);
	for (const std::string& key : keys.names) {
		settings.erase(key.c_str());
	}
	struct Blobs : hc::BlobVisitor {
		void blob(const char* name, size_t) override { names.push_back(name); }
		std::vector<std::string> names;
	} names;
	blobs.visit(names);
	for (const std::string& name : names.names) {
		blobs.remove(name.c_str());
	}
}

void on_signal(int signal) {
	g_stop = signal;
}

/// Reads the file `path` into `out`; false (logged) if it cannot be read. Stops after `limit` + 1 bytes.
bool read_image_file(const char* path, std::string& out, size_t limit) {
	std::FILE* file = std::fopen(path, "rb");
	if (file == nullptr) {
		hc::Log::error(kTag, "cannot open the image %s: %s", path, std::strerror(errno));
		return false;
	}
	char buffer[4096];
	size_t n = 0;
	while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
		out.append(buffer, n);
		if (out.size() > limit) {
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

} // namespace

/// The UDP sockets the main loop polls: the KNX tunnel's and the gateway search's.
struct UdpSockets {
	hc::posix::PosixUdpSocket& tunnel;
	const hc::posix::PosixUdpSocketFactory& search; ///< A socket only while a search runs.
};

/// Sleeps up to `ms` or until the web executor has a job or a UDP socket a datagram, then runs
/// the jobs and delivers the datagrams on this (the loop's) thread; their run time counts as a turn
/// of the loop (EventLoop::account_turn(), the System page).
void sleep_until_work(hc::posix::LoopExecutor& executor, UdpSockets sockets, uint64_t ms,
	hc::EventLoop& loop, const hc::Clock& clock) {
	pollfd entries[3];
	hc::posix::PosixUdpSocket* owners[3] = {};
	nfds_t count = 0;
	if (executor.wake_fd() >= 0) {
		entries[count].fd = executor.wake_fd();
		entries[count].events = POLLIN;
		entries[count].revents = 0;
		++count;
	}
	for (hc::posix::PosixUdpSocket* socket : {&sockets.tunnel, sockets.search.current()}) {
		if (socket != nullptr && socket->fd() >= 0) {
			entries[count].fd = socket->fd();
			entries[count].events = POLLIN;
			entries[count].revents = 0;
			owners[count] = socket;
			++count;
		}
	}
	if (count == 0) {
		executor.wait(ms);
		return;
	}
	int ready = poll(entries, count, static_cast<int>(ms));
	uint64_t started_ms = clock.monotonic_ms();
	if (ready <= 0) {
		executor.run_pending(); // a job may have slipped in right after the wait ended
		loop.account_turn(started_ms, clock.monotonic_ms());
		return;
	}
	for (nfds_t i = 0; i < count; ++i) {
		if ((entries[i].revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
			continue;
		}
		if (owners[i] != nullptr) {
			owners[i]->drain();
		} else {
			executor.run_pending();
		}
	}
	loop.account_turn(started_ms, clock.monotonic_ms());
}

int main(int argc, char** argv) {
	hc::posix::PosixClock clock;
	hc::posix::StderrLogSink sink;
	hc::posix::PosixSystemInfo system;
	hc::posix::PosixTimeZone time_zone;
	hc::posix::PosixNetworkDriver network;

	// Log through the sink while the arguments are parsed (the Controller installs it for good).
	hc::Log::set_sink(&sink);
	hc::posix::Options options;
	std::string error;
	if (!hc::posix::parse_options(argc, argv, [](const char* name) -> const char* { return std::getenv(name); },
			options, error)) {
		hc::Log::error(kTag, "%s", error.c_str());
		std::fprintf(stderr, "%s\n", hc::posix::usage(argv[0]).c_str());
		return 2;
	}
	if (options.action == hc::posix::Options::Action::help) {
		std::fprintf(stderr, "%s\n", hc::posix::usage(argv[0]).c_str());
		return 0;
	}
	if (options.action == hc::posix::Options::Action::version) {
		// The same words as the controller's first log line and the device's /api/status.
		std::printf("homecontrol %s (build %s)\n", hc::kVersion, hc::build_id());
		return 0;
	}
	const std::string& settings_path = options.settings_path;
	const char* image_path = options.image_path.empty() ? nullptr : options.image_path.c_str();
	const bool demo = options.demo;
	const unsigned http_port = options.http_port;
	const std::string www_dir = options.www_dir.empty() ? std::string(HC_WEB_UI_DIR) : options.www_dir;

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
	controller.shutdown().add(tunnel); // the stop closes the tunnel first (homecontrol #103)
	// The gateway search of the Settings page (/api/knx/search), on a socket of its own while it runs.
	hc::posix::PosixUdpSocketFactory search_sockets(controller.loop());
	hc::knx::GatewaySearch gateway_search(controller.loop(), clock, controller.connectivity(), search_sockets);
	const UdpSockets udp{knx_socket, search_sockets};
	hc::posix::FileBlobStore blobs(hc::posix::FileBlobStore::directory_for_settings(settings_path));
	// The image slot (spec/runtime-image.md 7): a file beside the settings, 512 KB at most.
	hc::posix::FileImageSlot slot(hc::posix::FileImageSlot::path_for_settings(settings_path));
	// The loop's blob writes go through a write queue to a worker of its own (homecontrol #101). The
	// worker is the later member, so it is destroyed first: its destructor waits for a job that
	// still reads the queue's copy.
	struct FlashWrites {
		hc::BlobQueue queue;
		hc::posix::ThreadWorker worker;
		FlashWrites(hc::EventLoop& loop, const hc::Clock& clock, hc::BlobStore& store,
			hc::posix::LoopExecutor& executor, const hc::BlobQueueLimits& limits)
			: queue(loop, clock, store, &worker, limits), worker(executor) {}
	};
	hc::BlobQueueLimits flash_limits;
	flash_limits.arena_bytes = 64 * 1024;
	flash_limits.entries = 64;
	FlashWrites flash(controller.loop(), clock, blobs, executor, flash_limits);
	hc::BlobQueue& flash_queue = flash.queue;
	controller.shutdown().add(flash_queue); // a planned restart waits for the queued writes
	hc::image::ImageService image(controller, model, flash_queue, slot, adapter, &tunnel);
	// The storage figures of the status, measured off the loop after a change (#74).
	hc::StorageStats storage(blobs);
	hc::posix::ThreadWorker storage_worker(executor);
	storage.set_worker(&storage_worker);
	// One HTTP client for every request, queued by priority (issue #45 item 13, as on the device).
	hc::posix::HttplibFetcher fetcher(executor);
	hc::FetchQueue fetches(controller.loop(), clock, fetcher);
	hc::FetchQueue::Lane enrollment_lane(fetches, "enrollment", 2);
	hc::FetchQueue::Lane external_lane(fetches, "external", 3);
	hc::ExternalValues external(controller.loop(), controller.time(), settings, controller.connectivity(), model,
		external_lane);
	image.set_external(&external);
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
	// The tunnel always runs (as on the device), so that knx.gateway set over the API connects at once.
	adapter.start();
	tunnel.set_gateway(&gateway, nat);
	if (gateway.empty()) {
		hc::Log::info(kTag, "no KNX gateway (setting %s): the KNX tunnel stays down until one is set",
			hc::knx::setting::kGateway);
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
		storage.measure_now();
		image.start();
		if (image_path != nullptr) {
			// A binary image (spec/runtime-image.md 9), compiled by E-Home; the signature is inside.
			std::string bytes;
			if (!read_image_file(image_path, bytes, slot.size())) {
				return 1;
			}
			if (!image.load(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), hc::image::Source::file)) {
				hc::Log::error(kTag, "the image %s was refused (the log above names each problem)", image_path);
				return 1;
			}
		}
	}
	if (http_port != 0) {
		web.reset(new hc::posix::WebServer(controller, model, executor, static_cast<uint16_t>(http_port), www_dir));
		web->set_bind_address(options.bind_address);
		if (!demo) {
			web->set_image(&image);
			web->set_mqtt(&mqtt);
			web->set_external(&external);
			web->set_storage_stats(&storage);
		}
		web->set_knx(&tunnel, &telegrams);
		web->set_knx_search(&gateway_search);
		// Restart and factory reset over the API: the process stops (a supervisor such as systemd with
		// Restart=always starts it again); the reset first erases the settings and the blobs.
		web->set_system_handler([&settings, &blobs, &slot](hc::SystemAction action) {
			if (action == hc::SystemAction::factory_reset) {
				erase_all(settings, blobs);
				const char* error = nullptr;
				slot.erase(hc::ImageSlot::kSector, &error); // the image's header: an erased slot
				hc::Log::warn(kTag, "factory reset over the API: settings and blobs erased; stopping");
			} else {
				hc::Log::warn(kTag, "restart requested over the API: stopping (a supervisor starts the process again)");
			}
			g_restart = true;
			g_stop = SIGTERM;
		});
		if (!web->start()) {
			return 1;
		}
	}
	// The E-Home connection (doc/ehome.md): the enrollment on its lane of the HTTP client, the socket
	// over plain ws:// (the POSIX client has no TLS). It joins the web server's API and live feed.
	std::unique_ptr<hc::posix::PosixWebSocketClient> ehome_client;
	std::unique_ptr<hc::EhomeLink> ehome;
	if (web) {
		ehome_client.reset(new hc::posix::PosixWebSocketClient(executor));
		ehome.reset(new hc::EhomeLink(web->api(), web->feed(), *ehome_client, enrollment_lane));
		ehome->start();
	}
	tunnel.start();
	hc::Log::info(kTag, "mDNS is not implemented on POSIX: _homecontrol._tcp is not announced (the host's wall clock "
		"needs no SNTP either; ntp.server is not used here)");
	while (!g_stop) {
		controller.run_once();
		uint64_t now = clock.monotonic_ms();
		uint64_t deadline = controller.next_deadline();
		uint64_t wait = deadline <= now ? 0 : deadline - now;
		// Sleeps until the deadline, a job from a web server thread, or a datagram for the KNX tunnel
		// (a signal interrupts the sleep; the loop then checks g_stop).
		sleep_until_work(executor, udp, wait < kMaxSleepMs ? wait : kMaxSleepMs, controller.loop(), clock);
	}
	mqtt.stop(); // offline on the status topic; the retained configs stay for the next run
	for (int i = 0; i < 30 && mqtt.state() != hc::MqttState::disabled; ++i) {
		controller.run_once(); // the retirement drains the queue and disconnects (at most kRetireMs)
		sleep_until_work(executor, udp, 100, controller.loop(), clock);
	}
	// The connections close (homecontrol #103): the KNX tunnel's DISCONNECT_REQUEST, its answer awaited
	// at most Shutdown::kMaxWaitMs, so the gateway frees the slot before the process ends.
	image.flush(); // pending value and history writes, into the write queue
	controller.shutdown().begin(g_restart ? "restart over the API" : "stop", hc::Callback());
	while (!controller.shutdown().finished()) {
		controller.run_once();
		sleep_until_work(executor, udp, 50, controller.loop(), clock);
	}
	// The queue's writes (also when a restart over the API ran the closing phase already).
	for (int i = 0; i < 100 && !flash_queue.idle(); ++i) {
		controller.run_once();
		sleep_until_work(executor, udp, 20, controller.loop(), clock);
	}
	if (!flash_queue.idle()) {
		hc::Log::warn(kTag, "stopping with %u blob change(s) not written", static_cast<unsigned>(flash_queue.pending()));
	}
	executor.stop();
	if (web) {
		web->stop();
	}
	hc::Log::info(kTag, "stopping on %s after %llu s", g_restart ? "a restart over the API" :
		hc::posix::PosixDiagnostics::signal_name(g_stop).c_str(),
		static_cast<unsigned long long>(controller.uptime_ms() / 1000));
	diagnostics.stopped(g_stop);
	return 0;
}
