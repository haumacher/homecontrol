# Schedules: the `schedules` member of the runtime image

*Specification of the schedule entries of issue #10 (phase 2). Implemented by the core's scheduler
(`core/include/hc/scheduler.h`, design in `doc/time.md`); E-Home exports them (its sibling issue of
#8) and its rule simulation fires them. The rule trigger that reacts to a firing is the `schedule`
trigger of `spec/rules.md` 2.3; this document follows the conventions of `spec/rules.md`
(camelCase members, the JSON literal values of its section 3.1, refusal reasons of its 10.1).*

## 1. Overview

A *schedule entry* names moments in local civil time: every workday at 06:30, every year on
24 December at 17:00, at sunset minus 30 minutes but not after 22:00, every 15 minutes from 08:00 to
18:00. At each such moment the controller *fires* the entry: every rule with a `schedule` trigger
naming it reacts (`spec/rules.md` 7.1), and the entry's optional *output* datapoint turns true for
the rest of the local day.

Decisions:

- **S1. Local civil time, with defined DST rules.** All times are wall times of the controller's
  time zone (the setting `tz`, later the image's). A time that does not exist on a spring-forward day
  (02:30 in the EU on the last Sunday of March) fires once when the gap ends (03:00); a time that
  occurs twice on a fall-back day fires once, at its first occurrence. *Alternatives:* skip the
  non-existent time (an alarm or a heating start silently lost once a year); fire both occurrences
  (a doubled pulse, a doubled count).
- **S2. Missed firings are detected, never silently lost.** A firing that fell into a gap (the
  controller was off, the clock was stepped over it) is logged; the entry's `missed` policy decides
  whether it is also fired once (`catchUp`) or dropped (`skip`, the default: most automations, a
  light at sunset or a blind in the morning, must not act hours late). A firing more than 60 s late
  counts as missed.
- **S3. One shape per kind, no cron strings.** Six kinds cover what a house needs; each has
  explicit members that E-Home can show as a form and the device can validate. *Alternative:* a cron
  expression (unreadable for inhabitants, no astronomy, no DST semantics of its own).
- **S4. The enable switch is the only adjustable part for now.** Like a rule's (`spec/rules.md`
  8.3), it defaults to the image's `enabled` and may be switched on the device unless the image
  fixes it. Adjustable times are a later extension (section 7).

## 2. The entry

The image carries `"schedules": [ ... ]`; the order is the *image order* (entries due at the same
second fire in this order). Members not defined here are refused, except `description` and
`comment`, which every object may carry and the device ignores.

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | schedule id: 1..32 characters of `A-Z a-z 0-9 _ -` | yes | Stable identity assigned by E-Home; the key of the persisted state and of `schedule` triggers. |
| `label` | string, 1..63 printable bytes | yes | Shown in the UI and the diagnostics. |
| `kind` | `weekly`, `yearly`, `dates`, `astro`, `interval`, `once` | yes | Section 3. |
| `enabled` | boolean | no, `true` | Default of the enable switch. |
| `enabledAdjustable` | boolean | no, `true` | Whether the inhabitant may switch the entry on and off. |
| `missed` | `skip` or `catchUp` | no, `skip` | What happens to a missed firing (section 4.3). |
| `output` | datapoint name | no | A `boolean` datapoint of the image that the scheduler owns: true from a firing until local midnight (section 5). |
| *kind members* | | | Section 3. |

## 3. Kinds

Value forms (those of `spec/rules.md` 3.1): a *time* is `{"hour": 0..23, "minute": 0..59,
"second": 0..59}` (all three; no `dayOfWeek`); a *date* is `{"year": 1970..2155, "month", "day"}`;
a *duration* is integer milliseconds; *days* is an array of 1 (Monday) .. 7 (Sunday), without
duplicates, at least one element, the numbering of the rules' `weekday()`.

| `kind` | Members | Fires |
|---|---|---|
| `weekly` | `time`; `days` (default all seven) | On each listed day at `time`. |
| `yearly` | `month` 1..12, `day` 1..31 (a day of that month; 29 February allowed); `time` | Every year on that date at `time`; 29 February only in leap years. |
| `dates` | `dates`: array of 1..128 dates, any order, no duplicates; `time` | On each listed date at `time` (holidays, vacations, collection days). |
| `astro` | `sun`: `dawn`, `sunrise`, `sunset` or `dusk`; `offset` (duration, a whole number of seconds, -12 h..12 h, default 0); `notBefore`, `notAfter` (times, optional; `notBefore <= notAfter` when both); `days` (default all seven) | On each listed day at the sun event plus the offset, moved up to `notBefore` if earlier and down to `notAfter` if later. |
| `interval` | `every` (duration, a whole number of minutes, 1 min..12 h); `from`, `until` (times, `from <= until`); `days` (default all seven) | On each listed day at `from`, `from + every`, ... up to the last such slot not after `until`. |
| `once` | `at`: `{"year", "month", "day", "hour", "minute", "second"}` | Once, at that local date and time; never again. |

Notes:

- `dawn` and `dusk` are the begin and end of civil twilight (sun 6° below the horizon), `sunrise`
  and `sunset` the upper limb on the horizon. They are computed for the device's location (settings
  `location.lat`, `location.lon`, `location.elev`; `doc/time.md`). The events of a date are those
  around its solar noon. A day on which the event does not occur (polar day or night; a summer
  night too bright for civil dusk) has no firing, whatever the clamp says.
