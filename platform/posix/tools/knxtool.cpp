/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// knxtool: talks to a KNXnet/IP gateway through the controller's own tunnel (doc/knx.md).
//
//   knxtool --gateway <host[:port]> [--nat] monitor
//   knxtool --gateway <host[:port]> [--nat] write <group address> <dpt> <value>
//   knxtool --gateway <host[:port]> [--nat] read <group address> <dpt>
//
// monitor prints every group telegram until Ctrl-C, with a guess of the value from the data length.
// write sends one A_GroupValue_Write and waits for the gateway's confirmation. read sends an
// A_GroupValue_Read and prints the first A_GroupValue_Response. Telegrams go to stdout, the
// tunnel's log to stderr.
//
// Exit codes: 0 done; 1 the bus did not confirm the write or nobody answered the read; 2 a usage
// error or a refused argument; 3 no tunnel to the gateway.

#include <csignal>
#include <cstdio>
#include <cstring>

#include "hc/connectivity.h"
#include "hc/dpt.h"
#include "hc/event_loop.h"
#include "hc/knx/address.h"
#include "hc/knx/cemi.h"
#include "hc/knx/group_value.h"
#include "hc/knx/knx_settings.h"
#include "hc/knx/tunnel.h"
#include "hc/log.h"
#include "hc/posix/platform.h"
#include "hc/posix/udp_socket.h"

namespace {

using hc::Dpt;
using hc::Value;
using hc::knx::GroupAddress;
using hc::knx::GroupData;
using hc::knx::GroupService;
using hc::knx::KnxTunnel;
using hc::knx::LData;
using hc::knx::SendOutcome;
using hc::knx::TunnelState;

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;
constexpr int kNoTunnel = 3;

/// How long write and read wait for the tunnel to come up.
constexpr uint64_t kConnectWaitMs = 15000;
/// How long read waits for the response after its confirmation.
constexpr uint64_t kResponseWaitMs = 5000;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) {
	g_stop = 1;
}

void usage() {
	std::fprintf(stderr,
		"usage: knxtool --gateway <host[:port]> [--nat] monitor\n"
		"       knxtool --gateway <host[:port]> [--nat] write <group address> <dpt> <value>\n"
		"       knxtool --gateway <host[:port]> [--nat] read <group address> <dpt>\n"
		"  group address 1/2/3 or 1/515, dpt like 1.001 or 9.001; port 3671 by default\n"
		"  exit codes: 0 done, 1 not confirmed / no response, 2 usage, 3 no tunnel\n");
}

/// Prints `value` decoded as `dpt` if it decodes ("9.001 21.5").
bool try_decode(const char* dpt_text, const GroupData& data, bool first) {
	std::optional<Dpt> dpt = Dpt::parse(dpt_text);
	Value value;
	if (!dpt || !hc::knx::decode_group_value(*dpt, data, value)) {
		return false;
	}
	char text[64];
	std::printf("%s%s %s", first ? "  ~ " : ", ", dpt_text, value.format(text, sizeof(text)));
	return true;
}

/// The guess of the monitor: what the data length suggests.
void print_guess(const GroupData& data) {
	if (data.service == GroupService::read) {
		return;
	}
	if (data.short_form) {
		bool first = !try_decode("1.001", data, true);
		std::printf("%s6-bit %u", first ? "  ~ " : ", ", static_cast<unsigned>(data.data[0]));
		return;
	}
	bool printed = false;
	switch (data.length) {
	case 1:
		printed = try_decode("5.010", data, true);
		try_decode("5.001", data, !printed);
		break;
	case 2:
		printed = try_decode("9.001", data, true) || try_decode("7.001", data, true);
		break;
	case 3:
		printed = try_decode("10.001", data, true);
		printed = try_decode("11.001", data, !printed) || printed;
		try_decode("232.600", data, !printed);
		break;
	case 4:
		printed = try_decode("14.000", data, true);
		try_decode("12.001", data, !printed);
		break;
	case 14:
		try_decode("16.001", data, true);
		break;
	default:
		break;
	}
}

