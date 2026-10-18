/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_I18N_SERVICE_H
#define HC_ESP_I18N_SERVICE_H

#include "console.h"
#include "on_demand_fetcher.h"
#include "hc/controller.h"
#include "hc/language_packs.h"
#include "hc/platform/blob_store.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The language packs on the device (doc/i18n.md): LanguagePacks over the OnDemandFetcher (HTTPS
/// with the certificate bundle) and the LittleFS blob store (`/storage/state/i18n.<lang>.json.gz`,
/// served by the web server at `/i18n/<lang>.json`), and the console command `lang`.
class I18nService {
public:
	I18nService(Controller& controller, LoopQueue& queue, BlobStore& store);

	I18nService(const I18nService&) = delete;
	I18nService& operator=(const I18nService&) = delete;

	/// Schedules the boot sync (after the blob store started).
	void start() { packs_.start(); }

	LanguagePacks& packs() { return packs_; }

	/// The `lang` command for ConsoleContext::extensions.
	const ConsoleCommand* console_command() const { return &console_command_; }

	/// `lang status | set <code> | sync` (on the event loop). 0 on success, 1 on a refusal (printed
	/// with its reason).
	int run_command(int argc, char** argv);

private:
	static int console_entry(void* context, int argc, char** argv);
	void print_status() const;

	Controller& controller_;
	OnDemandFetcher fetcher_;
	LanguagePacks packs_;
	ConsoleCommand console_command_;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_I18N_SERVICE_H
