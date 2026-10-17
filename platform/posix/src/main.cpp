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
// the WebSocket on the port above; --demo loads simulated datapoints for UI development.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <memory>

#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/demo.h"
#include "hc/log.h"
#include "hc/posix/file_settings.h"
#include "hc/posix/platform.h"
#include "hc/posix/web_server.h"

namespace {

constexpr const char* kTag = "hc.main";

constexpr const char* kDefaultSettingsFile = "homecontrol.settings";

constexpr unsigned kDefaultHttpPort = 8080;

#ifndef HC_WEB_UI_DIR
#define HC_WEB_UI_DIR "web/ui/dist"
#endif

/// Upper bound for one sleep, so a missed wakeup can never stall the loop for long.
constexpr uint64_t kMaxSleepMs = 1000;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) {
	g_stop = 1;
}

void print_usage(const char* program) {
	std::fprintf(stderr, "usage: %s [--settings <file>] [--image <file>] [--http <port>] [--www <dir>] [--demo]\n",
		program);
}

} // namespace

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
			hc::Log::error(kTag, "refusing --image %s: runtime images are not supported yet (issue #8)", argv[i + 1]);
			return 2;
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

	hc::posix::FileSettings settings(settings_path);
	settings.load();

	hc::Controller controller(hc::Platform{clock, sink, system, settings, time_zone, network});
	network.bind(controller.loop(), controller.connectivity());
	hc::DatapointModel model(clock);
	hc::DemoTechnology demo_technology(model, controller.loop(), clock);
	hc::posix::LoopExecutor executor;
	std::unique_ptr<hc::posix::WebServer> web;

	struct sigaction action;
	std::memset(&action, 0, sizeof(action));
	action.sa_handler = on_signal;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, nullptr);
	sigaction(SIGTERM, &action, nullptr);

	controller.start();
	if (demo) {
		demo_technology.load();
	}
	if (http_port != 0) {
		web.reset(new hc::posix::WebServer(controller, model, executor, static_cast<uint16_t>(http_port), www_dir));
		if (!web->start()) {
			return 1;
		}
	}
	hc::Log::info(kTag, "mDNS is not implemented on POSIX: _homecontrol._tcp is not announced (the host's wall clock "
		"needs no SNTP either; ntp.server is not used here)");
	while (!g_stop) {
		controller.run_once();
		uint64_t now = clock.monotonic_ms();
		uint64_t deadline = controller.next_deadline();
		uint64_t wait = deadline <= now ? 0 : deadline - now;
		// Sleeps until the deadline or until a server thread hands the loop a job (and runs it).
		executor.wait(wait < kMaxSleepMs ? wait : kMaxSleepMs);
	}
	executor.stop();
	if (web) {
		web->stop();
	}
	hc::Log::info(kTag, "stopping on signal after %llu s",
		static_cast<unsigned long long>(controller.uptime_ms() / 1000));
	return 0;
}