void print_telegram(const LData& frame, const GroupData& data, const char* prefix) {
	char source[hc::knx::IndividualAddress::kMaxText];
	char destination[GroupAddress::kMaxText];
	char hex[GroupData::kMaxData * 3 + 1];
	std::printf("%s%s -> %s %s%s%s%s", prefix, frame.source.format(source, sizeof(source)),
		frame.group_address().format(destination, sizeof(destination)), hc::knx::group_service_name(data.service),
		data.length > 0 ? " " : "", hc::knx::format_hex(data.data, data.length, hex, sizeof(hex)),
		frame.repeated ? " (repeated)" : "");
}

enum class Command { monitor, write, read };

/// Drives one command on the tunnel's events.
struct Tool : hc::knx::KnxTunnelListener {
	Tool(KnxTunnel& tunnel_, const hc::Clock& clock_) : tunnel(tunnel_), clock(clock_) {}

	void tunnel_state_changed(const KnxTunnel& t, TunnelState previous) override {
		(void) previous;
		if (t.state() == TunnelState::up) {
			was_up = true;
			if (command != Command::monitor && token == 0) {
				token = tunnel.send(request);
				if (token == 0) {
					finish(kNoTunnel, "the tunnel refused the telegram");
				}
			}
			if (command == Command::monitor) {
				std::fprintf(stderr, "monitoring; Ctrl-C ends\n");
			}
			return;
		}
		if (t.state() == TunnelState::down && (!was_up || command != Command::monitor)) {
			char reason[KnxTunnel::kMaxReason + 32];
			std::snprintf(reason, sizeof(reason), "no tunnel: %s", t.reason());
			finish(kNoTunnel, reason);
		}
	}

	void tunnel_frame(const KnxTunnel&, const LData& frame) override {
		GroupData data;
		if (!hc::knx::group_data(frame, data)) {
			return;
		}
		if (command == Command::monitor) {
			print_telegram(frame, data, "");
			print_guess(data);
			std::printf("\n");
			std::fflush(stdout);
			return;
		}
		if (command == Command::read && confirmed && data.service == GroupService::response &&
			frame.group_address() == address) {
			Value value;
			hc::DptResult result = hc::knx::decode_group_value(dpt, data, value);
			print_telegram(frame, data, "");
			char text[64];
			char dpt_text[Dpt::kMaxText];
			if (result) {
				std::printf("  = %s (%s)\n", value.format(text, sizeof(text)), dpt.format(dpt_text, sizeof(dpt_text)));
				finish(kOk, nullptr);
			} else {
				std::printf("\n");
				std::fprintf(stderr, "the response does not decode as %s: %s (%s)\n", dpt.format(dpt_text, sizeof(dpt_text)),
					hc::dpt_status_name(result.status), result.reason);
				finish(kFailed, nullptr);
			}
		}
	}

	void tunnel_sent(const KnxTunnel&, uint32_t sent, const LData& frame, SendOutcome outcome) override {
		if (sent != token || token == 0) {
			return;
		}
		if (outcome != SendOutcome::confirmed) {
			char reason[64];
			std::snprintf(reason, sizeof(reason), "the gateway reports: %s", hc::knx::send_outcome_name(outcome));
			finish(outcome == SendOutcome::lost ? kNoTunnel : kFailed, reason);
			return;
		}
		GroupData data;
		hc::knx::group_data(frame, data);
		print_telegram(frame, data, "sent ");
		std::printf(" (confirmed)\n");
		if (command == Command::write) {
			finish(kOk, nullptr);
			return;
		}
		confirmed = true;
		deadline = clock.monotonic_ms() + kResponseWaitMs;
	}

	void finish(int code, const char* reason) {
		if (done) {
			return;
		}
		done = true;
		exit_code = code;
		if (reason != nullptr) {
			std::fprintf(stderr, "knxtool: %s\n", reason);
		}
	}

	KnxTunnel& tunnel;
	const hc::Clock& clock;
	Command command = Command::monitor;
	GroupAddress address;
	Dpt dpt;
	LData request;
	uint32_t token = 0;
	bool was_up = false;
	bool confirmed = false;
	bool done = false;
	int exit_code = kOk;
	uint64_t deadline = 0;
};

} // namespace

