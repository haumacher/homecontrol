/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "external_service.h"

#include <cstdio>
#include <cstring>

namespace hc {
namespace esp {

namespace {

constexpr const char* kHint = "status | fetch <id>";
constexpr const char* kHelp =
	"External values (doc/external.md): the HTTP JSON sources of the runtime image (Open-Meteo weather, ...), "
	"their state and values; fetch one now";

} // namespace

ExternalService::ExternalService(Controller& controller, DatapointModel& model, HttpFetcher& fetcher)
	: controller_(controller),
	  model_(model),
	  fetcher_(fetcher),
	  values_(controller.loop(), controller.time(), controller.settings(), controller.connectivity(), model, fetcher_),
	  console_command_{"external", kHint, kHelp, &ExternalService::console_entry, this} {}

int ExternalService::console_entry(void* context, int argc, char** argv) {
	return static_cast<ExternalService*>(context)->run_command(argc, argv);
}

int ExternalService::run_command(int argc, char** argv) {
	if (argc == 1 || (argc == 2 && std::strcmp(argv[1], "status") == 0)) {
		print_status();
		return 0;
	}
	if (argc == 3 && std::strcmp(argv[1], "fetch") == 0) {
		const char* error = nullptr;
		if (!values_.fetch_now(argv[2], &error)) {
			std::printf("external fetch %s refused: %s\n", argv[2], error);
			return 1;
		}
		std::printf("fetching %s; the outcome follows in the log (external status shows the values)\n", argv[2]);
		return 0;
	}
	std::printf("usage: external %s\n", kHint);
	return 1;
}

void ExternalService::print_status() const {
	size_t count = values_.size();
	if (count == 0) {
		std::printf("no external sources (the runtime image configures them, issue #8)\n");
		return;
	}
	std::printf("table %u B, fetch %s\n", static_cast<unsigned>(values_.table_bytes()),
		fetcher_.busy() ? "waiting or running on the fetch worker" : "idle");
	uint64_t now = controller_.clock().monotonic_ms();
	for (size_t i = 0; i < count; ++i) {
		ExternalSourceStatus status;
		if (!values_.status(i, status)) {
			continue;
		}
		std::printf("%s: %s%s%s every %lu s, %lu fetch(es), %lu failure(s) in a row%s\n", status.id,
			status.preset[0] != '\0' ? "preset " : "", status.preset[0] != '\0' ? status.preset : "",
			status.preset[0] != '\0' ? "" : status.url, static_cast<unsigned long>(status.interval_s),
			static_cast<unsigned long>(status.fetches), static_cast<unsigned long>(status.failures),
			status.fetching ? ", fetching now" : "");
		if (status.preset[0] != '\0') {
			std::printf("  url   %s\n", status.url);
		}
		if (status.last_ok_ms != 0) {
			std::printf("  last  ok %llu s ago\n", static_cast<unsigned long long>((now - status.last_ok_ms) / 1000));
		}
		if (status.last_error[0] != '\0') {
			std::printf("  error %s\n", status.last_error);
		}
		std::printf("  next  in %llu s\n",
			static_cast<unsigned long long>(status.next_ms > now ? (status.next_ms - now) / 1000 : 0));
		for (size_t b = 0; b < status.bindings; ++b) {
			ExternalBindingStatus binding;
			if (!values_.binding(i, b, binding)) {
				continue;
			}
			const Datapoint* dp = model_.get(binding.datapoint);
			char value[40] = "-";
			if (dp != nullptr && !dp->value().empty()) {
				dp->value().format(value, sizeof(value));
			}
			std::printf("  %-28s %10s %-5s %-7s <- %s", dp != nullptr ? dp->name() : "?", value, binding.unit,
				dp != nullptr ? quality_name(dp->quality()) : "?", binding.path);
			if (binding.scale != 1 || binding.offset != 0) {
				std::printf(" * %g + %g", binding.scale, binding.offset);
			}
			std::printf("\n");
		}
	}
}

} // namespace esp
} // namespace hc
