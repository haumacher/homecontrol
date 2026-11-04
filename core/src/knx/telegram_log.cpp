/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/knx/telegram_log.h"

namespace hc {
namespace knx {

void TelegramLog::append(const TelegramRecord& record) {
	TelegramRecord stored = record;
	stored.monotonic_ms = clock_.monotonic_ms();
	WallTime wall = clock_.wall_time();
	stored.wall_seconds = wall.seconds;
	stored.wall_synchronized = wall.synchronized;
	++total_;
	if (listener_ != nullptr) {
		listener_->telegram_logged(stored, total_ - 1);
	}
}

} // namespace knx
} // namespace hc
