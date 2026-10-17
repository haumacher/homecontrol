/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_TIME_SERVICE_H
#define HC_TIME_SERVICE_H

#include <cstddef>
#include <cstdint>

#include "hc/astro.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/platform/clock.h"
#include "hc/platform/settings.h"
#include "hc/platform/time_zone.h"

namespace hc {

/// The setting keys of the time service (doc/time.md). Persisted names: never rename.
namespace setting {

/// Latitude in decimal degrees, a string ("48.1374", north positive, -90..90). Absent: no astronomy.
constexpr const char* kLatitude = "location.lat";
/// Longitude in decimal degrees, a string ("11.5755", east positive, -180..180). Absent: no astronomy.
constexpr const char* kLongitude = "location.lon";
/// Elevation above the surrounding horizon in metres, an integer (-500..9000); absent: 0.
constexpr const char* kElevation = "location.elev";

} // namespace setting

/// Parses a coordinate setting: an optional sign, 1..3 integer digits and at most 8 decimals
/// ("48.1374", "-0.5", "+151"), within `min`..`max`. On refusal `error` names the problem.
bool parse_coordinate(const char* text, double min, double max, double& out, const char** error = nullptr);

/// Days since 1970-01-01 to the proleptic Gregorian date (the inverse of hc::days_from_civil()).
Date date_from_days(int64_t days);

/// Day of the week of a day number: 1 Monday .. 7 Sunday (the numbering of spec/rules.md).
unsigned weekday_from_days(int64_t days);

/// The days of `month` in `year` (28..31); 0 for a month outside 1..12.
unsigned days_in_month(int32_t year, unsigned month);

/// What the rule engine and the scheduler ask about time: "what time is it" (monotonic, UTC and
/// local, with the synchronized flag) and "when does the sun rise". The TimeService implements it
/// from the platform; the tests implement it with a scripted fake.
///
/// The non-virtual helpers are defined on top of the virtual functions, so a fake gets them for
/// free and behaves exactly like the service.
///
/// Threading: called on the event loop's thread.
class TimeSource {
public:
	/// Milliseconds on the monotonic base of the event loop (Clock::monotonic_ms).
	virtual uint64_t monotonic_ms() const = 0;

	/// The wall clock: seconds since the epoch (UTC) and whether they come from a trusted source.
	/// An unsynchronized value must not drive calendars (no value for the rules' time functions, no
	/// schedule firings).
	virtual WallTime now_utc() const = 0;

	/// Converts a UTC instant into local civil time (time zone and DST as applied by the platform).
	/// False if the platform cannot.
	virtual bool to_local(int64_t utc_seconds, LocalTime& out) const = 0;

	/// The sun events of the local calendar date `date` (hc::sun_times() at the configured place).
	/// False when astronomy is unavailable: no location is configured. An event that does not occur
	/// that day is absent in `out`.
	virtual bool sun_times(const Date& date, SunTimes& out) const = 0;

	/// now_utc().synchronized.
	bool synchronized() const { return now_utc().synchronized; }

	/// The current local time; false (and `out` unspecified) while the wall clock is not
	/// synchronized or the conversion fails.
	bool local_time(LocalTime& out) const;

	/// The local date of a UTC instant; false if the conversion fails.
	bool local_date(int64_t utc_seconds, Date& out) const;

	/// The offset of local time from UTC at an instant, in seconds (local - UTC); 0 if the
	/// conversion fails.
	int32_t utc_offset(int64_t utc_seconds) const;

	/// The UTC instant of the local civil time `date hour:minute:second`, by the scheduler's DST
	/// rules: a time that occurs twice (the hour after a fall-back) resolves to its first
	/// occurrence; a time that does not exist (inside a spring-forward gap) resolves to the instant
	/// the gap ends (02:30 on the EU spring-forward day is 03:00 CEST). Assumes at most one UTC
	/// offset change within a day either side, which holds for every real zone.
	int64_t local_to_utc(const Date& date, unsigned hour, unsigned minute, unsigned second) const;

	/// The local time of a sun event of `date`; false without astronomy or when the event does not
	/// occur that day. What the rules' sunrise(), sunset(), dawn() and dusk() return.
	bool sun_event_local(const Date& date, SunEvent event, LocalTime& out) const;

protected:
	~TimeSource() = default;
};

/// Bits of a time event (TimeListener::time_changed); several may come in one call.
struct TimeEvents {
	/// The wall clock became synchronized (the first SNTP fix; subsumes the step it made).
	static constexpr uint8_t kSynchronized = 0x01;
	/// The wall clock lost its synchronized flag.
	static constexpr uint8_t kSynchronizationLost = 0x02;
	/// The local calendar date changed (midnight, or a jump or zone change across it).
	static constexpr uint8_t kDayChanged = 0x04;
	/// The synchronized wall clock stepped by more than TimeService::kJumpThresholdS against the
	/// monotonic clock (an SNTP correction, a manual set).
	static constexpr uint8_t kClockJumped = 0x08;
	/// The UTC offset of local time changed (a DST transition, a new time zone).
	static constexpr uint8_t kOffsetChanged = 0x10;
	/// The location for astronomy was set or cleared.
	static constexpr uint8_t kLocationChanged = 0x20;

