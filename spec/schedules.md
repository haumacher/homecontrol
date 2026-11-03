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
- **S4. The image declares what the inhabitant may adjust, within bounds; the device keeps the
  inhabitant's values as overrides.** The enable switch, like a rule's (`spec/rules.md` 8.3),
  defaults to the image's `enabled` and may be switched on the device unless the image fixes it.
  Besides, an entry may declare members of its kind adjustable (`adjustable`, with optional
  `bounds` and a step, section 2.1, issue #58): the opening time of the blinds on workdays, the
  days of an alarm, the offset of a dusk entry. The inhabitant's values are overrides persisted by
  schedule id and member, kept by a new image while they still fit it (4.2), like the rule
  parameters (`spec/rules.md` D5). An adjustable `time` is also a datapoint
  `schedule/<id>/time` (2.2), so a control page shows it and rules read it. *Alternatives:* a rule
  parameter compared with `time()` in a condition of an entry firing every minute (no DST rules of
  S1, no missed firings of S2, the moment invisible in the schedule view); every member adjustable
  (E-Home could not keep an installation's timing, and the UI could not know what makes sense).

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
| `adjustable` | array of member names | no, `[]` | The members of the kind the inhabitant may change (section 2.1). |
| `bounds` | object | no | Bounds and steps of adjustable members (section 2.1). |
| *kind members* | | | Section 3. |

### 2.1 Adjustable members

`adjustable` lists member names of the entry's kind, each at most once:

| `kind` | May be adjustable |
|---|---|
| `weekly` | `time`, `days` |
| `yearly` | `time` (the date stays fixed) |
| `dates` | `time` (the dates stay fixed) |
| `astro` | `offset`, `notBefore`, `notAfter`, `days` |
| `interval` | `every`, `from`, `until`, `days` |
| `once` | `at` |

A member not listed stays as the image gives it; another name is refused (`malformed`). An
adjustable `notBefore` or `notAfter` must be set in the image (the inhabitant moves a clamp, they do
not add one). `sun`, `month`, `day`, `dates`, `missed`, `output` and the label are never adjustable.

`bounds` maps an adjustable member (not `days`) to `{"min", "max", "step"}`, every member of it
optional, at most one bound object per member:

| Member | `min`, `max` (inclusive) | `step` | Absent `min`/`max` |
|---|---|---|---|
| `time`, `notBefore`, `notAfter`, `from`, `until` | time `{"hour", "minute", "second"}` | integer seconds, 1..86400 | 00:00:00, 23:59:59 |
| `offset` | duration (ms, whole seconds), -12 h..12 h | duration (ms, whole seconds), positive | -12 h, 12 h |
| `every` | duration (ms, whole minutes), 1 min..12 h | duration (ms, whole minutes), positive | 1 min, 12 h |
| `at` | `{"year", "month", "day", "hour", "minute", "second"}` | none (refused) | 1970-01-01 00:00:00, 2155-12-31 23:59:59 |

A value is *within the bounds* when `min <= value <= max` (times as seconds since midnight, `at` as
its local date and time) and, with a `step`, it is a whole multiple of the step: times count from
00:00:00 (a step of 300 allows 06:00, 06:05, ...), durations from 0 (a step of 300000 allows
-10 min, -5 min, 0, 5 min, ...). An absent step allows any value of the member's form. The image's
own value must be within its bounds (refused `malformed` otherwise), and `min` may not exceed `max`.
`days` takes no bounds: any non-empty set of days is allowed.

```json
{"id": "workdayMorning", "label": "Workday morning", "kind": "weekly", "days": [1, 2, 3, 4, 5],
 "time": {"hour": 6, "minute": 30, "second": 0}, "adjustable": ["time", "days"],
 "bounds": {"time": {"min": {"hour": 5, "minute": 0, "second": 0}, "max": {"hour": 9, "minute": 0, "second": 0},
                     "step": 300}}}
```

```json
{"id": "blindsEvening", "label": "Blinds down at dusk", "kind": "astro", "sun": "sunset", "offset": -1800000,
 "notAfter": {"hour": 22, "minute": 0, "second": 0}, "adjustable": ["offset", "notAfter"],
 "bounds": {"offset": {"min": -3600000, "max": 3600000, "step": 300000},
            "notAfter": {"min": {"hour": 20, "minute": 0, "second": 0}, "step": 1800}}}
```

### 2.2 The time datapoint

An entry of kind `weekly`, `yearly` or `dates` whose `time` is adjustable is also the datapoint
`schedule/<id>/time` of type `timeOfDay` (no day of the week), which the device derives when it
loads the image (`spec/runtime-image.md` 3.3; the prefix `schedule/` is reserved). The scheduler
owns it (technology `scheduler`):

1. **Value.** It carries the entry's effective time, quality good: from the load of the schedules
   (taking it then is no event for the rules, like the rule parameters' datapoints of
   `spec/rules.md` 8.6), and after every change (UI or API, a reset, a new image) as an update.
2. **Commands.** It is writable: a command (the UI, the API, MQTT, a rule's `write`) is a change of
   the member `time` as by the API (4.2), refused like it (outside the bounds, a time with a day of
   the week) with the datapoint keeping its value.
3. **Rules** read it, trigger on it and write it like any datapoint (`@"schedule/workdayMorning/time"`);
   a `read` of it is refused at load (`notReadable`). It is not persisted by the datapoint model and
   has no history: the entry's record keeps the override.

The other members stay API-only: days as a set have no datapoint type, and offsets and windows are
rarely put on a control page.

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

### 4.2 Enable switch and adjustable members

A disabled entry does not fire and has no next firing. Firings that passed while it was disabled
are not missed. The switch is persisted on the device by schedule id as an *override* of the
image's `enabled`; switching back to the image's value removes the override. A new image keeps the
override while the entry still exists and is adjustable; an entry that becomes
`enabledAdjustable: false` loses it (logged).

The inhabitant changes adjustable members (2.1) through the API (`PUT /api/schedules/<id>`,
`spec/api.md`), the UI or the time datapoint (2.2). A change names one or more members and is
applied whole or refused whole, naming the member: `not_adjustable` (the image does not list it),
`out_of_bounds` (outside its bounds or off its step), `malformed` (not a value of the member, an
empty `days`, or members that would contradict each other: `from` after `until`, `notBefore` after
`notAfter`). A value equal to the image's removes that member's override; any other value is the
member's override, persisted with the entry's record by schedule id and member. A reset
(`DELETE /api/schedules/<id>/override`) removes every override of the entry, the enable switch's
included.

A new image keeps a member's override while the entry still exists, the member is still adjustable
and the value is within the new bounds; otherwise the override is dropped and logged
(`schedule <id>: override of <member> (<value>) dropped: <why>`), and the member takes the image's
value. An override the new image's value happens to equal is no override any more. Kept overrides
that contradict the new image's other values (a kept `from` after a new `until`) are dropped too.
This is the rule of the rule parameters (`spec/rules.md` D5, 8.4).

**A change takes effect at once and is not a gap.** The entry is evaluated again with the new
values. Moments that passed before the change are not missed (4.3): moving the 06:30 to 06:00 at
06:15 fires nothing today and logs no missed firing; moving it to 06:20 at 06:15 fires at 06:20
today. **A handled firing stays handled:** the latest handled moment (4.3) is kept, and a day whose
firing was handled under the old values counts as handled under the new ones, so moving a 06:30
that fired today to 07:00 does not fire a second time today (tomorrow it fires at 07:00). This holds
for the kinds with one firing per day (`weekly`, `yearly`, `dates`, `astro`); an `interval` entry's
slots are handled up to the moment of the change only. **A `once` entry that fired (or was skipped)
is not re-armed:** it never fires again, wherever its `at` moves, even into the future; a new
one-shot is a new entry of the image. The DST rules of S1 apply to the new values (an
override of 02:30 fires at 03:00 on the spring-forward day).

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
firing, the enable and member overrides per 4.2). The state of ids no longer present is deleted. Entries are
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
| `malformed` | missing or unknown member, a value out of its range (`hour` 24, `month` 2 with `day` 30, an empty `days`, `until` before `from`, `notBefore` after `notAfter`, an offset beyond 12 h), an invalid id or label; an `adjustable` member the kind does not have or listed twice, an adjustable clamp the entry does not set, bounds of a member that is not adjustable or of `days`, `min` after `max`, a step of `at` or one that is not whole seconds (minutes for `every`), the image's value outside its bounds or off its step |
| `duplicateId` | two entries with the same id; a date listed twice in `dates` |
| `unknownReference` | `output` names no datapoint of the image |
| `typeError` | `output` names a datapoint that is not `boolean` |
| `outputConflict` | `output` names a datapoint owned by a technology (a KNX group object) or named by another entry |
| `limitExceeded` | more than 128 entries, more than 128 dates in one entry |

## 7. Not yet specified

- Datapoints for the adjustable members other than `time` (days as a set have no datapoint
  type).
- Exceptions ("weekly, but not on the dates of entry `holidays`") and vacation ranges; today a rule
  condition combines two entries' outputs.
- One-shot timers *started by rules* are the rules' own timers (`spec/rules.md` 2.5), not schedule
  entries.

## 8. Example

```json
"schedules": [
  {"id": "workdayMorning", "label": "Workday morning", "kind": "weekly",
   "days": [1, 2, 3, 4, 5], "time": {"hour": 6, "minute": 30, "second": 0}, "missed": "catchUp",
   "output": "schedule.morning", "adjustable": ["time", "days"],
   "bounds": {"time": {"min": {"hour": 5, "minute": 0, "second": 0}, "max": {"hour": 9, "minute": 0, "second": 0}, "step": 300}}},
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
