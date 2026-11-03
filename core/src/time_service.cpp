/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/time_service.h"

#include <cmath>
#include <cstdio>

#include "hc/log.h"
#include "hc/tz.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.time";

bool refuse(const char** error, const char* text) {
	if (error != nullptr) {
		*error = text;
	}
	return false;
}

bool is_digit(char c) {
	return c >= '0' && c <= '9';
}

bool is_leap(int32_t year) {
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

} // namespace

bool parse_coordinate(const char* text, double min, double max, double& out, const char** error) {
	if (text == nullptr || *text == '\0') {
		return refuse(error, "empty coordinate");
	}
	const char* p = text;
	bool negative = false;
	if (*p == '+' || *p == '-') {
		negative = *p == '-';
		++p;
	}
	double value = 0;
	unsigned digits = 0;
	while (is_digit(*p)) {
		if (++digits > 3) {
			return refuse(error, "coordinate has more than 3 integer digits");
		}
		value = value * 10 + (*p - '0');
		++p;
	}
	if (digits == 0) {
		return refuse(error, "coordinate needs integer digits (\"48.1374\")");
	}
	if (*p == '.') {
		++p;
		double scale = 0.1;
		unsigned decimals = 0;
		while (is_digit(*p)) {
			if (++decimals > 8) {
				return refuse(error, "coordinate has more than 8 decimals");
			}
			value += (*p - '0') * scale;
			scale /= 10;
			++p;
		}
		if (decimals == 0) {
			return refuse(error, "coordinate needs digits after the decimal point");
		}
	}
	if (*p != '\0') {
		return refuse(error, "coordinate must be decimal degrees (\"48.1374\", \"-0.5\")");
	}
	if (negative) {
		value = -value;
	}
	if (value < min || value > max) {
		return refuse(error, "coordinate out of range");
	}
	out = value;
	return true;
}

Date date_from_days(int64_t days) {
	// Howard Hinnant's civil_from_days.
	int64_t z = days + 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	int64_t day = doy - (153 * mp + 2) / 5 + 1;
	int64_t month = mp < 10 ? mp + 3 : mp - 9;
	int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
	Date date;
	date.year = static_cast<uint16_t>(year);
	date.month = static_cast<uint8_t>(month);
	date.day = static_cast<uint8_t>(day);
	return date;
}

unsigned weekday_from_days(int64_t days) {
	// 1970-01-01 was a Thursday (4).
	int64_t index = (days + 3) % 7;
	if (index < 0) {
		index += 7;
	}
	return static_cast<unsigned>(index) + 1;
}

unsigned days_in_month(int32_t year, unsigned month) {
	static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (month < 1 || month > 12) {
		return 0;
	}
	return month == 2 && is_leap(year) ? 29 : kDays[month - 1];
}

// --- TimeSource helpers ---

bool TimeSource::local_time(LocalTime& out) const {
	WallTime now = now_utc();
	return now.synchronized && to_local(now.seconds, out);
}

bool TimeSource::local_date(int64_t utc_seconds, Date& out) const {
	LocalTime local;
	if (!to_local(utc_seconds, local)) {
		return false;
	}
	out.year = static_cast<uint16_t>(local.year);
	out.month = local.month;
	out.day = local.day;
	return true;
}

int32_t TimeSource::utc_offset(int64_t utc_seconds) const {
	LocalTime local;
	if (!to_local(utc_seconds, local)) {
		return 0;
	}
	return utc_offset_seconds(local, utc_seconds);
}

