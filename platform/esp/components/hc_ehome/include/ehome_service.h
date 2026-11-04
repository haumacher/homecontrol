/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_EHOME_SERVICE_H
#define HC_ESP_EHOME_SERVICE_H

#include "console.h"
#include "esp_ws_client.h"
#include "hc/controller.h"
#include "hc/ehome_link.h"
#include "loop_queue.h"
#include "hc/platform/http_fetcher.h"
#include "web_server.h"

namespace hc {
namespace esp {

/// The E-Home connection on the device (issue #41, doc/ehome.md): the core's EhomeLink over the
/// EspWebSocketClient (wss:// with the certificate bundle) and its lane of the one fetch worker
/// for the enrollment (FetchService, issue #45 item 13), wired to the web
/// server's API and live feed. It carries the console command `ehome` (console_command(): the
/// connection's state, the installation key, accepting a key E-Home offered).
class EhomeService {
public:
	EhomeService(LoopQueue& queue, WebServer& web, HttpFetcher& fetcher);

	EhomeService(const EhomeService&) = delete;
	EhomeService& operator=(const EhomeService&) = delete;

	/// Registers the link with the API and connects when enrolled and switched on (on the loop,
	/// after WebServer::start()).
	void start() { link_.start(); }

	EhomeLink& link() { return link_; }
	const ConsoleCommand* console_command() const { return &console_command_; }

private:
	static int console_entry(void* context, int argc, char** argv);
	int console(int argc, char** argv);

	EspWebSocketClient client_;
	HttpFetcher& fetcher_;
	EhomeLink link_;
	ConsoleCommand console_command_;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_EHOME_SERVICE_H
