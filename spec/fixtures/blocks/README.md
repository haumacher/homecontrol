# Block conformance corpus

The cases in this directory pin the semantics of `spec/blocks.md`: the block contract (calls,
cascades, faults, failsafe values, the enable switch, persistence, image swaps, overrides, the quiet
load, load refusals) and the built-in types (`spec/blocks.md` 14, 16, 17, 18: the room controller, the heating demand, the heating curve, the sun shading). Every implementation
runs every case and must produce exactly the expected outputs: the C++ core (one table-driven test,
`core/test/rules_corpus_test.cpp`, which runs the rules corpus too) and E-Home's simulation
(haumacher/ehome#75). A change of block semantics lands with new or changed cases.

## Files

One case per file, `NN-<name>.json`, numbered in the order of the list below; a number is never
reused. The format is that of the rules corpus (`spec/fixtures/rules/README.md`): every member and
timeline step defined there means the same here. The additions:

| Member | Type | Meaning |
|---|---|---|
| `blocks` | array | The `blocks` member of the image (`spec/blocks.md` 4). `rules` may be absent (no rules). The block mirrors `block/<id>/<x>` (`spec/blocks.md` 6) are not listed in `datapoints`: the runner derives them, after the parameter datapoints of the rules. |
| `persisted` | object | Also block parameter overrides, `"@<blockId>/<parameterId>": {"type": <parameter type>, "value": <JSON value>}`, and persisted state records, `"block/<id>": {"type": "<type id>", "version": "<major>.<minor>", "fields": {"<field>": <number>}}` (written as the device's record of `spec/blocks.md` 8.1). |
| `location` | object | `{"lat", "lon"}`: the controller's location in degrees (north and east positive). With it and a synchronized `clock`, `sun()` (`spec/blocks.md` B21) gives the sun's position by `spec/blocks.md` 13.2 at the instant of the wall clock; without it, none. |
| `expected.loadRefused` | string | As for rules, with the reasons of `spec/blocks.md` 11; `expected.path` (optional) is the refusal's path. |

The conformance type `org.homecontrol.conformance` (`spec/blocks.md` 15) is registered by the runner
in versions 1.1 and 2.0; the firmware does not contain it.

### Timeline steps

All steps of the rules corpus, and:

| Member | Input |
|---|---|
| `setBlock` | `{"block", "enabled"?, "parameters"?: {"<id>": <value>, ...}, "reset"?: ["<id>", ...], "restart"?: true}`: the change of `PUT /api/blocks/<id>` (`spec/api.md`). With `"refused": "<code>"` (`unknown`, `unknown_parameter`, `not_adjustable`, `invalid_value`, `not_faulted`) it must be refused with that code, and nothing changes. |
| `image` | `{"rules"?: [...], "blocks"?: [...], "schedules"?: [...]}`: an image swap; an absent `rules` or `blocks` means the new image has none. |
| `reboot` | As for rules; the block runtime writes its due records first (an orderly reboot, `spec/blocks.md` 8.1). |

## Running a case

As in the rules corpus, with these additions:

1. The model is the case's datapoints, the rules' parameter datapoints, then the blocks' mirrors.
   The persisted overrides and records are stored before the boot.
2. At time 0 the rules are loaded, then the blocks (a refusal of either ends the case with
   `loadRefused`); the rules start with the reason `boot`, then the blocks (`spec/blocks.md` 5.2).
3. Timer expiries of rules and blocks, and the blocks' periods, are processed with the steps, in the
   order of `spec/blocks.md` 5.1.

## Expected outputs

| Member | Compared |
|---|---|
| `commands` | Every successful `DatapointModel::command()` by a rule or a block, in order: `at`, `rule` or `block`, `datapoint`, `value`. Publishes of mirrors are not commands; they show in `final.datapoints`. |
| `logs` | Every log entry of the rule engine and of the block runtime with a code (`spec/rules.md` 9.2, `spec/blocks.md` 9), in order: `at`, `rule` or `block`, `code`; for the code `log` also `message`. |
| `final` | As for rules, and: `blocks` (block id → `{"status": "running" \| "disabled" \| "fault", "state": {<field>: <number>}, "parameters": {<id>: <value>}, "calls": <number>}`, each part optional, `state` and `parameters` comparing only the keys listed); `records` (the complete set of persisted state records after an orderly shutdown's writes: block id → `{<field>: <number>}`, the persisted fields only); `persisted` includes the block overrides `@<blockId>/<parameterId>`. |

Reals compare with a relative tolerance of 1e-9 (and an absolute one of 1e-9 around 0); everything
else compares exactly. State fields that hold monotonic times (the room controller's `lastStep`,
`cycleStart`, `boostUntil` while set, `protectionUntil`, `sentAt`; the heating demand's `lastStep` and
`switchedAt`; the heating curve's `lastStep`; the sun shading's `windCalmAt`, `rainDryAt`, `frostClearAt`, `brightAt`, `manualAt`, `manualSeen` and `movedAt` while not -1) depend on the origin a runner
gives the monotonic clock and are not compared; differences of them are. Several `parameterReset`
entries of one block for parameters that are gone come in an unspecified order; no case has two.

## Cases

| File | Pins |
|---|---|
| `01-load-refused-unknown-type` | A version the firmware does not have refuses the image with unknownType. |
| `02-load-refused-missing-input` | An unbound required input refuses the image with missingInput. |
| `03-load-refused-type-mismatch` | A real input bound to a boolean datapoint refuses the image with typeError. |
| `04-load-refused-output-owned` | An output bound to a schedule entry's output refuses the image with outputConflict. |
| `05-load-refused-parameter-bounds` | A parameter value outside its bounds refuses the image with invalidParameter. |
| `06-load-refused-aggregation` | An aggregation the port does not allow refuses the image with forbidden. |
| `07-load-refused-unknown-member` | An unknown member of a block refuses the image with malformed (blocks are strict, B1). |
| `08-calls-inputs-outputs` | Start, input calls by change and by update, the aggregated mean and its mirror, quality, a timer and the period. |
| `09-fault-failsafe-restart` | A fault writes the failsafe values once, stops the block until a restart, and the restart starts from the persisted record. |
| `10-disable-enable-failsafe` | Disabling writes the failsafe values and stops the calls; enabling starts with the reason enable; a disabled block at boot writes its failsafe values. |
| `11-image-swap-keeps-state` | A swap to a newer minor version of the same type keeps the state record and the overrides; the timers stop. |
| `12-image-swap-drops-state` | A swap to another major version starts the state fresh (stateReset) and drops the overrides that no longer fit (parameterReset). |
| `13-quiet-load` | The mirrors of the parameters, the enable switch and an aggregated input take their values at a load without an event; later changes are events. |
| `14-loop-guard` | A block and a rule that feed each other are stopped by the loop guard after 32 derived firings and calls; the next external event runs normally again. |
| `15-persistence-reboot` | The persisted fields survive an orderly reboot; a stored record of another type starts that block fresh (stateReset). |
| `16-parameter-api` | Changes through the API and the parameter datapoints: validated, persisted as overrides, the default removes one, refusals name their reason. |
| `17-room-pi-step` | Room controller, continuous, radiator preset: the PI(D) step over the monotonic dt with the D term on the filtered value, a setpoint change, minChange. |
| `18-room-window-no-windup` | A window open for 2 h: after windowDelay the controller regulates to the frost setpoint, the output saturates at 0 and the integral does not wind up; closing returns at once. |
| `19-room-sensor-timeout` | No report for sensorTimeout, or a stale sensor, puts the valve at the fallback position; the next good report regulates again. |
| `20-room-pwm` | PWM: the switch is on for valve% of each cycle; a cycle restarts when the state changes. |
| `21-room-two-point` | Two-point control on the filtered value: on at or below setpoint - hysteresis/2, off at or above setpoint + hysteresis/2, unchanged between. |
| `22-room-boost` | A setpoint step of at least boostThreshold opens the valve fully until the error falls below half the threshold; the integral is frozen meanwhile. |
| `23-room-valve-protection` | Valve protection on its weekday from protectionTime: the valve fully open for protectionDuration, before summer; once per day. |
| `24-room-modes` | The setpoint from presence, the heating period, the HVAC mode and an external setpoint; frost mode, off, forced and summer. |
| `25-room-integral-reboot` | The integral survives an orderly reboot (persisted at most every persistEvery and at the reboot); the filter starts again from the first report. |
| `26-fault-disabled-swap` | A block disabled while faulted is found disabled by the next image and writes nothing; its outputs hold the failsafe values of the fault (6.1). |
| `27-demand-hysteresis-min-times` | Heating demand: the request's hysteresis; minOn and minOff hold a switching until they expire (the timer wake), also one asked by enable and summer. |
| `28-demand-top-mean-stale` | Heating demand: the level over valves and demands read one by one (B20), the mean of the highest, a demand as 100 %, clamping; a datapoint that is not good ignored or counted as full; staleInputs; the ports' mirrors. |
| `29-demand-pump-run-on` | Heating demand: the pump's run-on; the wake timer at the earlier of a held switching and the run-on's end; a switching off by enable runs the pump on. |
| `30-demand-offset-persist` | Heating demand: the offset integrates the level above targetLevel, falls below it and while the request is off, stays in 0..maxOffset, is sent in steps of 0.1 K and survives an orderly reboot. |
| `31-demand-32-valves` | An input binds 32 datapoints (B20): the mean of all of them, one stale left out. |
| `32-load-refused-multiple` | 33 datapoints for an input of multiple 32 refuse the image with forbidden. |
| `33-load-refused-demand-thresholds` | The heating demand's check (B17): offThreshold not below onThreshold is invalidParameter on offThreshold. |
| `34-curve-forms` | Heating curve: slope and level, the design point, points with interpolation and flat ends; the room setpoint along the diagonal, the offset, minFlow/maxFlow, minChange. |
| `35-curve-filter-persist` | Heating curve: the outside filter over the monotonic dt, persisted over an orderly reboot; state fault until the outside reports again. |
| `36-curve-summer-switch` | Heating curve: summer after summerDays above summerOn (reset when not above), winter below summerOff; the external summerSwitch decides while good. |
| `37-curve-frost` | Heating curve: frost protection on the momentary outside temperature in summer and when off, with its 1 K hysteresis; offFlow otherwise. |
| `38-curve-sensor-timeout` | Heating curve: a stale outside temperature is not fed; fault after sensorTimeout for a stale and for a silent one, on the held value; a report recovers. |
| `39-load-refused-curve-points` | The heating curve's check (B17): the points' outside temperatures must rise; the first that does not is blamed. |
| `40-heating-chain` | A room controller, a heating demand and a heating curve chained through their mirrors: valve to level, offset to flow, the curve's summer to the request. |
| `41-demand-image-swap-keeps-hold` | Heating demand at an image swap: the record in RAM keeps the request and its switching time, so a deploy does not cut short minOn; the start writes the outputs again. |
| `42-shading-sun-on-facade` | Sun shading: the sun on the facade by azimuth and elevation (`spec/blocks.md` 13.2, 18.2 D8), horizon points and an overhang; `sun()` itself through the sun mirrors; one move at the begin and one at the end of shading. |
| `43-shading-slat-tracking` | Sun shading: slats at the cut-off angle, in steps of slatStep from slatOpen (180° slats: slatOpen 45, step 7); minMoveInterval holds the steps back, and a step passed while held is skipped. |
| `44-shading-brightness` | Sun shading: brightness hysteresis and both delays, by the timer wake; the maximum of two sensors; a sensor that is not good counts as dark. |
| `45-shading-wind` | Sun shading: the wind alarm over shading and heat protection, an operation during it, a gust restarting the release, a stale storm, windTimeout at the start and for a silent sensor. |
| `46-shading-rain-frost` | Sun shading: rain and frost (the alarm input and the outside temperature), the order wind > rain > frost, each released after alarmRelease. |
| `47-shading-manual-duration-sun-change` | Sun shading: the locks `duration` (restarted by a second operation) and `sunChange`; a lock that ends in shading moves, one that ends in idle writes nothing. |
| `48-shading-manual-midnight` | Sun shading: the lock `midnight` on the local date: an evening closing keeps the setting sun from retracting the blind; the lock ends at local midnight. |
| `49-shading-heat-protection` | Sun shading: heat protection without a sun position, its 1 K hysteresis and releases; enable false switches the automatic off while the alarms work on. |
| `50-shading-min-interval-reboot` | Sun shading: minMoveInterval holds moves and releases until the timer wake, with the target as it is then; a pointless held move is dropped; a reboot in idle writes nothing. |
| `51-load-refused-own-output` | B22: an output bound to a datapoint an input of the same block binds refuses the image with forbidden. |
| `52-load-refused-shading-horizon` | The sun shading's check (B17): the horizon points' directions must rise. |
| `53-shading-failsafe-parameters` | Sun shading: the failsafe is retracted; enabled again it moves at once with all outputs; the adjustable shadePosition and its reset move the blind. |
| `54-shading-sensors-one-by-one` | Sun shading: wind and rain read one by one (D4): a stale storm behind a good calm and one silent sensor of two are alarms; a stale `manual` report and the start value are no operation (D6). |

## How the expected values were computed

The inputs (the datapoints, blocks, rules and timelines) were written by hand, with the expected
load refusals of 01 to 07. Every other expected output (`commands`, `logs`, `final` and each `check`)
was computed by a throw-away executable model of `spec/blocks.md` in Python, written by a separate
agent from the specification texts alone (`spec/blocks.md`, `spec/rules.md`, the two READMEs),
without access to the C++ sources; it implements the block runtime, the room controller and the
conformance type literally in IEEE doubles, and the subset of the rules the cases use (datapoint and
start triggers, write actions, simple expressions). Its 30 questions on the specification were
answered in the specification before the cases were filled (the PWM's sentinel and stale timer,
dead bumpless code, timers across a swap, a faulted block at a swap, among others). The model filled
the cases with `--write`; first numbers were checked by hand (valve 40 then 27 in case 17, the
filtered value, P, I and D of its first step), and the C++ runner passes every case unchanged. A
`note` names what the case pins or a judgment call.

Cases 27 to 41 (the heating demand and the heating curve, `spec/blocks.md` 16, 17 and B20) were made
the same way. Their inputs were written by hand, with the load refusals of 32, 33 and 39. The same
model was extended by a separate agent from the specification texts alone, without access to the
C++ or the Java sources (`count`/`readAt`, the two types' manifests, checks, calls and steps,
literally in IEEE doubles), with two self-test cases of its own whose values it computed by hand.
Its 19 questions on sections 16, 17 and B20 were answered in the specification before the cases were
filled (among them `targetLevel`'s lower bound, `min`/`max` on ties in 13, the failsafe phase and the
minimum times, the stale value for frost protection, a duplicate binding). The model filled the cases
with `--write`. Independently, the numbers were derived by hand while the cases were designed and
compared with the model's: the switching times of 27 (30, 330, 570, 870, 1110, 1410 s) and 29 (10,
70, 190, 370, 430, 550, 730 s); the levels of 28 (45, 100, 75, 90, 45, 100, 80) and 31 (16.5, 16,
594.5/31); the offset of 30 (0.25 K a minute, 3 at 780 s, 2.1667 after the reboot, 2.8333 at the
end); the flows of 34 (slope 51, 58.5, 62.25, 64.25, 34.55, 25, 70; design 40, 45, 25, 60; points 39,
44, 47, 29, 22.5, 50.5), 37 (10, 25, 10, 39.2, 10, 25) and the first filter step of 35 (43.8033); the
summer of 36 at 133 200 s; the swap of 41 (the outputs again at 60 s, off at 330 s, the pump at 390 s). Two inputs of the first draft were wrong, not the model (a missing
`outsideFilter` in 34, a single report in 36 that ran into `sensorTimeout`); they were corrected and
filled again. The C++ runner passes every case unchanged.

Cases 42 to 54 (the sun shading, `spec/blocks.md` 18, B21, B22, 13.1, 13.2) were made the same way.
Their inputs were written by hand (42 to 53; 54 by the model's agent from its self-test, reviewed),
with the load refusals of 51 and 52. The same model was extended by a separate agent from the
specification texts alone, without access to the C++ or Java sources: the exact trigonometry and
the sun's position literally in binary64 (never a library sine on that path), `sun()` from the
case's new `location` and the wall clock, B22 and the type. Its 26 questions (the instant of
`sun()`, the location's source, the order of the output checks, what counts as an operation, a
stale storm hidden by the aggregation, the slat steps' origin, `overhangAngle` 90, among others)
were answered in the specification before the cases were filled; four of the answers changed the
type (`manualSeen`, wind and rain read one by one, slat steps from `slatOpen`, no overhang test at
90). The model filled the cases with `--write`. Independently, the agent derived the numbers by hand
and, for the sun, with the Astronomical Almanac's formula on the library's functions: the begin and
end of shading in 42 (4 800 and 21 240 s for the east facade, 11 820 s for the south), the slat steps
of 43, the switching times of 44 (430, 3 600, 3 820, 4 800 s), the alarms of 45 and 46, the locks of
47 and 48, and the moves of 49, 50 and 53; all agreed with the model. The smallest margin of a
decision to its threshold is 0.016° (42, the azimuth test of the south facade), far above the 1e-9
of the comparison and above the 0.01° between the two sun formulas. The C++ runner and E-Home's
runner pass every case unchanged; `sun()` gives the same bits in all three (`SunPosition.TheNrelSpaExample`
pins them).
