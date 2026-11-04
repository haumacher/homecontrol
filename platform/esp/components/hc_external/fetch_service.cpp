/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "fetch_service.h"

#include <cstdio>

namespace hc {
namespace esp {

const char* FetchService::describe(char* buffer, size_t size) const {
	const FetchQueue::Lane* running = queue_.running();
	if (running != nullptr) {
		std::snprintf(buffer, size, "one worker, running '%s', %u waiting", running->name(),
			static_cast<unsigned>(queue_.waiting()));
	} else {
		std::snprintf(buffer, size, "one worker, idle (%s)", worker_.allocated() ? "allocated" : "free");
	}
	return buffer;
}

FetchService& fetch_service(Controller& controller, LoopQueue& queue) {
	static FetchService instance(controller, queue);
	return instance;
}

} // namespace esp
} // namespace hc
