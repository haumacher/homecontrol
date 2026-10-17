/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hc/knx/knx_settings.h"
#include "knx_service.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kUsage =
	"knx status | knx gateway set <host[:port]> | knx gateway clear | knx nat on|off | knx log [<n>] | "
	"knx write <ga> <dpt> <value> | knx read <ga> <dpt>";

int usage() {
	std::printf("usage: %s\n", kUsage);
	return 1;
}

int refused(const char* error) {
	std::printf("error: %s\n", error);
	return 1;
}

int status(KnxService& service) {
	knx::KnxTunnel& tunnel = service.tunnel();
	char text[knx::GatewayAddress::kMaxText];
	std::printf("tunnel    %s (%s)\n", knx::tunnel_state_name(tunnel.state()), tunnel.reason());
	if (tunnel.gateway().empty()) {
		std::printf("gateway   none: knx gateway set <host[:port]>\n");
	} else {
		std::printf("gateway   %s%s\n", tunnel.gateway().format(text, sizeof(text)), tunnel.nat() ? " (NAT mode)" : "");
	}
	if (tunnel.is_up()) {
		char address[knx::IndividualAddress::kMaxText];
		char endpoint[Ipv4Endpoint::kMaxText];
		std::printf("channel   %u, address %s, data endpoint %s\n", static_cast<unsigned>(tunnel.channel()),
			tunnel.address().format(address, sizeof(address)), tunnel.data_endpoint().format(endpoint, sizeof(endpoint)));
		std::printf("queue     %u frame(s)\n", static_cast<unsigned>(tunnel.queued()));
	} else if (tunnel.next_attempt_at() != EventLoop::kNever) {
		uint64_t now = service.now_ms();
		uint64_t at = tunnel.next_attempt_at();
		std::printf("next      attempt %u in %llu ms (failures %u)\n", static_cast<unsigned>(tunnel.attempts() + 1),
			static_cast<unsigned long long>(at > now ? at - now : 0), static_cast<unsigned>(tunnel.backoff().failures()));
	}
	std::printf("datapoints %u mapped, %u reads pending\n", static_cast<unsigned>(service.adapter().size()),
		static_cast<unsigned>(service.adapter().reads_pending()));
	std::printf("telegrams %llu seen (last %u kept: knx log), %u datagrams dropped\n",
		static_cast<unsigned long long>(service.telegrams().total()), static_cast<unsigned>(service.telegrams().size()),
		static_cast<unsigned>(service.socket().dropped()));
	return 0;
}

int log(KnxService& service, int argc, char** argv) {
	knx::TelegramLog& telegrams = service.telegrams();
	size_t count = telegrams.size();
	if (argc == 3) {
		char* end = nullptr;
		long n = std::strtol(argv[2], &end, 10);
		if (end == argv[2] || *end != '\0' || n <= 0) {
			return refused("count must be a positive number");
		}
		if (static_cast<size_t>(n) < count) {
			count = static_cast<size_t>(n);
		}
	}
	if (telegrams.size() == 0) {
		std::printf("no group telegrams yet\n");
		return 0;
	}
	char line[knx::TelegramLog::kMaxLine];
	for (size_t i = telegrams.size() - count; i < telegrams.size(); ++i) {
		std::printf("%s\n", knx::TelegramLog::format(*telegrams.at(i), &service.model(), line, sizeof(line)));
	}
	return 0;
}

} // namespace

int knx_console_command(KnxService& service, int argc, char** argv) {
	if (argc < 2) {
		return usage();
	}
	const char* error = nullptr;
	const char* what = argv[1];
	if (std::strcmp(what, "status") == 0 && argc == 2) {
		return status(service);
	}
	if (std::strcmp(what, "gateway") == 0 && argc == 4 && std::strcmp(argv[2], "set") == 0) {
		if (!service.set_gateway(argv[3], &error)) {
			return refused(error);
		}
		std::printf("knx.gateway %s stored; connecting (knx status)\n", argv[3]);
		return 0;
	}
	if (std::strcmp(what, "gateway") == 0 && argc == 3 && std::strcmp(argv[2], "clear") == 0) {
		if (!service.clear_gateway(&error)) {
			return refused(error);
		}
		std::printf("knx.gateway cleared; the tunnel is down\n");
		return 0;
	}
	if (std::strcmp(what, "nat") == 0 && argc == 3 && (std::strcmp(argv[2], "on") == 0 || std::strcmp(argv[2], "off") == 0)) {
		bool nat = argv[2][1] == 'n';
		if (!service.set_nat(nat, &error)) {
			return refused(error);
		}
		std::printf("knx.nat %s stored\n", nat ? "on" : "off");
		return 0;
	}
	if (std::strcmp(what, "log") == 0 && argc <= 3) {
		return log(service, argc, argv);
	}
	if (std::strcmp(what, "write") == 0 && argc == 5) {
		if (!service.write(argv[2], argv[3], argv[4], &error)) {
			return refused(error);
		}
		std::printf("sent; the confirmation follows in the log\n");
		return 0;
	}
	if (std::strcmp(what, "read") == 0 && argc == 4) {
		if (!service.read(argv[2], argv[3], &error)) {
			return refused(error);
		}
		std::printf("read sent; the response follows in the log (at most 5 s)\n");
		return 0;
	}
	return usage();
}

} // namespace esp
} // namespace hc
