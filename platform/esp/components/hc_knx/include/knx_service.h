/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_KNX_SERVICE_H
#define HC_ESP_KNX_SERVICE_H

#include <cstdint>

#include "esp_transient_socket.h"
#include "esp_udp_socket.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/dpt.h"
#include "hc/knx/adapter.h"
#include "hc/knx/gateway_search.h"
#include "hc/knx/telegram_log.h"
#include "hc/monitor_feed.h"
#include "hc/knx/tunnel.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// KNX on the device: the lwIP UDP socket and the core's KnxTunnel, KnxAdapter and TelegramLog, and
/// the GatewaySearch (its socket and reader task exist only while a search runs),
/// wired to the controller's event loop and network, configured from the settings knx.gateway and
/// knx.nat (doc/knx.md). The objects are statically allocated inside it.
///
/// It also carries what the console's `knx` command needs (knx_console_command()): the writes and
/// reads typed on the console are reported in the log when the gateway confirms them or the bus
/// answers.
class KnxService : public knx::KnxTunnelListener {
public:
	/// How long `knx read` waits for the response.
	static constexpr uint32_t kReadTimeoutMs = 5000;

	/// `model` is the controller's datapoint model (empty until runtime images exist, issue #8).
	KnxService(Controller& controller, LoopQueue& queue, DatapointModel& model);

	KnxService(const KnxService&) = delete;
	KnxService& operator=(const KnxService&) = delete;

	/// Creates the reader task, reads the settings and starts the tunnel (it connects when the
	/// network is up and a gateway is set). Call once, on the event loop's task.
	void init();

	/// Stores knx.gateway ("host[:port]") and connects to it. False with `error`.
	bool set_gateway(const char* text, const char** error);

	/// Erases knx.gateway and closes the tunnel. False with `error` when the setting cannot be erased.
	bool clear_gateway(const char** error);

	/// Stores knx.nat and reconnects in that mode. False with `error`.
	bool set_nat(bool nat, const char** error);

	/// Sends a group write of `value` (text form, hc::knx::parse_value_text()) as `dpt` to `address`.
	/// The confirmation follows in the log. False with `error`.
	bool write(const char* address, const char* dpt, const char* value, const char** error);

	/// Sends a group read of `address`; the response (decoded as `dpt`) or its absence follows in the
	/// log. False with `error`.
	bool read(const char* address, const char* dpt, const char** error);

	knx::KnxTunnel& tunnel() { return tunnel_; }
	knx::KnxAdapter& adapter() { return adapter_; }
	/// The gateway search of the Settings page (/api/knx/search), on a transient socket of its own.
	knx::GatewaySearch& search() { return search_; }
	knx::TelegramLog& telegrams() { return telegrams_; }
	/// The monitor stream that keeps the telegrams (`knx log` reads them there, issue #45 item 10);
	/// nullptr until the web server's live feed is set.
	void set_monitor(const MonitorFeed* monitor) { monitor_ = monitor; }
	const MonitorFeed* monitor() const { return monitor_; }
	const DatapointModel& model() const { return model_; }
	const EspUdpSocket& socket() const { return socket_; }
	uint64_t now_ms() const { return controller_.clock().monotonic_ms(); }

	// KnxTunnelListener
	void tunnel_frame(const knx::KnxTunnel& tunnel, const knx::LData& frame) override;
	void tunnel_sent(const knx::KnxTunnel& tunnel, uint32_t token, const knx::LData& frame,
		knx::SendOutcome outcome) override;

private:
	void read_timeout();

	Controller& controller_;
	DatapointModel& model_;
	EspUdpSocket socket_;
	knx::TelegramLog telegrams_;
	const MonitorFeed* monitor_ = nullptr;
	knx::KnxTunnel tunnel_;
	knx::KnxAdapter adapter_;
	EspUdpSocketFactory search_sockets_;
	knx::GatewaySearch search_;
	uint32_t write_token_ = 0;
	uint32_t read_token_ = 0;
	bool reading_ = false;
	knx::GroupAddress read_address_;
	Dpt read_dpt_;
	TimerHandle read_timer_;
};

/// The body of the console command `knx` (runs on the event loop, like every console command):
///
///   knx status | knx gateway set <host[:port]> | knx gateway clear | knx nat on|off
///   knx log [<count>] | knx write <ga> <dpt> <value> | knx read <ga> <dpt>
///
/// Prints to stdout; returns 0 on success, 1 on a refusal (printed with its reason).
int knx_console_command(KnxService& service, int argc, char** argv);

/// For the console's command table.
constexpr const char* kKnxCommandHint =
	"status | gateway set <host[:port]> | gateway clear | nat on|off | log [<n>] | write <ga> <dpt> <value> | "
	"read <ga> <dpt>";
constexpr const char* kKnxCommandHelp =
	"KNX tunnel (doc/knx.md): state, the gateway (knx.gateway, port 3671 by default) and NAT mode, the last "
	"group telegrams, a group write or read";

} // namespace esp
} // namespace hc

#endif // HC_ESP_KNX_SERVICE_H
