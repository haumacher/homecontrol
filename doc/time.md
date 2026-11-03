# Time: the time service, astronomy and the scheduler

The core's answer to "what time is it, and when does the sun set" (issue #9) and the scheduler that
fires the schedule entries of the runtime image (issue #10, format in `spec/schedules.md`). All of
it is portable core code, tested on the host under fake clocks.

| Piece | Where |
|---|---|
| `TimeSource` (the interface the rules and the scheduler read), `TimeService`, time events, location settings | `core/include/hc/time_service.h` |
| Sunrise, sunset, civil dawn and dusk (NOAA) | `core/include/hc/astro.h` |
| The scheduler | `core/include/hc/scheduler.h` |
| SNTP, the `tz` setting, the platform clock and time zone | `doc/network.md`, `core/include/hc/platform/` |

## The time service

`hc::TimeService` is constructed and started by the `Controller` (`Controller::time()`). It reads
the platform `Clock` (monotonic milliseconds; wall seconds with the *synchronized* flag that SNTP
sets) and converts through the platform `TimeZone`, which applies the POSIX TZ string of the `tz`
setting (validated by `hc::posix_tz_valid()`).

### `TimeSource`

What consumers call; the service implements it and the tests fake it:

| Function | Answer |
|---|---|
| `monotonic_ms()` | the event loop's time base |
| `now_utc()` | wall seconds since the epoch and `synchronized` |
| `to_local(utc, LocalTime&)` | local civil time (fields, DST flag, zone abbreviation) |
| `sun_times(date, SunTimes&)` | dawn, sunrise, sunset, dusk of a local date as UTC instants; false without a location; an event that does not occur is absent. The TimeService keeps the last 12 dates' results for its location (one computation costs about 5 ms on the C6, which has no FPU, and a schedule's next occurrence may look at ten dates, #50) |
| `sun_window(date, SunWindow&)` | the span within which every sun event of the date lies, without computing them (`hc::sun_window()`, below); false exactly when `sun_times()` is |
| `synchronized()`, `local_time()`, `local_date()`, `utc_offset()` | helpers on top of the above; `local_time()` is false while not synchronized |
| `local_to_utc(date, h, m, s)` | the UTC instant of a local time, with the DST rules of `spec/schedules.md` S1: a repeated time resolves to its first occurrence, a non-existent one to the end of the gap |
| `sun_event_local(date, event, LocalTime&)` | what the rules' `sunrise()`, `sunset()`, `dawn()`, `dusk()` return (`spec/rules.md` 4.7) |

The rule interpreter (issue #12) fixes its cascade time by reading `now_utc()` once and converting
it with `to_local()`; its time functions have no value while `synchronized` is false.

### Time events

`TimeListener::time_changed(events)` receives bits of `TimeEvents`, several per call:

| Bit | When |
|---|---|
| `kSynchronized` | the clock became synchronized (the first SNTP fix; subsumes the step it made). At `start()` if it already is. |
| `kSynchronizationLost` | the flag went away (logged as a warning) |
| `kClockJumped` | the synchronized wall clock stepped more than 60 s against the monotonic clock (logged with the step) |
| `kDayChanged` | the local date changed: midnight, also on the DST days (the date is compared, not elapsed time), or a jump or zone change across midnight |
| `kOffsetChanged` | the UTC offset changed: a DST transition, a new time zone (`Controller::set_time_zone()` calls `refresh()`, so this arrives at once) |
| `kLocationChanged` | `set_location()` or `clear_location()` |

A listener registers with a mask (`add_listener(listener, events)`, at most 4 listeners). The watch
is a periodic 1 s timer: it runs while the clock is not synchronized and afterwards only while some
listener asks for one of `kSynchronizationLost | kClockJumped | kDayChanged | kOffsetChanged`. The
controller subscribes to `kSynchronized` alone and logs `wall clock synchronized: <local time>` once,
as it always did; with only that subscription a synchronized controller has no time work on its
loop. The scheduler subscribes to everything. Events are detected within a second; listeners run
synchronously on the loop and must not (un)register from within the call.

### Location settings

| Key | Type | Default | Meaning |
|---|---|---|---|
| `location.lat` | string, decimal degrees (`48.1374`, up to 8 decimals), -90..90, north positive | none | latitude |
| `location.lon` | string, decimal degrees, -180..180, east positive | none | longitude |
| `location.elev` | integer, metres, -500..9000 | 0 | height above the surrounding horizon; lowers sunrise, raises sunset |

Both coordinates absent: astronomy is unavailable (logged at start as info): the rules' sun
functions have no value, `astro` schedule entries are refused with a log line (once) and the others
run. One coordinate missing or invalid is logged as an error and treated the same. An invalid
elevation is logged and replaced by 0. `TimeService::set_location()` validates and stores the three
keys (`%.6f`), `clear_location()` erases them; both notify. Nothing on the device calls them yet:
the runtime image (issue #8) or a console command will. Strings for the coordinates because the
settings have no floating-point type (doc/network.md). Persisted names: never rename.

## Astronomy

`hc::sun_times(year, month, day, location)` implements the NOAA solar calculator (Meeus'
low-precision solar coordinates: mean anomaly, equation of centre, apparent longitude with
nutation, obliquity, equation of time, declination). For each event it starts from the solar noon
of the date at the place's longitude and refines three times with the sun's position at the
previous estimate. Zenith angles: 90.833° for sunrise and sunset (refraction and the sun's
semi-diameter) plus the horizon dip `2.076° · √elevation / 60`, 96° for civil dawn and dusk. When
the hour angle does not exist (|cos| > 1) the event is absent: polar day or night, and the white
nights in which the sun never sinks 6° (Helsinki in June still has a few minutes of civil dusk,
Tromsø has none).

`hc::sun_window(year, month, day, location)` bounds the events of a date without trigonometry: an
event is the date's solar noon (12:00 UTC minus 4 minutes per degree east, minus the equation of
time) moved by an hour angle of 0..180°, and the NOAA equation of time is at most 18.5 minutes in
magnitude (the sum of its terms' amplitudes; 16.5 in practice). The window is therefore the mean
solar noon ±(12 h + 20 min), whatever the latitude, elevation or season (`astro_test.cpp` checks
every event of two years over a grid of places from pole to pole against it). The scheduler uses it
to skip dates (below).

The events of a date are those around its solar noon; they are UTC instants and may lie on the
neighbouring UTC date (Sydney's sunrise is on the previous UTC day), and civil dusk at high
latitude in summer can lie after the following local midnight.

**Accuracy, measured** (`core/test/astro_test.cpp`) against the US Naval Observatory's published
times (aa.usno.navy.mil, rounded by USNO to the minute) for 14 place-dates, 52 events: Greenwich on
both equinoxes, Munich and Helsinki on both solstices, Sydney on both solstices, Buenos Aires,
Quito, New York, Berlin on the spring-forward day, and Tromsø in polar day (no events) and polar
night (civil twilight only): the largest difference is **33 s**, i.e. within the rounding of the
reference; the test requires at most 60 s. Near a polar boundary (an event that barely occurs) the
error grows, as for any rise/set computation; the event's existence near the boundary may differ
from USNO by a day.

## The scheduler

`hc::Scheduler` (a `TimeListener`, and a `Technology` for its output datapoints) holds the entries
of the image. `load(specs, count)` validates everything (a refusal keeps the previous entries and
logs entry and reason), allocates two blocks (the entry table; the strings and the sorted dates), and
evaluates. The image loader (issue #8) parses the JSON of `spec/schedules.md` into `ScheduleSpec`s
and calls it; it also registers the scheduler with the time service and its listeners with the
scheduler. Until then nothing constructs a scheduler outside the tests.

### Interfaces for the rules

- `ScheduleListener::schedule_fired(const char* id)`: called once per firing, on the loop, from the
  scheduler's timer (never from within `load()`), in the order of the firings' moments, image order
  on ties. The rule engine turns it into the external event of `spec/rules.md` 7.1. A listener must
  not load or (un)register from within the call; it may switch entries (`set_enabled`), which takes
  effect for firings not yet delivered.
- `Scheduler::find(id)` for the rule loader's `unknownReference` check.
- For the UI: `next_firing(i)` (as armed), `next_after(i, utc)` (a preview), `last_fired(i)`,
  `enabled(i)`, `set_enabled(id, on)` (refused when the image fixes the switch).

### Timers

One event-loop timer for all entries. On every evaluation each enabled entry's next moment is
computed; the timer is set for the earliest, at most 10 min ahead (`kMaxTimerMs`): the scheduler
re-reads the wall clock at least that often, so a drift between the wall clock and the monotonic
base cannot delay a firing by more than that period's drift. The timer's deadline is computed from
whole wall seconds, so a firing is up to a second late, never early; a timer that fires early
(the wall clock slewed back) just re-arms. Evaluations happen on load, on every time event
(`kSynchronized`, `kClockJumped`, `kDayChanged`, `kOffsetChanged`, `kLocationChanged`,
`kSynchronizationLost` which stops the timer), on `set_enabled` and after every firing round. While
the clock is not synchronized nothing is armed (logged once: `schedules wait for a synchronized
wall clock`).

### Next-firing computation

Per kind, from a UTC instant `t` and the local date `L` of `t`:

- `weekly`, `interval`: the days `L-1 .. L+8` that are in `days`; the first moment after `t`. An
  interval day converts only the slots from two hours before the local time of `t` (more than any
  offset change), so the walk is short even for a one-minute interval.
- `astro`: the same days, the *earliest* moment after `t` (an offset or clamp may move a day's
  firing past a neighbour's); a day without the event has none. If no day of the window has one
  (polar night), the entry has no next firing until a day change re-evaluates it. The sun is
  computed only for the days that can hold the answer (#57): a day's firing lies within its sun
  window moved by the offset and clamped like the firing, so a day whose latest possible firing is
  not after `t` is skipped, and the walk stops at the first day whose earliest possible firing is
  not before the best found (that bound grows with the day: the window moves by a day, the clamp's
  local times by a day give or take a DST hour, and the clamp is non-decreasing in both). The result
  is the same as the walk over every day (`scheduler_astro_test.cpp` compares the two over a year
  at places from Sydney to McMurdo, with the ±12 h offsets); the first evaluation after the clock
  synchronizes computes two dates instead of ten (about 11 ms instead of 55 ms on the C6), and
  polar nights still walk the whole window, once per day thanks to the TimeService's cache.
- `yearly`: the years `L.year-1 .. L.year+8` (29 February: the next leap year within eight years).
- `dates`: the sorted list from `L-1`.
- `once`: its moment if after `t`.

Local times become UTC through `TimeSource::local_to_utc()` (DST rules S1). On a spring-forward day
02:30 fires at 03:00 once; on a fall-back day 02:30 fires once, at its first occurrence, because
the next moment is always searched strictly after the latest handled one.

### Missed firings and the catch-up store

Each entry keeps its *accounted* moment: the latest moment it fired or skipped. An evaluation that
finds the first moment after it already in the past arms the timer at once; the timer then walks
to the latest past moment and decides: at most 60 s late (`kGraceS`) is on time and fires; older is
missed: logged with the range, and with `catchUp` fired once now, with `skip` only accounted. The
next moment is searched after `max(now, accounted)`, so a clock stepped backwards never repeats a
firing. A new entry starts accounted at the first evaluation with a synchronized clock.

An entry that does not run (switched off, or an `astro` entry without a location) is *idle*: its
accounted moment follows the clock, so the time it was idle is never a gap, also across a reboot
or an image swap (below).

The accounted moment, the last firing, the enable override and the idle mark survive reboots in
the settings, one key per entry: `sch.` + the 8 lower-case hex digits of the 32-bit FNV-1a hash of
the id (12 characters, within the 15 of NVS). The value is a string
`1;<lastFired>;<accounted>;<flags>;<id>`: format version 1; UTC seconds or empty; flags are the
enable override (`0` off, `1` on, absent: the image's default) followed by `i` when the entry was
idle when the record was written; e.g. `1;1792209600;1792209600;;workdayMorning` or `1;;;0i;wake`.
An idle record means the entry stayed idle until the controller stopped (switching it on or the
location arriving rewrites the record), so it restarts from now. The id in the value detects a
foreign record (a hash collision with a removed entry, garbage): it is logged and ignored. Two ids
of one image with the same hash: the second entry is not persisted (logged at load). On each load
the records whose key belongs to no entry are deleted. The record is written at each firing, at
each skip, when an entry is first evaluated, when it becomes idle or runs again, and when the
switch changes. Wear: an entry firing every minute writes about 1440 records of ~60 bytes a day;
NVS spreads them over its pages, which is acceptable for the default partition but a reason to
prefer coarser intervals.

### Output datapoints

An entry with `output` binds that boolean datapoint to the scheduler (`DatapointModel::bind`). It
reports `true` at each firing and, at every evaluation (which includes each local midnight),
whether the last firing lies on the current local date, so the value turns `false` at midnight and
is right again after a reboot. Commands are refused with a log line. The datapoint model has no
unbind: a scheduler must not be destroyed while a model it bound to lives on, unless the model is
reloaded (the image loader replaces both together); an output dropped by a new image stays bound
to the scheduler (its commands remain refused).

### Memory

The scheduler object is about 80 bytes; per entry about 150 bytes (the copied spec, the record key,
four times) plus the id and label and 4 bytes per listed date. 128 entries: about 20 KB at most.
Nothing is allocated after `load()`.

## What the ESP side still has to do

- **KNX time master** (issue #9: DPT 10/11/19 on configured group addresses, periodically and on
  read requests): implemented in the core as `hc::knx::TimeMaster` (doc/knx.md, "Time master"). It
  listens for the time events (synchronized, lost, jumped, day and offset changes), sends nothing
  while the clock is not synchronized and publishes through datapoints the KNX adapter transmits.
  Still to do: the image loader (#8) feeds its configuration and wires it to the controller's time
  service and the adapter on the device.
- **Battery-backed RTC** (DS3231 over I2C, issue #9's option): not in this package; it would be a
  platform `Clock` source that reports synchronized after a power cut without network.
- **Wiring**: the image loader (#8) constructs the scheduler with the controller's loop, time
  service, settings and datapoint model, registers it (`time().add_listener(scheduler)`) and
  connects the rule engine (#12) as its `ScheduleListener`. A console command to set the location
  (`location set <lat> <lon> [elev]`) is a small follow-up in `hc_console`.
