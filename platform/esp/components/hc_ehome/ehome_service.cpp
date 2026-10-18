/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "ehome_service.h"

namespace hc {
namespace esp {

EhomeService::EhomeService(Controller& controller, LoopQueue& queue, WebServer& web)
	: client_(queue),
	  fetcher_(controller.loop(), controller.clock(), queue),
	  link_(web.api(), web.feed(), client_, fetcher_) {}

} // namespace esp
} // namespace hc