	/// The events that need the service to watch the clock after it is synchronized.
	static constexpr uint8_t kContinuous = kSynchronizationLost | kDayChanged | kClockJumped | kOffsetChanged;
	static constexpr uint8_t kAll = 0x3F;
};

/// Receives time events. Called on the event loop's thread, synchronously from the service's clock
/// watch (or from start(), set_location(), refresh()), with the bits that happened. A listener may
/// read the service and schedule work but must not add or remove listeners from within the call
/// (refused and logged).
class TimeListener {
public:
	virtual void time_changed(uint8_t events) = 0;

protected:
	~TimeListener() = default;
};

/// The one place that answers "what time is it" for the core: the platform clock with its
/// synchronized flag, local civil time through the platform's time zone, and astronomy at the
/// configured location (settings location.lat, location.lon, location.elev). It watches the
/// clock on the event loop and tells its listeners when the clock became synchronized, lost
/// synchronization, jumped, crossed local midnight or changed its UTC offset (doc/time.md).
///
/// The watch is a periodic timer of kWatchPeriodMs. It runs while the clock is not synchronized
/// and, after that, only while a listener subscribes to one of TimeEvents::kContinuous: without
/// such a listener a synchronized controller has no time work on its loop.
class TimeService : public TimeSource {
public:
	/// Period of the clock watch.
	static constexpr uint32_t kWatchPeriodMs = 1000;

	/// A step of the wall clock against the monotonic clock beyond this is a jump.
	static constexpr int64_t kJumpThresholdS = 60;

	/// Listeners registered at the same time.
	static constexpr size_t kListenerCapacity = 4;

	TimeService(EventLoop& loop, const Clock& clock, const TimeZone& zone, Settings& settings);
	~TimeService();

	TimeService(const TimeService&) = delete;
	TimeService& operator=(const TimeService&) = delete;

	/// Reads the location settings (logging what it found), takes the clock's state and starts the
	/// watch if needed. If the clock is already synchronized, the listeners subscribed to
	/// kSynchronized are told so before start() returns. A second call is refused (logged).
	void start();

	bool started() const { return started_; }

	/// Registers a listener for the `events` bits (TimeEvents). Refused (false, logged) when full,
	/// for a duplicate, and from within a notification.
	bool add_listener(TimeListener& listener, uint8_t events = TimeEvents::kAll);

	/// Unregisters a listener; false if it was not registered or from within a notification.
	bool remove_listener(TimeListener& listener);

	/// Runs the clock watch now (after the time zone was applied, for example), notifying what
	/// changed. Does nothing before start().
	void refresh();

	/// The configured location; false when astronomy is unavailable.
	bool location(GeoLocation& out) const;

	/// Validates, stores (settings location.lat, location.lon, location.elev) and applies a
	/// location; tells the listeners (kLocationChanged). On refusal returns false, logs and points
	/// `error` (if given) to the reason.
	bool set_location(const GeoLocation& where, const char** error = nullptr);

	/// Erases the location settings: astronomy becomes unavailable (listeners are told).
	void clear_location();

	// TimeSource
	uint64_t monotonic_ms() const override;
	WallTime now_utc() const override;
	bool to_local(int64_t utc_seconds, LocalTime& out) const override;
	bool sun_times(const Date& date, SunTimes& out) const override;

private:
	struct Registration {
		TimeListener* listener;
		uint8_t events;
	};

	void tick();
	void notify(uint8_t events);
	void update_watch();
	bool wants_continuous() const;
	void load_location();

	EventLoop& loop_;
	const Clock& clock_;
	const TimeZone& zone_;
	Settings& settings_;
	Registration listeners_[kListenerCapacity] = {};
	size_t listener_count_ = 0;
	TimerHandle watch_;
	GeoLocation location_;
	bool has_location_ = false;
	bool started_ = false;
	bool notifying_ = false;
	bool synchronized_ = false;
	int64_t clock_offset_ms_ = 0; ///< wall - monotonic at the last tick (jump detection).
	Date last_date_;
	int32_t last_utc_offset_ = 0;
};

} // namespace hc

#endif // HC_TIME_SERVICE_H
