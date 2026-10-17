/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/knx/telegram_log.h"

#include <cstdio>

#include "hc/knx/group_value.h"

namespace hc {
namespace knx {

void TelegramLog::append(const TelegramRecord& record) {
	size_t slot = (head_ + count_) % kCapacity;
	if (count_ == kCapacity) {
		slot = head_;
		head_ = (head_ + 1) % kCapacity;
	} else {
		++count_;
	}
	TelegramRecord& stored = records_[slot];
	stored = record;
	stored.monotonic_ms = clock_.monotonic_ms();
	WallTime wall = clock_.wall_time();
	stored.wall_seconds = wall.seconds;
	stored.wall_synchronized = wall.synchronized;
	++total_;
}

const TelegramRecord* TelegramLog::at(size_t index) const {
	if (index >= count_) {
		return nullptr;
	}
	return &records_[(head_ + index) % kCapacity];
}

const TelegramRecord* TelegramLog::find(uint64_t sequence) const {
	if (sequence < first_sequence() || sequence >= total_) {
		return nullptr;
	}
	return at(static_cast<size_t>(sequence - first_sequence()));
}

void TelegramLog::forget_datapoints() {
	for (TelegramRecord& record : records_) {
		record.datapoint = TelegramRecord::kNoDatapoint;
	}
}

void TelegramLog::clear() {
	head_ = 0;
	count_ = 0;
	total_ = 0;
}

const char* TelegramLog::format(const TelegramRecord& record, const DatapointModel* model, char* buffer,
	size_t size) {
	char source[IndividualAddress::kMaxText];
	char destination[GroupAddress::kMaxText];
	char hex[GroupData::kMaxData * 3 + 1];
	const char* name = nullptr;
	if (model != nullptr && record.datapoint != TelegramRecord::kNoDatapoint) {
		const Datapoint* dp = model->get(record.datapoint);
		if (dp != nullptr) {
			name = dp->name();
		}
	}
	std::snprintf(buffer, size, "%llu.%03u %s %s -> %s %s%s%s%s%s%s%s",
		static_cast<unsigned long long>(record.monotonic_ms / 1000), static_cast<unsigned>(record.monotonic_ms % 1000),
		record.direction == TelegramRecord::Direction::received ? "rx" : "tx", record.source.format(source, sizeof(source)),
		record.destination.format(destination, sizeof(destination)), group_service_name(record.service),
		record.length > 0 ? " " : "", format_hex(record.data, record.length, hex, sizeof(hex)),
		record.repeated ? " (repeated)" : "", name != nullptr ? " [" : "", name != nullptr ? name : "",
		name != nullptr ? "]" : "");
	return buffer;
}

} // namespace knx
} // namespace hc
