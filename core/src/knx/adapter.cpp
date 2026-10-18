/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/knx/adapter.h"

#include <algorithm>
#include <cstdio>
#include <new>

#include "hc/knx/group_value.h"
#include "hc/log.h"

namespace hc {
namespace knx {

namespace {

constexpr const char* kTag = "hc.knx";

bool numeric(ValueType type) {
	return type == ValueType::unsigned_int || type == ValueType::signed_int || type == ValueType::real ||
		type == ValueType::enumeration;
}

} // namespace

KnxAdapter::KnxAdapter(EventLoop& loop, const Clock& clock, DatapointModel& model, KnxTunnel& tunnel, TelegramLog* log)
	: loop_(loop), clock_(clock), model_(model), tunnel_(tunnel), log_(log) {}

KnxAdapter::~KnxAdapter() {
	loop_.cancel(read_timer_);
	free_tables();
}

void KnxAdapter::free_tables() {
	delete[] entries_;
	delete[] listens_;
	entries_ = nullptr;
	listens_ = nullptr;
	count_ = 0;
	listen_count_ = 0;
}

void KnxAdapter::start() {
	if (started_) {
		return;
	}
	started_ = tunnel_.add_listener(this);
}

bool KnxAdapter::configure(const KnxMapping* mappings, size_t count, const char** error) {
	char reason[160];
	auto refuse = [&](size_t index, const char* text) {
		std::snprintf(reason, sizeof(reason), "mapping %u: %s", static_cast<unsigned>(index), text);
		Log::error(kTag, "refusing the KNX mapping: %s", reason);
		if (error != nullptr) {
			*error = "invalid mapping (the log names it)";
		}
		return false;
	};
	size_t listens = 0;
	for (size_t i = 0; i < count; ++i) {
		const KnxMapping& m = mappings[i];
		const Datapoint* dp = model_.get(m.datapoint);
		if (dp == nullptr) {
			return refuse(i, "no datapoint with this id");
		}
		if (m.transmit_only) {
			if (dp->technology() == nullptr) {
				return refuse(i, "a transmit-only datapoint needs its owner bound first");
			}
			if (dp->technology() == this) {
				return refuse(i, "a transmit-only datapoint must be owned by another component, not the adapter");
			}
			if (dp->transmitter() != nullptr && dp->transmitter() != this) {
				return refuse(i, "the datapoint has another transmitter");
			}
			if (!dp->transmits()) {
				return refuse(i, "a transmit-only datapoint needs the transmit flag");
			}
			if (!m.has_send) {
				return refuse(i, "a transmit-only datapoint needs a sending address");
			}
			if (m.listen_count > 0) {
				return refuse(i, "a transmit-only datapoint listens to no address");
			}
			for (size_t j = 0; j < count; ++j) {
				if (j != i && mappings[j].has_send && mappings[j].send == m.send) {
					char text[64];
					std::snprintf(text, sizeof(text), "mapping %u sends on the same address", static_cast<unsigned>(j));
					return refuse(i, text);
				}
			}
		} else if (dp->technology() != nullptr && dp->technology() != this) {
			return refuse(i, "the datapoint belongs to another technology");
		}
		for (size_t j = 0; j < i; ++j) {
			if (mappings[j].datapoint == m.datapoint) {
				return refuse(i, "the datapoint is mapped twice");
			}
		}
		if (!m.dpt.supported()) {
			return refuse(i, m.dpt.unsupported_reason());
		}
		ValueType type = m.dpt.value_type();
		if (type != dp->type() && !(numeric(type) && numeric(dp->type()))) {
			return refuse(i, "the DPT's value type does not fit the datapoint's");
		}
		if (m.listen_count > KnxMapping::kMaxListen || (m.listen_count > 0 && m.listen == nullptr)) {
			return refuse(i, "more than 8 listening addresses");
		}
		if (!m.has_send && m.listen_count == 0) {
			return refuse(i, "no group address");
		}
		if (!m.has_send && (dp->transmits() || dp->readable())) {
			return refuse(i, "a transmitting or readable datapoint needs a sending address");
		}
		listens += m.listen_count + (m.has_send ? 1 : 0);
	}
	Entry* entries = count > 0 ? new (std::nothrow) Entry[count] : nullptr;
	Listen* listen = listens > 0 ? new (std::nothrow) Listen[listens] : nullptr;
	if ((count > 0 && entries == nullptr) || (listens > 0 && listen == nullptr)) {
		delete[] entries;
		delete[] listen;
		Log::error(kTag, "refusing the KNX mapping: out of memory for %u datapoints", static_cast<unsigned>(count));
		if (error != nullptr) {
			*error = "out of memory";
		}
		return false;
	}
	for (size_t i = 0; i < count; ++i) {
		const KnxMapping& m = mappings[i];
		entries[i] = Entry{m.datapoint, m.dpt, m.has_send, m.send, m.listen_count > 0 ? m.listen[0] : m.send,
			m.transmit_only, false};
	}
	std::sort(entries, entries + count, [](const Entry& a, const Entry& b) { return a.datapoint < b.datapoint; });
	size_t n = 0;
	for (size_t i = 0; i < count; ++i) {
		const Entry& e = entries[i];
		const KnxMapping* m = nullptr;
		for (size_t j = 0; j < count; ++j) {
			if (mappings[j].datapoint == e.datapoint) {
				m = &mappings[j];
				break;
			}
		}
		if (m->has_send) {
			listen[n++] = Listen{m->send, static_cast<uint16_t>(i)};
		}
		for (size_t k = 0; k < m->listen_count; ++k) {
			if (m->has_send && m->listen[k] == m->send) {
				continue; // the sending address listens already
			}
			listen[n++] = Listen{m->listen[k], static_cast<uint16_t>(i)};
		}
	}
	std::stable_sort(listen, listen + n, [](const Listen& a, const Listen& b) {
		return a.address < b.address || (a.address == b.address && a.entry < b.entry);
	});
	for (size_t i = 0; i < count; ++i) {
		if (entries[i].transmit_only) {
			model_.set_transmitter(entries[i].datapoint, *this);
		} else {
			model_.bind(entries[i].datapoint, *this);
		}
	}
	loop_.cancel(read_timer_);
	read_timer_ = TimerHandle();
	free_tables();
	entries_ = entries;
	count_ = count;
	listens_ = listen;
	listen_count_ = n;
	pending_count_ = 0;
	read_cursor_ = count_;
	if (log_ != nullptr) {
		log_->forget_datapoints();
	}
	Log::info(kTag, "knx mapping: %u datapoints on %u group addresses", static_cast<unsigned>(count_),
		static_cast<unsigned>(listen_count_));
	if (tunnel_.is_up()) {
		want_reads_on_start();
	}
	return true;
}

void KnxAdapter::want_reads_on_start() {
	for (size_t i = 0; i < count_; ++i) {
		const Datapoint* dp = model_.get(entries_[i].datapoint);
		if (dp != nullptr && dp->read_on_start() && !entries_[i].transmit_only) {
			entries_[i].read_wanted = true;
		}
	}
	read_cursor_ = 0;
	schedule_read();
}

void KnxAdapter::clear() {
	loop_.cancel(read_timer_);
	read_timer_ = TimerHandle();
	free_tables();
	pending_count_ = 0;
	read_cursor_ = 0;
	if (log_ != nullptr) {
		log_->forget_datapoints();
	}
}

size_t KnxAdapter::reads_pending() const {
	size_t result = 0;
	for (size_t i = 0; i < count_; ++i) {
		result += entries_[i].read_wanted ? 1 : 0;
	}
	return result;
}

bool KnxAdapter::read(const Datapoint& datapoint) {
	const Entry* entry = entry_of(datapoint.id());
	if (entry == nullptr) {
		Log::warn(kTag, "refusing read of '%s': not mapped to a group address", datapoint.name());
		return false;
	}
	if (entry->transmit_only) {
		Log::warn(kTag, "refusing read of '%s': the adapter only transmits it for its owner", datapoint.name());
		return false;
	}
	if (!tunnel_.is_up()) {
		Log::warn(kTag, "refusing read of '%s': the KNX tunnel is %s (%s)", datapoint.name(),
			tunnel_state_name(tunnel_.state()), tunnel_.reason());
		return false;
	}
	size_t index = static_cast<size_t>(entry - entries_);
	if (!entries_[index].read_wanted) {
		entries_[index].read_wanted = true;
		if (read_cursor_ >= count_) {
			read_cursor_ = index;
		}
		schedule_read();
	}
	return true;
}

const KnxAdapter::Entry* KnxAdapter::entry_of(uint16_t datapoint) const {
	const Entry* begin = entries_;
	const Entry* end = begin + count_;
	const Entry* it =
		std::lower_bound(begin, end, datapoint, [](const Entry& e, uint16_t id) { return e.datapoint < id; });
	return it != end && it->datapoint == datapoint ? it : nullptr;
}

std::optional<uint16_t> KnxAdapter::sender_of(GroupAddress address) const {
	for (size_t i = first_listen(address); i < listen_count_ && listens_[i].address == address; ++i) {
		const Entry& entry = entries_[listens_[i].entry];
		if (entry.has_send && entry.send == address) {
			return entry.datapoint; // listens_ is sorted by entry, entries_ by datapoint id
		}
	}
	return std::nullopt;
}

size_t KnxAdapter::first_listen(GroupAddress address) const {
	const Listen* begin = listens_;
	const Listen* end = begin + listen_count_;
	const Listen* it =
		std::lower_bound(begin, end, address, [](const Listen& l, GroupAddress a) { return l.address < a; });
	return static_cast<size_t>(it - begin);
}

bool KnxAdapter::command(const Datapoint& datapoint, const Value& value) {
	const Entry* entry = entry_of(datapoint.id());
	if (entry == nullptr || !entry->has_send) {
		Log::warn(kTag, "refusing command for '%s': not mapped to a sending group address", datapoint.name());
		return false;
	}
	char address[GroupAddress::kMaxText];
	if (!tunnel_.is_up()) {
		Log::warn(kTag, "refusing command for '%s': the KNX tunnel is %s (%s)", datapoint.name(),
			tunnel_state_name(tunnel_.state()), tunnel_.reason());
		return false;
	}
	LData frame;
	const char* error = nullptr;
	if (!make_group_value(frame, entry->send, GroupService::write, entry->dpt, value, &error)) {
		char dpt[Dpt::kMaxText];
		Log::warn(kTag, "refusing command for '%s': %s cannot encode it (%s)", datapoint.name(),
			entry->dpt.format(dpt, sizeof(dpt)), error);
		return false;
	}
	if (pending_count_ == kPendingCapacity) {
		Log::warn(kTag, "refusing command for '%s': %u writes wait for their confirmation", datapoint.name(),
			static_cast<unsigned>(kPendingCapacity));
		return false;
	}
	uint32_t token = tunnel_.send(frame);
	if (token == 0) {
		Log::warn(kTag, "refusing command for '%s' to %s: the tunnel refused the frame", datapoint.name(),
			entry->send.format(address, sizeof(address)));
		return false;
	}
	pending_[pending_count_++] = PendingSend{token, datapoint.id()};
	return true;
}

void KnxAdapter::tunnel_state_changed(const KnxTunnel& tunnel, TunnelState previous) {
	(void) previous;
	if (tunnel.state() == TunnelState::up) {
		want_reads_on_start();
		return;
	}
	if (tunnel.state() == TunnelState::down) {
		loop_.cancel(read_timer_);
		read_timer_ = TimerHandle();
		read_cursor_ = count_;
		for (size_t i = 0; i < count_; ++i) {
			entries_[i].read_wanted = false;
		}
		pending_count_ = 0;
		for (size_t i = 0; i < count_; ++i) {
			if (!entries_[i].transmit_only) {
				model_.report_stale(*this, entries_[i].datapoint);
			}
		}
	}
}

void KnxAdapter::schedule_read() {
	if (read_timer_.valid()) {
		return;
	}
	// The next waiting read from the cursor on, around the end once (a read asked for behind the
	// cursor waits for its turn).
	size_t start = read_cursor_ < count_ ? read_cursor_ : 0;
	size_t found = count_;
	for (size_t k = 0; k < count_ && found == count_; ++k) {
		size_t i = (start + k) % count_;
		if (entries_[i].read_wanted) {
			found = i;
		}
	}
	read_cursor_ = found;
	if (found >= count_) {
		return;
	}
	read_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kReadSpacingMs,
		Callback::member<KnxAdapter, &KnxAdapter::read_due>(this));
}

void KnxAdapter::read_due() {
	read_timer_ = TimerHandle();
	if (read_cursor_ >= count_ || !tunnel_.is_up()) {
		return;
	}
	Entry& entry = entries_[read_cursor_];
	LData frame;
	make_group_read(frame, entry.has_send ? entry.send : entry.first_listen);
	if (tunnel_.send(frame) != 0) {
		entry.read_wanted = false;
		++read_cursor_;
	}
	// A full queue: the same read is tried again after the spacing.
	schedule_read();
}

void KnxAdapter::tunnel_frame(const KnxTunnel& tunnel, const LData& frame) {
	(void) tunnel;
	GroupData data;
	if (!group_data(frame, data)) {
		return; // not group communication of the application layer: nothing for the datapoints
	}
	log_telegram(TelegramRecord::Direction::received, frame, data);
	if (data.service == GroupService::read) {
		answer_read(frame.group_address());
		return;
	}
	deliver(frame, data, TelegramRecord::kNoDatapoint);
}

void KnxAdapter::deliver(const LData& frame, const GroupData& data, uint16_t except) {
	GroupAddress address = frame.group_address();
	for (size_t i = first_listen(address); i < listen_count_ && listens_[i].address == address; ++i) {
		const Entry& entry = entries_[listens_[i].entry];
		if (entry.datapoint == except) {
			continue;
		}
		if (entry.transmit_only) {
			char text[GroupAddress::kMaxText];
			char source[IndividualAddress::kMaxText];
			const Datapoint* dp = model_.get(entry.datapoint);
			Log::debug(kTag, "%s from %s: '%s' is transmit-only, not updated", address.format(text, sizeof(text)),
				frame.source.format(source, sizeof(source)), dp != nullptr ? dp->name() : "?");
			continue;
		}
		Value value;
		DptResult result = decode_group_value(entry.dpt, data, value);
		if (!result) {
			char text[GroupAddress::kMaxText];
			char source[IndividualAddress::kMaxText];
			char dpt[Dpt::kMaxText];
			char hex[GroupData::kMaxData * 3 + 1];
			const Datapoint* dp = model_.get(entry.datapoint);
			Log::warn(kTag, "%s from %s: %s refuses %s%s (%s: %s); '%s' unchanged", address.format(text, sizeof(text)),
				frame.source.format(source, sizeof(source)), entry.dpt.format(dpt, sizeof(dpt)),
				format_hex(data.data, data.length, hex, sizeof(hex)), data.short_form ? " (short)" : "",
				dpt_status_name(result.status), result.reason, dp != nullptr ? dp->name() : "?");
			continue;
		}
		model_.report(*this, entry.datapoint, value);
	}
}

void KnxAdapter::answer_read(GroupAddress address) {
	const Datapoint* unanswered = nullptr;
	for (size_t i = first_listen(address); i < listen_count_ && listens_[i].address == address; ++i) {
		const Entry& entry = entries_[listens_[i].entry];
		if (!entry.has_send || entry.send != address) {
			continue;
		}
		const Datapoint* dp = model_.get(entry.datapoint);
		if (dp == nullptr || !dp->readable()) {
			continue;
		}
		if (dp->quality() != Quality::good) {
			unanswered = dp;
			continue;
		}
		LData frame;
		const char* error = nullptr;
		char text[GroupAddress::kMaxText];
		if (!make_group_value(frame, address, GroupService::response, entry.dpt, dp->value(), &error)) {
			Log::warn(kTag, "cannot answer the read of %s with '%s': %s", address.format(text, sizeof(text)), dp->name(),
				error);
			return;
		}
		if (pending_count_ < kPendingCapacity) {
			uint32_t token = tunnel_.send(frame);
			if (token != 0) {
				pending_[pending_count_++] = PendingSend{token, dp->id()};
			}
		} else {
			Log::warn(kTag, "not answering the read of %s: %u sends wait for their confirmation",
				address.format(text, sizeof(text)), static_cast<unsigned>(kPendingCapacity));
		}
		return;
	}
	if (unanswered != nullptr) {
		char text[GroupAddress::kMaxText];
		Log::info(kTag, "not answering the read of %s: '%s' has no good value (%s)", address.format(text, sizeof(text)),
			unanswered->name(), quality_name(unanswered->quality()));
	}
}

void KnxAdapter::tunnel_sent(const KnxTunnel& tunnel, uint32_t token, const LData& frame, SendOutcome outcome) {
	(void) tunnel;
	uint16_t sender = TelegramRecord::kNoDatapoint;
	for (size_t i = 0; i < pending_count_; ++i) {
		if (pending_[i].token == token) {
			sender = pending_[i].datapoint;
			pending_[i] = pending_[--pending_count_];
			break;
		}
	}
	GroupData data;
	if (!group_data(frame, data)) {
		return;
	}
	if (outcome != SendOutcome::confirmed) {
		if (sender != TelegramRecord::kNoDatapoint) {
			char text[GroupAddress::kMaxText];
			const Datapoint* dp = model_.get(sender);
			Log::warn(kTag, "%s of '%s' to %s: %s", group_service_name(data.service), dp != nullptr ? dp->name() : "?",
				frame.group_address().format(text, sizeof(text)), send_outcome_name(outcome));
		}
		return;
	}
	log_telegram(TelegramRecord::Direction::sent, frame, data);
	if (data.service != GroupService::read) {
		deliver(frame, data, sender);
	}
}

void KnxAdapter::log_telegram(TelegramRecord::Direction direction, const LData& frame, const GroupData& data) {
	GroupAddress address = frame.group_address();
	size_t first = first_listen(address);
	uint16_t datapoint = TelegramRecord::kNoDatapoint;
	if (first < listen_count_ && listens_[first].address == address) {
		datapoint = entries_[listens_[first].entry].datapoint;
	}
	char source[IndividualAddress::kMaxText];
	char text[GroupAddress::kMaxText];
	char hex[GroupData::kMaxData * 3 + 1];
	Log::debug(kTag, "%s %s -> %s %s %s", direction == TelegramRecord::Direction::received ? "rx" : "tx",
		frame.source.format(source, sizeof(source)), address.format(text, sizeof(text)), group_service_name(data.service),
		format_hex(data.data, data.length, hex, sizeof(hex)));
	if (log_ == nullptr) {
		return;
	}
	TelegramRecord record;
	record.direction = direction;
	record.source = frame.source;
	record.destination = address;
	record.service = data.service;
	record.priority = frame.priority;
	record.repeated = frame.repeated;
	record.hop_count = frame.hop_count;
	record.length = data.length;
	for (size_t i = 0; i < data.length; ++i) {
		record.data[i] = data.data[i];
	}
	record.short_form = data.short_form;
	record.datapoint = datapoint;
	log_->append(record);
}

} // namespace knx
} // namespace hc