int64_t TimeSource::local_to_utc(const Date& date, unsigned hour, unsigned minute, unsigned second) const {
	int64_t naive = days_from_civil(date.year, date.month, date.day) * 86400 + hour * 3600 + minute * 60 + second;
	int32_t before = utc_offset(naive - 86400);
	int32_t after = utc_offset(naive + 86400);
	if (before == after) {
		return naive - before;
	}
	int64_t with_before = naive - before;
	int64_t with_after = naive - after;
	bool valid_before = utc_offset(with_before) == before;
	bool valid_after = utc_offset(with_after) == after;
	if (valid_before && valid_after) {
		return with_before < with_after ? with_before : with_after; // fold: the first occurrence
	}
	if (valid_before) {
		return with_before;
	}
	if (valid_after) {
		return with_after;
	}
	// A gap: the first instant that has the new offset.
	int64_t low = with_before < with_after ? with_before : with_after;
	int64_t high = with_before < with_after ? with_after : with_before;
	while (low < high) {
		int64_t middle = low + (high - low) / 2;
		if (utc_offset(middle) == after) {
			high = middle;
		} else {
			low = middle + 1;
		}
	}
	return low;
}

bool TimeSource::sun_event_local(const Date& date, SunEvent event, LocalTime& out) const {
	SunTimes times;
	if (!sun_times(date, times) || !times.occurs(event)) {
		return false;
	}
	return to_local(times.at(event), out);
}

// --- TimeService ---

TimeService::TimeService(EventLoop& loop, const Clock& clock, const TimeZone& zone, Settings& settings)
	: loop_(loop), clock_(clock), zone_(zone), settings_(settings) {}

TimeService::~TimeService() {
	if (watch_.valid()) {
		loop_.cancel(watch_);
	}
}

void TimeService::start() {
	if (started_) {
		Log::error(kTag, "refusing start: time service already started");
		return;
	}
	started_ = true;
	load_location();
	WallTime now = clock_.wall_time();
	synchronized_ = now.synchronized;
	clock_offset_ms_ = now.seconds * 1000 - static_cast<int64_t>(clock_.monotonic_ms());
	if (synchronized_) {
		LocalTime local;
		if (zone_.to_local(now.seconds, local)) {
			last_date_.year = static_cast<uint16_t>(local.year);
			last_date_.month = local.month;
			last_date_.day = local.day;
			last_utc_offset_ = utc_offset_seconds(local, now.seconds);
		}
	}
	update_watch();
	if (synchronized_) {
		notify(TimeEvents::kSynchronized);
	}
}

bool TimeService::add_listener(TimeListener& listener, uint8_t events) {
	if (notifying_) {
		Log::error(kTag, "refusing to add a time listener from within a notification");
		return false;
	}
	for (size_t i = 0; i < listener_count_; ++i) {
		if (listeners_[i].listener == &listener) {
			Log::error(kTag, "refusing to add a time listener twice");
			return false;
		}
	}
	if (listener_count_ == kListenerCapacity) {
		Log::error(kTag, "refusing time listener: capacity of %u exhausted", static_cast<unsigned>(kListenerCapacity));
		return false;
	}
	listeners_[listener_count_++] = Registration{&listener, events};
	update_watch();
	return true;
}

bool TimeService::remove_listener(TimeListener& listener) {
	if (notifying_) {
		Log::error(kTag, "refusing to remove a time listener from within a notification");
		return false;
	}
	for (size_t i = 0; i < listener_count_; ++i) {
		if (listeners_[i].listener == &listener) {
			for (size_t j = i + 1; j < listener_count_; ++j) {
				listeners_[j - 1] = listeners_[j];
			}
			--listener_count_;
			update_watch();
			return true;
		}
	}
	return false;
}

void TimeService::refresh() {
	if (started_) {
		tick();
	}
}

bool TimeService::wants_continuous() const {
	for (size_t i = 0; i < listener_count_; ++i) {
		if ((listeners_[i].events & TimeEvents::kContinuous) != 0) {
			return true;
		}
	}
	return false;
}

void TimeService::update_watch() {
	if (!started_) {
		return;
	}
	bool needed = !synchronized_ || wants_continuous();
	if (needed && !watch_.valid()) {
		watch_ = loop_.schedule_periodic(clock_.monotonic_ms() + kWatchPeriodMs, kWatchPeriodMs,
			Callback::member<TimeService, &TimeService::tick>(this));
	} else if (!needed && watch_.valid()) {
		loop_.cancel(watch_);
		watch_ = TimerHandle();
	}
}

