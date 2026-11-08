# Rule conformance corpus

The cases in this directory pin the semantics of `spec/rules.md`. Both rule interpreters run every
case and must produce exactly the expected outputs: the C++ core (issue #12, one table-driven test
in `core/test/`) and E-Home's Java reference interpreter (issue #13). A change of rule semantics
lands with new or changed cases; a case added on one side fails the other until it is implemented.

## Files

One case per file, `NN-<name>.json`, UTF-8, numbered in the order of the list below. New cases take
the next free number; a number is never reused.

| Member | Type | Meaning |
|---|---|---|
| `title` | string | One line: what the case pins. |
| `description` | string, optional | More explanation for people. |
| `note` | string, optional | A point for review: a judgment call, an assumption, or the decision of `spec/rules.md` section 12 the case pins. |
| `datapoints` | array | The datapoints of the image: `{"name", "type"}` with a datapoint value type of `spec/rules.md` 3.1, optional `"writable": false` (default `true`), `"refuseCommands": true` (the owning technology refuses every command), `"readable": false` (no technology can be asked for its value: a `read` of it is refused at load, `notReadable`; default `true`) and `"refuseReads": true` (the owning technology refuses every read, `readRefused`). The parameter datapoints `rule/<ruleId>/<parameterId>` (`spec/rules.md` 8.6) are not listed: the runner derives them from `rules` (and from each `image` step's rules). |
| `schedules` | array, optional | Schedule entries that exist in the image (for `schedule` triggers): each an id, or `{"id", "output"}` with the name of the datapoint the entry writes (a rule may not write it, `spec/rules.md` 10.1); or a full entry of the image (`spec/schedules.md` 2, recognized by its `kind`, without `output`), which the device's scheduler runs on the case's clock: it fires by itself, and its adjustable `time` (`spec/schedules.md` 2.2) is the datapoint `schedule/<id>/time`, which the runner derives like the parameter datapoints. A case uses either form, not both. |
| `clock` | object, optional | `{"start": "YYYY-MM-DDTHH:MM:SS", "synchronized": bool, "zone": "<POSIX TZ>"}`: the local wall time at time 0 (a time that occurs twice is its first occurrence), whether it is synchronized (default `true`) and the time zone as a POSIX TZ string (`"CET-1CEST,M3.5.0,M10.5.0/3"`; absent: UTC). Without `clock` the wall clock is not synchronized. A runner applies the zone through its own evaluation of the string, never the host's time zone. |
| `ui` | object, optional | `{"lang": "<tag>"}`: the device's UI language (the setting `ui.lang`, default `en`), in which labels are resolved (`spec/rules.md` 2.2). |
| `location` | object, optional | `{"lat", "lon"}`: the controller's location in degrees (north and east positive). With it and a synchronized `clock`, `sunAzimuth()` and `sunElevation()` (`spec/rules.md` 4.7) give the sun's position by `spec/blocks.md` 13.2 at the wall second; without it they have no value. It does not change `astro`. |
| `astro` | object, optional | The sun times of the time service: `sunrise`, `sunset`, `dawn`, `dusk` as time-of-day values; an absent one has no value. |
| `persisted` | object, optional | Parameter overrides and rule states stored before the boot: `"<ruleId>/<parameterId>": {"type": <parameter type>, "value": <JSON value>}`, and `"<ruleId>/.state": {"type": "state", "value": "<state id>"}` (`spec/rules.md` 8.5). |
| `initial` | object, optional | Datapoint values present before the rules start: `"<name>": <JSON value>`, quality good, updated at time 0. Datapoints not listed are unknown. |
| `rules` | array | The `rules` member of the image (`spec/rules.md` section 2), expressions as JSON AST. |
| `timeline` | array | The inputs, in order (below). |
| `end` | integer | The time at which the run ends. |
| `expected` | object | The outputs (below), or `{"loadRefused": "<reason>"}`. |

Times (`at`, `end`, the `at` of expected entries) are milliseconds since the boot on the monotonic
clock; the runner may map time 0 to any monotonic base. The wall clock advances with the monotonic
clock. Durations (timer remainders, duration parameters) are milliseconds.

### Values

A datapoint value, a parameter value and a command value are written as the JSON literal value of
`spec/rules.md` 3.1 for the type of the datapoint or parameter: `unsigned`, `signed`,
`enumeration`, `integer` and `real` as numbers, `timeOfDay` as `{"hour", "minute", "second"}` with
an optional `"dayOfWeek"` (1 Monday .. 7 Sunday, absent for none), `date` as
`{"year", "month", "day"}`, `dateTime` additionally `"hour", "minute", "second"`,
`"dayOfWeek"` and an optional `"summerTime": true` (the summer-time flag, `spec/rules.md` 3.1;
absent: clear), `color` as `{"red", "green", "blue"}` with an optional `"white"`, `controlled` as
`{"control", "value"}`, `raw` as hex octets `"0A FF"`, `duration` as milliseconds. Unknown (no
value) is `null`: in a `check` or `final` it means an unknown datapoint or a stopped timer.

### Timeline steps

Each step has `at` and exactly one of:

| Member | Input |
|---|---|
| `report` | `{"datapoint", "value"}`: the owning technology reports the value (origin report). |
| `stale` | datapoint name: the technology reports the datapoint stale (an update only when its quality was good). |
| `command` | `{"datapoint", "value"}`: a command from the UI or API (origin command), an external event; it is not listed in the expected commands. With `"refused": true` the model or the owning technology must refuse it (a parameter datapoint with an invalid value, or of a parameter that is not adjustable). |
| `schedule` | schedule id: the scheduler fires the entry. |
| `setParameter` | `{"rule", "parameter", "value"}`: the UI or API sets a parameter (`spec/rules.md` 8.2). |
| `setSchedule` | `{"id", <member>: <value>, ..., "enabled"?}` or `{"id", "reset": true}`: the change of `PUT /api/schedules/<id>` (`spec/api.md`) to a full entry, the members in their image form; with `"refused": "<code>"` (`not_adjustable`, `out_of_bounds`, `malformed`) the change must be refused with it. |
| `clockSync` | boolean: the wall clock becomes synchronized or not. |
| `image` | `{"rules": [...], "schedules"?: [...]}`: an image swap with these rules; the datapoints stay, with their values (`spec/rules.md` 7.6); the schedules stay too unless `schedules` gives the new image's full entries (`spec/schedules.md` 4.4: overrides kept or dropped). |
| `reboot` | `true`: an orderly reboot (delayed writes of the store complete first). Datapoint values become unknown; timers, change tracking and diagnostics are lost; persisted overrides and rule states stay; the current image is loaded again, rules with states restore their persisted states (`spec/rules.md` 7.10) and every rule starts with the reason `boot`. |
| `check` | an object like `expected.final`, compared at this time. |

## Running a case

1. Build the datapoint model from `datapoints`. Every datapoint is owned by a test technology that
   accepts every command and every read (and reports neither back) unless `refuseCommands` or
   `refuseReads` is set; a datapoint is writable and transmitting unless `writable` is `false`.
   Append the parameter datapoints of `rules` (`spec/rules.md` 8.6), owned by the rule engine.
   Store the `initial` values. Load the `persisted` overrides and states into the parameter store.
   Set the `ui` language. An `image` step loads the model again in the same way for its rules,
   keeping the values of the datapoints that stay (`spec/rules.md` 7.6).
2. At time 0 load `rules`. If `expected.loadRefused` is given, the load must be refused with that
   reason (`spec/rules.md` 10.1) and the case ends. Otherwise every rule starts with the reason
   `boot` (`spec/rules.md` 7.5).
3. For each timeline step in order: first process the expiries of all rule timers and silence
   periods with a deadline up to the step's `at`, each as a cascade at its own deadline, in the
   order of `spec/rules.md` 7.8; then apply the step at its `at`. Steps with the same `at` are
   applied in the listed order, each its own cascade.
4. After the last step, process the expiries up to `end`, then compare `expected.final`.

## Expected outputs

| Member | Compared |
|---|---|
| `commands` | Every successful `DatapointModel::command()` call made by the rule engine, in order: `at`, `rule`, `datapoint`, `value`. A command refused by the technology is not a command; it appears as a `writeRefused` log entry. |
| `reads` | Optional (absent: none). Every `read` action the technology accepted (`spec/rules.md` 7.12), in order: `at`, `rule`, `datapoint`. A refused read is not listed; it appears as a `readRefused` log entry. |
| `logs` | Every log entry of the rule engine with a code of `spec/rules.md` 9.2, in order: `at`, `rule`, `code`; for the code `log` also `level` and `message` (exact); for `loopGuard` optionally `culprit` (rule id) and `firings` (its derived firings in the cascade). Other members and the free text after the code are not compared. |
| `final` | Optional; each section compares only the keys it lists: `datapoints` (current value, `null` for unknown), `states` (rule id → state id), `parameters` (`"<ruleId>/<parameterId>"` → current value), `timers` (`"<ruleId>/<timerId>"` → remaining ms, `null` when stopped), `firings` (rule id → the diagnostics counter), `names` (rule id → the resolved name), `labels` (`"<ruleId>/<parameterId>"` → the resolved label), `schedules` (schedule id → `{"values": {<member>: <value>}, "overridden": [<member>...], "enabled": bool}`, each part optional, `values` in the image form of the members; `overridden` the complete set), `persisted` (the complete set of stored overrides and states, as the store holds them after its delayed writes: `"<ruleId>/<parameterId>"` → value, `"<ruleId>/.state"` → state id). |

Reals compare with a relative tolerance of 1e-9; everything else compares exactly.

## Cases

| File | Pins |
|---|---|
| `01-trigger-datapoint-change` | `datapoint` trigger with `on: change` versus `on: update`; a repeated report is an update. |
| `02-trigger-datapoint-origin` | `origin: report` and `origin: command`; a stale report counts as report; UI commands are external. |
| `03-trigger-match-edge` | `match` fires on the rising edge only; baseline at boot without firing; a stale value resets the edge. |
| `04-trigger-schedule` | `schedule` trigger; an unfired schedule does nothing. |
| `05-timer-presence-light` | Worked example 1: `timer` trigger, `restartTimer`, `cancelTimer`, `onlyOnChange`, parameters in a condition and a timer duration. |
| `06-trigger-start` | `start` trigger with reasons `boot` and `image`; a reboot loses datapoint values and diagnostics. |
| `07-trigger-parameter` | `parameter` trigger; out-of-range and unknown parameters refused; setting the same value fires nothing; the override is persisted. |
| `08-operators-arithmetic` | Precedence, integer and real, `/` always real, `%` with the dividend's sign, a refused unsigned write, division by zero, integer overflow. |
| `09-operators-comparison-logic` | Comparisons of numbers, strings, enumerations, colors, times and dates; `and`, `or`, `not` and their precedence. |
| `10-operators-duration-time` | Duration literals and arithmetic, time of day wrapping at midnight, time-of-day and date differences, date-time arithmetic. |
| `11-functions-numeric` | `round` half away from zero, `floor`, `ceil`, `trunc`, `roundTo`, `clamp` (with an error), `min`, `max`, `abs`, `between`, `toInt`. |
| `12-functions-string-color-controlled` | `length` in bytes, `contains`, `startsWith`, `endsWith`, `toString`, the 28-byte limit, `rgbw`, channels, an invalid channel, `controlled`. |
| `13-null-condition` | A condition without value: neither branch, `conditionNoValue` once until it had a value; three-valued `and` and `or`. |
| `14-null-stale-known-lastknown` | `known`, `lastKnown`, `quality`, `coalesce` for unknown, good and stale; `valueNoValue`. |
| `15-eval-error-once` | `evalError` once per expression until it evaluates without error; later actions still run; an error in a condition. |
| `16-write-refused-onlyonchange` | Exact write conversion, technology refusal, `onlyOnChange` against a value set by the UI. |
| `17-time-functions` | `weekday`, `time`, `now`, `today`, `hour`, `minute`, `inWindow` across midnight, `sunrise`, `sunset`; no value when not synchronized. |
| `18-history-since-change` | `sinceChange`, `changedWithin`, `sinceUpdate`; a repeated report is not a change; a never-known datapoint. |
| `19-function-hysteresis` | `hysteresis` with the output datapoint as previous state; `low >= high` is an evaluation error. |
| `20-timers-start-restart-cancel` | `startTimer` versus `restartTimer`, `cancelTimer`, `running`, `remaining`, expiry order, a refused duration. |
| `21-state-machine-staircase` | Worked example 4: entry actions, timer transitions, a self-transition restarting the timer. |
| `22-state-machine-order` | First satisfied transition wins; exit, actions, entry order; a guard without value is skipped; internal reactions. |
| `23-queue-order` | FIFO cascade, rule order per event, expressions read current values. |
| `24-loop-guard` | A ping-pong refused after 32 derived firings with `loopGuard`; the next external event is processed normally. |
| `25-image-swap-state-timers` | State and running timers survive a swap by id; removed rules and timers are cancelled; `stateReset`. |
| `26-image-swap-parameters` | Overrides kept while declared, type-compatible and valid; `parameterReset` otherwise; undeclared and removed ones deleted. |
| `27-parameter-persistence-reboot` | Persisted overrides at boot and across a reboot; a narrowed range resets; an undeclared stored override is deleted. |
| `28-enable-switch` | Disabling cancels timers and stops firing; enabling starts with reason `enable`; a fixed switch is refused. |
| `29-set-parameter-action` | `setParameter` as an action: a derived event, persisted; an invalid value refused. |
| `30-log-format` | Value formatting in log messages; cutting at 200 bytes on a character boundary. |
| `31-load-refused-type-error` | Load refusal `typeError`. |
| `32-load-refused-match-time` | Load refusal `forbidden`: a time function in a `match` condition. |
| `33-load-refused-unknown-timer` | Load refusal `unknownReference`. |
| `34-example-heating-setback` | Worked example 3: setpoint from the schedule's comfort period, window, parameters; `onlyOnChange`. |
| `35-example-blind-sun-wind` | Worked example 2: wind hold timer, sun window, `hysteresis`, `if`. |
| `36-parameter-reset-to-default` | Setting the default removes the override; a stored override across a reboot; the parameter follows the default of later images (decision 15). |
| `37-state-persistence-reboot` | States survive a reboot without entry actions, timers do not; `stateReset` for an undeclared persisted state and a rule without states; a swap that drops the state (decision 3). |
| `38-date-time-across-dst` | `dateTime` arithmetic and comparison on the instant across the end of summer time in Europe/Berlin (decision 7). |
| `39-load-refused-schedule-output` | Load refusal `notWritable`: a write to a schedule entry's output datapoint. |
| `40-labels-language` | Labels as strings and language maps resolved from `ui.lang`: exact tag, base language, `en`, first entry (decision 11). |
| `41-canonical-normalization` | Normalized trees: folded minus, enumeration literals by partner, the controlled literal as a call, `mon 07:30` (decision 6). |
| `42-state-override-fold-labels` | Decisions 3, 7, 11 and 15 together: a `setParameter` action to the default removes the override, the state persists across a reboot, date-time subtraction across the end of summer time, a regional label falls back to its base language. |
| `43-action-read` | The `read` action at the start and on a schedule; the read changes no value; reads are not commands (decision 12). |
| `44-action-read-refused` | A read the technology refuses: `readRefused` once per action; the following actions still run. |
| `45-load-refused-not-readable` | Load refusal `notReadable`: a read of a datapoint no technology can be asked for. |
| `46-trigger-silence` | The `silence` trigger: once per silent period, re-armed by a report, a stale report is no sign of life (decision 12). |
| `47-silence-enable-parameter` | Silence and the enable switch (disarmed, then counted from the enabling); the period from a parameter, re-armed on its change. |
| `48-silence-image-swap` | Silence across an image swap: counted from the last update; a period that ended before the swap fires right after it. |
| `49-silence-reboot` | Silence after a reboot: counted from the start, also for a datapoint that never reports. |
| `50-parameter-datapoint-change` | Parameters as datapoints `rule/<ruleId>/<parameterId>`: values at the start, after API changes, an action and a reset; a duration in ms (decision 10). |
| `51-parameter-datapoint-command` | A command to a parameter datapoint sets the parameter; an invalid value is refused; a fixed parameter's datapoint is not writable. |
| `52-parameter-datapoint-other-rule` | Another rule reads, triggers on and writes a parameter datapoint; the parameter's event comes before the datapoint's update. |
| `53-parameter-datapoint-enabled` | The enable switch as a datapoint: commands disable and enable the rule, another rule follows it, a fixed switch is not writable. |
| `54-silence-read-parameter-datapoint` | A `silence` trigger whose period is a parameter: a tuner writes the parameter's datapoint `rule/watch/after`, the silence fires a `read` after it, the answer re-arms it, and after a reboot the silence counts from boot. |
| `55-schedule-adjust-time` | An adjustable time: within its bounds and on its step accepted, otherwise `out_of_bounds`; a fixed member `not_adjustable`; the override survives a reboot; setting the image's value removes it (`spec/schedules.md` 2.1, 4.2). |
| `56-schedule-change-semantics` | A change is not a gap: moving the time earlier past now fires nothing today, later today fires today, a day that fired stays handled; a reset returns to the image (`spec/schedules.md` 4.2). |
| `57-schedule-time-datapoint` | `schedule/<id>/time`: its quiet value at the load, a command changes the schedule, one outside the bounds is refused, an API change updates it, rules read it when the entry fires (`spec/schedules.md` 2.2). |
| `58-schedule-image-swap-overrides` | An image swap keeps an override while the entry, the adjustable member and the fit to the new bounds remain, and drops it otherwise (`spec/schedules.md` 4.2, 4.4). |
| `59-sun-position` | `sunAzimuth()` and `sunElevation()` at the cascade's wall second (the milliseconds dropped) from the case's `location`, through the time zone; below the horizon at night; the log format of the exact values. |
| `60-sun-position-nrel-example` | The NREL Solar Position Algorithm's published example (Golden, Colorado, 2003-10-17 12:30:30 UTC-7) at the start: a western longitude, a fixed zone. |
| `61-sun-position-no-clock` | No value while the wall clock is not synchronized, also with a location: `valueNoValue` and `conditionNoValue` once, `known()`, `coalesce()`, again after a value; the southern hemisphere. |
| `62-sun-position-no-location` | No value without a location while `time()` has its value. |
| `63-load-refused-match-sun` | Load refusal `forbidden`: `sunElevation()` in a `match` condition (a rule cannot trigger on the sun's position). |
| `64-sun-follow-timer` | Following the sun with a timer that restarts itself (the pattern of `spec/rules.md` 4.7): `onlyOnChange` writes when the sun comes onto a facade and leaves it. |
| `65-parameter-id-quoted` | A parameter id with a hyphen, `$"max-helligkeit"` in the text (issue #81, schema 1.5): in a condition and a value, a `setParameter` action, a parameter trigger, its datapoint `rule/dimmer/max-helligkeit` read by another rule, a refused command to it, the persisted override across a reboot; the identifier `max_helligkeit` beside it is another parameter. |
| `66-load-refused-parameter-id-slash` | Load refusal `malformed`: a parameter id with `/` (spec/rules.md 2.1). |
| `67-load-refused-parameter-id-space` | Load refusal `malformed`: a parameter id with a space. |
| `68-load-refused-parameter-id-umlaut` | Load refusal `malformed`: a parameter id with an umlaut. |
| `69-load-refused-parameter-id-length` | Load refusal `malformed`: a parameter id of 25 characters. |
| `70-load-refused-parameter-id-enabled` | Load refusal `duplicateId`: a declared parameter `enabled` (reserved for the enable switch). |
| `71-load-refused-parameter-id-keyword` | Load refusal `malformed`: a parameter id that is a keyword (`not`). |

The expected outputs were computed by hand and cross-checked against a throw-away executable model of
the specification. The sun's positions of cases 59 to 64 come from the throw-away executable model of
the block corpus (`spec/fixtures/blocks/README.md`), which writes `spec/blocks.md` 13.1 and 13.2
literally in binary64; every position was cross-checked against the same formulas on the C library's
`sin`, `cos`, `asin` and `atan2` (within 1e-9°) and the Astronomical Almanac's low-precision formula
(a different algorithm, within 0.02°), case 60 also against the NREL example's published topocentric
azimuth 194.34024° and elevation without refraction 39.872046° (within 0.0025° after the parallax);
no decision of case 64 lies within 0.02° of its threshold. A `note` starting with "Review:" contains
a judgment call the author should confirm, one starting with "Decided" names the decision of
`spec/rules.md` section 12 it pins.
