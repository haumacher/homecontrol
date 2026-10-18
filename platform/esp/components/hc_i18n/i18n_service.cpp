/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "i18n_service.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "hc/log.h"
#include "hc/network_settings.h"
#include "hc/version.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.i18n";
constexpr const char* kHint = "status | set <code> | sync";
constexpr const char* kHelp =
	"The web UI's language (doc/i18n.md): ui.lang, the language pack of this release downloaded from the update "
	"server, a sync now";

} // namespace

// ---- I18nService ------------------------------------------------------------------------------

I18nService::I18nService(Controller& controller, LoopQueue& queue, BlobStore& store)
	: controller_(controller),
	  fetcher_(controller.loop(), controller.clock(), queue),
	  packs_(controller.loop(), controller.time(), controller.settings(), controller.connectivity(), fetcher_, store,
		  kVersion),
	  console_command_{"lang", kHint, kHelp, &I18nService::console_entry, this} {}

int I18nService::console_entry(void* context, int argc, char** argv) {
	return static_cast<I18nService*>(context)->run_command(argc, argv);
}

int I18nService::run_command(int argc, char** argv) {
	if (argc == 1 || (argc == 2 && std::strcmp(argv[1], "status") == 0)) {
		print_status();
		return 0;
	}
	if (argc == 3 && std::strcmp(argv[1], "set") == 0) {
		const char* error = nullptr;
		if (!store_ui_lang(controller_.settings(), argv[2], &error)) {
			std::printf("lang set %s refused: %s\n", argv[2], error);
			return 1;
		}
		Log::info(kTag, "UI language set to %s on the console", argv[2]);
		packs_.language_changed();
		std::printf("ui.lang = %s; the web UI shows it at its next load, the pack sync follows (lang status)\n", argv[2]);
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "sync") == 0) {
		const char* error = nullptr;
		if (!packs_.sync_now(&error)) {
			std::printf("lang sync refused: %s\n", error);
			return 1;
		}
		std::printf("syncing; the outcome follows in the log (lang status)\n");
		return 0;
	}
	std::printf("usage: lang %s\n", kHint);
	return 1;
}

void I18nService::print_status() const {
	std::printf("ui.lang   %s\n", packs_.lang());
	std::printf("pack      %s: %s\n", language_pack_state_name(packs_.state()), packs_.detail());
	std::printf("release   %s (the i18n directory the device asks for)\n", packs_.release());
	std::printf("offered   ");
	if (packs_.offer_count() == 0) {
		std::printf("none known (the manifest was not read yet)");
	}
	for (size_t i = 0; i < packs_.offer_count(); ++i) {
		std::printf("%s%s (%s)", i > 0 ? ", " : "", packs_.offer(i).code, packs_.offer(i).name);
	}
	std::printf("\nfetcher   %s\n", fetcher_.allocated() ? "allocated" : "free");
}

} // namespace esp
} // namespace hc
