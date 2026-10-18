/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "ehome_service.h"

#include <cstdio>
#include <cstring>

namespace hc {
namespace esp {

namespace {

constexpr const char* kHint = "status | key | key accept";
constexpr const char* kHelp =
	"E-Home connection (doc/ehome.md): state and installation; the installation key in force and a new one "
	"E-Home offered (fingerprints); accept the offered key (runtime images then need its signature)";

} // namespace

EhomeService::EhomeService(Controller& controller, LoopQueue& queue, WebServer& web)
	: client_(queue),
	  fetcher_(controller.loop(), controller.clock(), queue),
	  link_(web.api(), web.feed(), client_, fetcher_),
	  console_command_{"ehome", kHint, kHelp, &EhomeService::console_entry, this} {}

int EhomeService::console_entry(void* context, int argc, char** argv) {
	return static_cast<EhomeService*>(context)->console(argc, argv);
}

int EhomeService::console(int argc, char** argv) {
	char key[kFingerprintSize];
	char pending[kFingerprintSize];
	if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
		std::printf("state     %s%s%s\n", ehome_state_name(link_.state()), link_.error()[0] != '\0' ? ": " : "",
			link_.error());
		std::printf("enrolled  %s, switched %s\n", link_.enrolled() ? "yes" : "no", link_.enabled() ? "on" : "off");
	}
	if ((argc == 2 && (std::strcmp(argv[1], "status") == 0 || std::strcmp(argv[1], "key") == 0))) {
		KeyState state = link_.stored_key(key);
		std::printf("key       %s\n", state == KeyState::ok ? key
			: state == KeyState::damaged ? "DAMAGED (every image is refused: disconnect and enroll again)"
										 : "none (runtime images are not signed)");
		if (link_.pending_key(pending)) {
			std::printf("offered   %s (not in force: ehome key accept)\n", pending);
		}
		return 0;
	}
	if (argc == 3 && std::strcmp(argv[1], "key") == 0 && std::strcmp(argv[2], "accept") == 0) {
		if (!link_.pending_key(pending)) {
			std::printf("refused: E-Home has not offered a new installation key\n");
			return 1;
		}
		const char* code = nullptr;
		const char* error = nullptr;
		if (!link_.accept_key(nullptr, &code, &error)) {
			std::printf("refused: %s (%s)\n", error, code);
			return 1;
		}
		std::printf("installation key %s accepted: runtime images need its signature from now on\n", pending);
		return 0;
	}
	std::printf("usage: ehome %s\n", kHint);
	return 1;
}

} // namespace esp
} // namespace hc
