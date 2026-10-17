/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_EXTERNAL_SERVICE_H
#define HC_ESP_EXTERNAL_SERVICE_H

#include "console.h"
#include "esp_http_fetcher.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/external_values.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// External values on the device (issue #17, doc/external.md): the core's ExternalValues over the
/// EspHttpFetcher, wired to the controller's loop, time service, settings (location.lat/lon) and
/// network. Configured by the runtime image's `external` member (issue #8:
/// values().configure_json()); without a configuration it does nothing.
///
/// It also carries the console command `external` (console_command(), for the console's extension
/// table).
class ExternalService {
public:
	ExternalService(Controller& controller, LoopQueue& queue, DatapointModel& model);

	ExternalService(const ExternalService&) = delete;
	ExternalService& operator=(const ExternalService&) = delete;

	ExternalValues& values() { return values_; }
	const ExternalValues& values() const { return values_; }

	/// The `external` command for ConsoleContext::extensions.
	const ConsoleCommand* console_command() const { return &console_command_; }

	/// The body of `external status | external fetch <id>` (runs on the event loop). Prints to
	/// stdout; 0 on success, 1 on a refusal (printed with its reason).
	int run_command(int argc, char** argv);

private:
	static int console_entry(void* context, int argc, char** argv);
	void print_status() const;

	Controller& controller_;
	DatapointModel& model_;
	EspHttpFetcher fetcher_;
	ExternalValues values_;
	ConsoleCommand console_command_;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_EXTERNAL_SERVICE_H