void TimeService::tick() {
	WallTime now = clock_.wall_time();
	int64_t offset_ms = now.seconds * 1000 - static_cast<int64_t>(clock_.monotonic_ms());
	uint8_t events = 0;
	if (now.synchronized && !synchronized_) {
		events |= TimeEvents::kSynchronized;
	} else if (!now.synchronized && synchronized_) {
		events |= TimeEvents::kSynchronizationLost;
		Log::warn(kTag, "wall clock lost its synchronization");
	} else if (now.synchronized) {
		int64_t step_ms = offset_ms - clock_offset_ms_;
		if (step_ms > kJumpThresholdS * 1000 || step_ms < -kJumpThresholdS * 1000) {
			events |= TimeEvents::kClockJumped;
			Log::info(kTag, "wall clock jumped by %lld s", static_cast<long long>(step_ms / 1000));
		}
	}
	clock_offset_ms_ = offset_ms;
	if (now.synchronized) {
		LocalTime local;
		if (zone_.to_local(now.seconds, local)) {
			Date date;
			date.year = static_cast<uint16_t>(local.year);
			date.month = local.month;
			date.day = local.day;
			int32_t utc_offset = utc_offset_seconds(local, now.seconds);
			if ((events & TimeEvents::kSynchronized) == 0) {
				if (!(date == last_date_)) {
					events |= TimeEvents::kDayChanged;
				}
				if (utc_offset != last_utc_offset_) {
					events |= TimeEvents::kOffsetChanged;
				}
			}
			last_date_ = date;
			last_utc_offset_ = utc_offset;
		}
	}
	synchronized_ = now.synchronized;
	update_watch();
	if (events != 0) {
		notify(events);
	}
}

void TimeService::notify(uint8_t events) {
	notifying_ = true;
	for (size_t i = 0; i < listener_count_; ++i) {
		uint8_t mine = events & listeners_[i].events;
		if (mine != 0) {
			listeners_[i].listener->time_changed(mine);
		}
	}
	notifying_ = false;
}

void TimeService::load_location() {
	char lat_text[Settings::kMaxStringLength + 1];
	char lon_text[Settings::kMaxStringLength + 1];
	SettingsStatus lat_status = settings_.get_string(setting::kLatitude, lat_text, sizeof(lat_text));
	SettingsStatus lon_status = settings_.get_string(setting::kLongitude, lon_text, sizeof(lon_text));
	has_location_ = false;
	if (lat_status == SettingsStatus::not_found && lon_status == SettingsStatus::not_found) {
		Log::info(kTag, "astronomy unavailable: no location set (settings %s, %s)", setting::kLatitude,
			setting::kLongitude);
		return;
	}
	if (lat_status != SettingsStatus::ok || lon_status != SettingsStatus::ok) {
		Log::error(kTag, "astronomy unavailable: %s %s, %s %s", setting::kLatitude, settings_status_name(lat_status),
			setting::kLongitude, settings_status_name(lon_status));
		return;
	}
	GeoLocation where;
	const char* error = nullptr;
	if (!parse_coordinate(lat_text, -90.0, 90.0, where.latitude, &error)) {
		Log::error(kTag, "astronomy unavailable: %s \"%s\": %s", setting::kLatitude, lat_text, error);
		return;
	}
	if (!parse_coordinate(lon_text, -180.0, 180.0, where.longitude, &error)) {
		Log::error(kTag, "astronomy unavailable: %s \"%s\": %s", setting::kLongitude, lon_text, error);
		return;
	}
	int32_t elevation = 0;
	SettingsStatus elev_status = settings_.get_int(setting::kElevation, elevation);
	if (elev_status != SettingsStatus::ok && elev_status != SettingsStatus::not_found) {
		Log::error(kTag, "ignoring %s: %s; using 0 m", setting::kElevation, settings_status_name(elev_status));
		elevation = 0;
	}
	where.elevation_m = elevation;
	if (!geo_location_valid(where, &error)) {
		Log::error(kTag, "ignoring %s %d: %s; using 0 m", setting::kElevation, static_cast<int>(elevation), error);
		where.elevation_m = 0;
	}
	location_ = where;
	has_location_ = true;
	Log::info(kTag, "location %s, %s, %d m", lat_text, lon_text, static_cast<int>(where.elevation_m));
}

