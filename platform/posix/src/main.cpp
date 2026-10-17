/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The controller as a host executable: the simulator and the single-board-computer fallback.
//
//   homecontrol [--settings <file>] [--image <file>]
//
// Runs the controller until SIGINT or SIGTERM. The settings (time zone, ...) live in a text file,
// by default homecontrol.settings in the working directory (format in doc/network.md). With the
// setting knx.gateway it keeps a KNXnet/IP tunnel to that gateway (doc/knx.md).

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/knx/adapter.h"
#include "hc/knx/knx_settings.h"
#include "hc/knx/telegram_log.h"
#include "hc/knx/tunnel.h"
#include "hc/log.h"
#include "hc/posix/file_settings.h"
#include "hc/posix/platform.h"
#include "hc/posix/udp_socket.h"

namespace {

constexpr const char* kTag = "hc.main";

constexpr const char* kDefaultSettingsFile = "homecontrol.settings";

/// Upper bound for one sleep, so a missed wakeup can never stall the loop for long.
constexpr uint64_t kMaxSleepMs = 1000;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) {
	g_stop = 1;
}

void print_usage(const char* program) {
	std::fprintf(stderr, "usage: %s [--settings <file>] [--image <file>]\n", program);
}

} // namespace

int main(int argc, char** argv) {
	hc::posix::PosixClock clock;
	hc::posix::StderrLogSink sink;
	hc::posix::PosixSystemInfo system;
	hc::posix::PosixTimeZone time_zone;
	hc::posix::PosixNetworkDriver network;
	const char* settings_path = kDefaultSettingsFile;

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

	// KNX: the tunnel runs when knx.gateway is set. The datapoint model stays empty until runtime
	// images exist (issue #8); the telegram log still records what the bus says.
	hc::knx::GatewayAddress gateway;
	bool nat = false;
	hc::knx::read_gateway_settings(settings, gateway, nat);
	hc::posix::PosixUdpSocket knx_socket(controller.loop());
	hc::DatapointModel model(clock);
	hc::knx::TelegramLog telegrams(clock);
	hc::knx::KnxTunnel tunnel(controller.loop(), clock, controller.connectivity(), knx_socket);
	hc::knx::KnxAdapter adapter(controller.loop(), clock, model, tunnel, &telegrams);
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
	if (knx) {
		tunnel.start();
	}
	hc::Log::info(kTag, "mDNS is not implemented on POSIX: _homecontrol._tcp is not announced (the host's wall clock "
		"needs no SNTP either; ntp.server is not used here)");
	while (!g_stop) {
		controller.run_once();
		uint64_t now = clock.monotonic_ms();
		uint64_t deadline = controller.next_deadline();
		if (deadline <= now) {
			continue;
		}
		uint64_t wait = deadline - now;
		// Sleeps (a signal interrupts it, the loop then checks g_stop); wakes early for a datagram of
		// the KNX tunnel.
		knx_socket.wait(wait < kMaxSleepMs ? wait : kMaxSleepMs);
	}
	tunnel.stop(); // DISCONNECT_REQUEST, so the gateway frees the tunnel at once
	hc::Log::info(kTag, "stopping on signal after %llu s",
		static_cast<unsigned long long>(controller.uptime_ms() / 1000));
	return 0;
}