int main(int argc, char** argv) {
	hc::posix::PosixClock clock;
	hc::posix::StderrLogSink sink;
	hc::Log::set_sink(&sink);

	const char* gateway_text = nullptr;
	bool nat = false;
	int i = 1;
	for (; i < argc; ++i) {
		if (std::strcmp(argv[i], "--gateway") == 0 && i + 1 < argc) {
			gateway_text = argv[++i];
		} else if (std::strcmp(argv[i], "--nat") == 0) {
			nat = true;
		} else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
			usage();
			return kOk;
		} else {
			break;
		}
	}
	if (gateway_text == nullptr) {
		std::fprintf(stderr, "knxtool: --gateway <host[:port]> is required\n");
		usage();
		return kUsage;
	}
	const char* error = nullptr;
	std::optional<hc::knx::GatewayAddress> gateway = hc::knx::GatewayAddress::parse(gateway_text, &error);
	if (!gateway) {
		std::fprintf(stderr, "knxtool: gateway '%s': %s\n", gateway_text, error);
		return kUsage;
	}
	if (i >= argc) {
		usage();
		return kUsage;
	}

	hc::EventLoop loop;
	hc::posix::PosixNetworkDriver network;
	hc::Connectivity connectivity(loop, clock, network);
	network.bind(loop, connectivity);
	hc::posix::PosixUdpSocket socket(loop);
	KnxTunnel tunnel(loop, clock, connectivity, socket);
	Tool tool(tunnel, clock);

	const char* command = argv[i];
	int rest = argc - i - 1;
	if (std::strcmp(command, "monitor") == 0 && rest == 0) {
		tool.command = Command::monitor;
	} else if ((std::strcmp(command, "write") == 0 && rest == 3) || (std::strcmp(command, "read") == 0 && rest == 2)) {
		tool.command = command[0] == 'w' ? Command::write : Command::read;
		std::optional<GroupAddress> address = GroupAddress::parse(argv[i + 1], &error);
		if (!address) {
			std::fprintf(stderr, "knxtool: group address '%s': %s\n", argv[i + 1], error);
			return kUsage;
		}
		std::optional<Dpt> dpt = Dpt::parse(argv[i + 2]);
		if (!dpt) {
			std::fprintf(stderr, "knxtool: '%s' is no datapoint type (expected e.g. 1.001, 9.001)\n", argv[i + 2]);
			return kUsage;
		}
		if (!dpt->supported()) {
			std::fprintf(stderr, "knxtool: datapoint type %s: %s\n", argv[i + 2], dpt->unsupported_reason());
			return kUsage;
		}
		tool.address = *address;
		tool.dpt = *dpt;
		if (tool.command == Command::write) {
			Value value;
			if (!hc::knx::parse_value_text(*dpt, argv[i + 3], value, &error)) {
				std::fprintf(stderr, "knxtool: value '%s' for %s: %s\n", argv[i + 3], argv[i + 2], error);
				return kUsage;
			}
			if (!hc::knx::make_group_value(tool.request, *address, GroupService::write, *dpt, value, &error)) {
				std::fprintf(stderr, "knxtool: value '%s' as %s: %s\n", argv[i + 3], argv[i + 2], error);
				return kUsage;
			}
		} else {
			hc::knx::make_group_read(tool.request, *address);
		}
	} else {
		std::fprintf(stderr, "knxtool: unknown command or wrong number of arguments: %s\n", command);
		usage();
		return kUsage;
	}

	struct sigaction action;
	std::memset(&action, 0, sizeof(action));
	action.sa_handler = on_signal;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, nullptr);
	sigaction(SIGTERM, &action, nullptr);

	tunnel.add_listener(&tool);
	tunnel.set_gateway(&*gateway, nat);
	connectivity.start();
	tunnel.start();
	uint64_t connect_deadline = clock.monotonic_ms() + kConnectWaitMs;
	while (!g_stop && !tool.done) {
		loop.run_once(clock.monotonic_ms());
		uint64_t now = clock.monotonic_ms();
		if (tool.command != Command::monitor && !tunnel.is_up() && !tool.was_up && now >= connect_deadline) {
			tool.finish(kNoTunnel, "no tunnel within 15 s");
			break;
		}
		if (tool.command == Command::read && tool.confirmed && now >= tool.deadline) {
			tool.finish(kFailed, "no A_GroupValue_Response within 5 s");
			break;
		}
		uint64_t deadline = loop.next_deadline();
		if (deadline <= now) {
			continue;
		}
		uint64_t wait = deadline - now;
		socket.wait(wait < 100 ? wait : 100);
	}
	tunnel.stop();
	return g_stop && !tool.done ? kOk : tool.exit_code;
}
