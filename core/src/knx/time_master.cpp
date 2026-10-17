/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/knx/time_master.h"

#include <cstdio>
#include <cstring>

#include "hc/log.h"
#include "hc/tz.h"

namespace hc {
namespace knx {

namespace {

constexpr const char* kTag = "hc.knx";

const char* default_name(int kind) {
	switch (kind) {
		case 0: return "knx.time";
		case 1: return "knx.date";
		default: return "knx.datetime";
	}
}

Dpt dpt_of(int kind) {
	switch (kind) {
		case 0: return Dpt{10, 1};
		case 1: return Dpt{11, 1};
		default: return Dpt{19, 1};
	}
}

/// Floor division for a positive divisor.
int64_t floor_div(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

} // namespace

TimeMaster::TimeMaster(EventLoop& loop, const TimeSource& time, DatapointModel& model, const KnxAdapter& adapter)
	: loop_(loop), time_(time), model_(model), adapter_(adapter) {}

TimeMaster::~TimeMaster() {
	loop_.cancel(tick_);
	loop_.cancel(kick_);
}

bool TimeMaster::configure(const TimeMasterConfig& config, const char** error) {
	char reason[160];
	auto refuse = [&]() {
		Log::error(kTag, "refusing the time master configuration: %s", reason);
		if (error != nullptr) {
			*error = "invalid time master configuration (the log names it)";
		}
		return false;
	};
	if (model_.notifying()) {
		std::snprintf(reason, sizeof(reason), "called from a datapoint listener (post it to the event loop)");
		return refuse();
	}
	if (config.interval_s < TimeMasterConfig::kMinIntervalS || config.interval_s > TimeMasterConfig::kMaxIntervalS) {
		std::snprintf(reason, sizeof(reason), "interval %u s outside %u..%u s", static_cast<unsigned>(config.interval_s),
			static_cast<unsigned>(TimeMasterConfig::kMinIntervalS), static_cast<unsigned>(TimeMasterConfig::kMaxIntervalS));
		return refuse();
	}
	const TimeMasterConfig::Output* configured[kMaxOutputs] = {&config.time, &config.date, &config.date_time};
	Output outputs[kMaxOutputs] = {};
	size_t count = 0;
	for (int kind = 0; kind < static_cast<int>(kMaxOutputs); ++kind) {
		const TimeMasterConfig::Output& o = *configured[kind];
		if (!o.enabled) {
			continue;
		}
		Dpt dpt = dpt_of(kind);
		char dpt_text[Dpt::kMaxText];
		char address[GroupAddress::kMaxText];
		dpt.format(dpt_text, sizeof(dpt_text));
		o.address.format(address, sizeof(address));
		const char* name = o.datapoint != nullptr ? o.datapoint : default_name(kind);
		const Datapoint* dp = model_.find(name);
		if (dp == nullptr) {
			std::snprintf(reason, sizeof(reason), "%s on %s: no datapoint '%s' in the model", dpt_text, address, name);
			return refuse();
		}
		if (dp->type() != dpt.value_type()) {
			std::snprintf(reason, sizeof(reason), "%s on %s: '%s' is a %s datapoint, the DPT needs %s", dpt_text, address,
				name, value_type_name(dp->type()), value_type_name(dpt.value_type()));
			return refuse();
		}
		if (!dp->transmits()) {
			std::snprintf(reason, sizeof(reason), "%s on %s: '%s' does not transmit (no T flag)", dpt_text, address, name);
			return refuse();
		}
		if (dp->technology() != nullptr && dp->technology() != this) {
			std::snprintf(reason, sizeof(reason), "%s on %s: '%s' belongs to %s", dpt_text, address, name,
				dp->technology()->name());
			return refuse();
		}
		if (dp->transmitter() != nullptr && dp->transmitter() != static_cast<const Technology*>(&adapter_)) {
			std::snprintf(reason, sizeof(reason), "%s on %s: '%s' is transmitted by %s", dpt_text, address, name,
				dp->transmitter()->name());
			return refuse();
		}
		for (size_t j = 0; j < count; ++j) {
			if (outputs[j].address == o.address) {
				std::snprintf(reason, sizeof(reason), "the address %s is configured twice", address);
				return refuse();
			}
		}
		std::optional<uint16_t> sender = adapter_.sender_of(o.address);
		if (sender && *sender != dp->id()) {
			const Datapoint* other = model_.get(*sender);
			std::snprintf(reason, sizeof(reason), "%s on %s: the address is the sending address of '%s'", dpt_text,
				address, other != nullptr ? other->name() : "?");
			return refuse();
		}
		outputs[count++] = Output{static_cast<Kind>(kind), dp->id(), o.address};
	}
	for (size_t i = 0; i < count; ++i) {
		model_.bind(outputs[i].datapoint, *this);
	}
	stop();
	for (size_t i = 0; i < kMaxOutputs; ++i) {
		outputs_[i] = outputs[i];
	}
	count_ = count;
	interval_s_ = config.interval_s;
	at_midnight_ = config.at_midnight;
	on_synchronized_ = config.on_synchronized;
	last_utc_ = 0;
	last_date_ = Date();
	Log::info(kTag, "time master: %u addresses, every %u s%s%s", static_cast<unsigned>(count_),
		static_cast<unsigned>(interval_s_), at_midnight_ ? ", at midnight" : "",
		on_synchronized_ ? ", on synchronization" : "");
	if (count_ > 0 && time_.synchronized()) {
		kick_ = loop_.schedule_at(time_.monotonic_ms(), Callback::member<TimeMaster, &TimeMaster::kick>(this));
	}
	return true;
}

void TimeMaster::clear() {
	stop();
	count_ = 0;
}

void TimeMaster::stop() {
	loop_.cancel(tick_);
	loop_.cancel(kick_);
	tick_ = TimerHandle();
	kick_ = TimerHandle();
}

size_t TimeMaster::mappings(KnxMapping* out, size_t capacity) const {
	size_t n = 0;
	for (size_t i = 0; i < count_ && n < capacity; ++i) {
		KnxMapping m;
		m.datapoint = outputs_[i].datapoint;
		m.dpt = dpt_of(static_cast<int>(outputs_[i].kind));
		m.has_send = true;
		m.send = outputs_[i].address;
		m.transmit_only = true;
		out[n++] = m;
	}
	return n;
}

bool TimeMaster::command(const Datapoint& datapoint, const Value& value) {
	(void) value;
	Log::warn(kTag, "refusing command for '%s': the time master sends the controller's clock", datapoint.name());
	return false;
}

void TimeMaster::kick() {
	kick_ = TimerHandle();
	if (count_ == 0 || !time_.synchronized()) {
		return;
	}
	if (on_synchronized_) {
		broadcast("configured");
	}
	arm();
}

void TimeMaster::time_changed(uint8_t events) {
	if (count_ == 0) {
		return;
	}
	if (!time_.synchronized()) {
		if ((events & TimeEvents::kSynchronizationLost) != 0 || tick_.valid()) {
			Log::warn(kTag, "time master: the wall clock lost synchronization; the time broadcasts stop");
			stop();
			for (size_t i = 0; i < count_; ++i) {
				model_.report_stale(*this, outputs_[i].datapoint);
			}
		}
		return;
	}
	const char* why = nullptr;
	if ((events & TimeEvents::kSynchronized) != 0) {
		Log::info(kTag, "time master: broadcasting the time on %u addresses every %u s", static_cast<unsigned>(count_),
			static_cast<unsigned>(interval_s_));
		if (on_synchronized_) {
			why = "synchronized";
		}
	}
	int64_t now = time_.now_utc().seconds;
	if ((events & TimeEvents::kClockJumped) != 0) {
		why = "clock jumped";
	}
	if ((events & TimeEvents::kOffsetChanged) != 0 && (last_utc_ == 0 || time_.utc_offset(now) != last_offset_)) {
		why = "UTC offset changed";
	}
	if ((events & TimeEvents::kDayChanged) != 0 && at_midnight_) {
		Date today;
		if (time_.local_date(now, today) && !(today == last_date_)) {
			why = "midnight";
		}
	}
	if (why != nullptr) {
		broadcast(why);
	}
	arm();
}

void TimeMaster::tick() {
	tick_ = TimerHandle();
	if (count_ == 0 || !time_.synchronized()) {
		return;
	}
	if (time_.now_utc().seconds >= target_utc_) {
		broadcast("interval");
	}
	arm();
}

void TimeMaster::arm() {
	loop_.cancel(tick_);
	tick_ = TimerHandle();
	WallTime now = time_.now_utc();
	if (count_ == 0 || !now.synchronized) {
		return;
	}
	// The next multiple of the interval on the local time line; an offset change before it
	// re-arms (kOffsetChanged).
	int64_t offset = time_.utc_offset(now.seconds);
	int64_t local = now.seconds + offset;
	int64_t interval = interval_s_;
	target_utc_ = (floor_div(local, interval) + 1) * interval - offset;
	int64_t wait_ms = (target_utc_ - now.seconds) * 1000;
	if (wait_ms > static_cast<int64_t>(kMaxTimerMs)) {
		wait_ms = kMaxTimerMs;
	}
	tick_ = loop_.schedule_at(time_.monotonic_ms() + static_cast<uint64_t>(wait_ms),
		Callback::member<TimeMaster, &TimeMaster::tick>(this));
}

void TimeMaster::broadcast(const char* why) {
	WallTime now = time_.now_utc();
	LocalTime local;
	if (!now.synchronized || !time_.to_local(now.seconds, local)) {
		return;
	}
	Date date{static_cast<uint16_t>(local.year), local.month, local.day};
	uint8_t dow = static_cast<uint8_t>(weekday_from_days(days_from_civil(local.year, local.month, local.day)));
	uint8_t second = local.second > 59 ? 59 : local.second; // a leap second
	size_t sent = 0;
	for (size_t i = 0; i < count_; ++i) {
		Value value;
		switch (outputs_[i].kind) {
			case Kind::time: value = Value::of_time(TimeOfDay{dow, local.hour, local.minute, second}); break;
			case Kind::date: value = Value::of_date(date); break;
			case Kind::date_time: {
				DateTime dt;
				dt.year = static_cast<uint16_t>(local.year);
				dt.month = local.month;
				dt.day = local.day;
				dt.day_of_week = dow;
				dt.hour = local.hour;
				dt.minute = local.minute;
				dt.second = second;
				// Working days are not known: NWD. Only synchronized time is sent, so the clock has an
				// external sync signal (CLQ), and its source (SNTP or the host's clock) is reliable (SRC).
				dt.flags = DateTime::kNoWorkingDay | DateTime::kClockQuality | DateTime::kSyncReliable;
				if (local.dst) {
					dt.flags |= DateTime::kSummerTime;
				}
				value = Value::of_date_time(dt);
				break;
			}
		}
		if (model_.publish(*this, outputs_[i].datapoint, value) == DatapointStatus::ok) {
			++sent;
		}
	}
	last_utc_ = now.seconds;
	last_date_ = date;
	last_offset_ = time_.utc_offset(now.seconds);
	Log::debug(kTag, "time master (%s): %04d-%02u-%02u %02u:%02u:%02u %s sent to %u of %u addresses", why,
		static_cast<int>(local.year), local.month, local.day, local.hour, local.minute, second, local.zone,
		static_cast<unsigned>(sent), static_cast<unsigned>(count_));
}

} // namespace knx
} // namespace hc