bool TimeService::location(GeoLocation& out) const {
	if (has_location_) {
		out = location_;
	}
	return has_location_;
}

bool TimeService::set_location(const GeoLocation& where, const char** error) {
	const char* reason = nullptr;
	if (notifying_) {
		reason = "called from within a time notification";
	} else {
		geo_location_valid(where, &reason);
	}
	if (reason != nullptr) {
		Log::error(kTag, "refusing location: %s", reason);
		return refuse(error, reason);
	}
	char lat_text[24];
	char lon_text[24];
	std::snprintf(lat_text, sizeof(lat_text), "%.6f", where.latitude);
	std::snprintf(lon_text, sizeof(lon_text), "%.6f", where.longitude);
	int32_t elevation = static_cast<int32_t>(std::lround(where.elevation_m));
	SettingsStatus status = settings_.set_string(setting::kLatitude, lat_text);
	if (status == SettingsStatus::ok) {
		status = settings_.set_string(setting::kLongitude, lon_text);
	}
	if (status == SettingsStatus::ok) {
		status = settings_.set_int(setting::kElevation, elevation);
	}
	if (status != SettingsStatus::ok) {
		Log::error(kTag, "refusing location: cannot store it (%s)", settings_status_name(status));
		return refuse(error, settings_status_name(status));
	}
	GeoLocation stored;
	parse_coordinate(lat_text, -90.0, 90.0, stored.latitude);
	parse_coordinate(lon_text, -180.0, 180.0, stored.longitude);
	stored.elevation_m = elevation;
	location_ = stored;
	has_location_ = true;
	Log::info(kTag, "location set to %s, %s, %d m", lat_text, lon_text, static_cast<int>(elevation));
	notify(TimeEvents::kLocationChanged);
	return true;
}

void TimeService::clear_location() {
	if (notifying_) {
		Log::error(kTag, "refusing to clear the location from within a time notification");
		return;
	}
	settings_.erase(setting::kLatitude);
	settings_.erase(setting::kLongitude);
	settings_.erase(setting::kElevation);
	bool had = has_location_;
	has_location_ = false;
	Log::info(kTag, "location cleared: astronomy unavailable");
	if (had) {
		notify(TimeEvents::kLocationChanged);
	}
}

uint64_t TimeService::monotonic_ms() const {
	return clock_.monotonic_ms();
}

WallTime TimeService::now_utc() const {
	return clock_.wall_time();
}

bool TimeService::to_local(int64_t utc_seconds, LocalTime& out) const {
	return zone_.to_local(utc_seconds, out);
}

bool TimeService::sun_times(const Date& date, SunTimes& out) const {
	if (!has_location_) {
		return false;
	}
	if (sun_cache_count_ > 0 && (sun_cache_location_.latitude != location_.latitude ||
			sun_cache_location_.longitude != location_.longitude ||
			sun_cache_location_.elevation_m != location_.elevation_m)) {
		sun_cache_count_ = 0;
		sun_cache_next_ = 0;
	}
	for (size_t i = 0; i < sun_cache_count_; ++i) {
		if (sun_cache_[i].date == date) {
			out = sun_cache_[i].times;
			return true;
		}
	}
	out = hc::sun_times(date.year, date.month, date.day, location_);
	++sun_computations_;
	sun_cache_location_ = location_;
	sun_cache_[sun_cache_next_] = SunCacheEntry{date, out};
	sun_cache_next_ = (sun_cache_next_ + 1) % kSunCacheSize;
	if (sun_cache_count_ < kSunCacheSize) {
		++sun_cache_count_;
	}
	return true;
}

} // namespace hc
