/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_EHOME_SERVICE_H
#define HC_ESP_EHOME_SERVICE_H

#include "esp_ws_client.h"
#include "hc/controller.h"
#include "hc/ehome_link.h"
#include "loop_queue.h"
#include "on_demand_fetcher.h"
#include "web_server.h"

namespace hc {
namespace esp {

/// The E-Home connection on the device (issue #41, doc/ehome.md): the core's EhomeLink over the
/// EspWebSocketClient (wss:// with the certificate bundle) and an OnDemandFetcher for the
/// enrollment (its 8 KB buffer exists only while the enrollment's POST runs), wired to the web
/// server's API and live feed.
class EhomeService {
public:
	EhomeService(Controller& controller, LoopQueue& queue, WebServer& web);

	EhomeService(const EhomeService&) = delete;
	EhomeService& operator=(const EhomeService&) = delete;

	/// Registers the link with the API and connects when enrolled and switched on (on the loop,
	/// after WebServer::start()).
	void start() { link_.start(); }

	EhomeLink& link() { return link_; }

private:
	EspWebSocketClient client_;
	OnDemandFetcher fetcher_;
	EhomeLink link_;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_EHOME_SERVICE_H