- Without a location an `astro` entry does not fire: the device logs the refusal (once) and the
  other entries run. It starts firing when a location is set; the time without one is not a gap
  (section 4.3).
- An `interval` window across midnight is two entries (refused with `malformed`, the reason says
  so). On a spring-forward day the slots inside the gap collapse into one firing when the gap ends;
  on a fall-back day the repeated hour's slots fire in its first pass only.

## 4. Semantics

### 4.1 When entries fire

The device fires an entry when the synchronized wall clock reaches one of its moments (section 3,
with the DST rules of S1). While the wall clock is not synchronized nothing fires (the time
service's flag, `doc/time.md`); the first synchronization is not a gap: an entry new to the device
(no persisted state) counts from the moment it is first evaluated. A firing is delivered to the
rules as an external event (`spec/rules.md` 7.1) within about a second of its moment.

### 4.2 Enable switch

A disabled entry does not fire and has no next firing. Firings that passed while it was disabled
are not missed. The switch is persisted on the device by schedule id as an *override* of the
image's `enabled`; switching back to the image's value removes the override. A new image keeps the
override while the entry still exists and is adjustable; an entry that becomes
`enabledAdjustable: false` loses it (logged).

### 4.3 Missed firings

The device remembers, per entry and across reboots, the latest moment it has handled (fired or
skipped). When it evaluates the entry and finds moments after that one that are more than 60 s in
the past (after a reboot, after the clock was stepped forward):

- it logs them (`schedule <id> missed its firing of <time>`, or `missed its firings from <first> to
  <last>`);
- with `catchUp` it fires the entry **once**, now, however many moments were missed; with `skip`
  it fires nothing.

A moment that is due and at most 60 s late fires normally (moments before it in the same gap are
logged as missed and not caught up separately). A clock stepped backwards never repeats a firing:
moments up to the latest handled one do not fire again.

### 4.4 Image swap

Entries whose id is in the new image keep their persisted state (latest handled moment, last
firing, enable override per 4.2). The state of ids no longer present is deleted. Entries are
re-evaluated at once; missed firings are handled per 4.3.

## 5. Output datapoint

With `output`, the scheduler becomes the owning technology of that datapoint (`doc/datapoints.md`):
it reports `true` at each firing and `false` at the next local midnight, and after a load or reboot
it reports whether the entry fired today. Commands to the datapoint are refused (E-Home should
declare it not writable). The datapoint must be `boolean`, exist in the image and not be owned by
another technology or named by another entry.

## 6. Loading: checks and limits

The device refuses the whole image at the first failure, naming the entry and one of these reasons
(those of `spec/rules.md` 10.1, plus one):

| Reason | Examples |
|---|---|
| `malformed` | missing or unknown member, a value out of its range (`hour` 24, `month` 2 with `day` 30, an empty `days`, `until` before `from`, `notBefore` after `notAfter`, an offset beyond 12 h), an invalid id or label |
| `duplicateId` | two entries with the same id; a date listed twice in `dates` |
| `unknownReference` | `output` names no datapoint of the image |
| `typeError` | `output` names a datapoint that is not `boolean` |
| `outputConflict` | `output` names a datapoint owned by a technology (a KNX group object) or named by another entry |
| `limitExceeded` | more than 128 entries, more than 128 dates in one entry |

## 7. Not yet specified

- Adjustable times (the inhabitant moves the 06:30 of a weekly entry) through the adjustable
  parameter mechanism of `spec/rules.md` 8; until then a rule parameter compared with `time()` in a
  condition serves.
- Exceptions ("weekly, but not on the dates of entry `holidays`") and vacation ranges; today a rule
  condition combines two entries' outputs.
- One-shot timers *started by rules* are the rules' own timers (`spec/rules.md` 2.5), not schedule
  entries.

## 8. Example

```json
"schedules": [
  {"id": "workdayMorning", "label": "Workday morning", "kind": "weekly",
   "days": [1, 2, 3, 4, 5], "time": {"hour": 6, "minute": 30, "second": 0}, "missed": "catchUp",
   "output": "schedule.morning"},
  {"id": "christmasEve", "label": "Christmas Eve", "kind": "yearly",
   "month": 12, "day": 24, "time": {"hour": 17, "minute": 0, "second": 0}},
  {"id": "trash", "label": "Bin collection", "kind": "dates",
   "dates": [{"year": 2027, "month": 1, "day": 8}, {"year": 2027, "month": 1, "day": 22}],
   "time": {"hour": 6, "minute": 15, "second": 0}},
  {"id": "blindsEvening", "label": "Blinds down at dusk", "kind": "astro",
   "sun": "sunset", "offset": -1800000,
   "notBefore": {"hour": 17, "minute": 0, "second": 0}, "notAfter": {"hour": 22, "minute": 0, "second": 0}},
  {"id": "circulation", "label": "Hot water circulation", "kind": "interval",
   "days": [1, 2, 3, 4, 5, 6, 7], "every": 900000,
   "from": {"hour": 6, "minute": 0, "second": 0}, "until": {"hour": 22, "minute": 0, "second": 0},
   "enabledAdjustable": false},
  {"id": "partyEnd", "label": "Party ends", "kind": "once",
   "at": {"year": 2026, "month": 12, "day": 31, "hour": 23, "minute": 59, "second": 30}}
]
```
