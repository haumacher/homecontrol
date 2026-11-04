# Block conformance corpus

The cases in this directory pin the semantics of `spec/blocks.md`: the block contract (calls,
cascades, faults, failsafe values, the enable switch, persistence, image swaps, overrides, the quiet
load, load refusals) and the built-in room controller (`spec/blocks.md` 14). Every implementation
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
`cycleStart`, `boostUntil` while set, `protectionUntil`, `sentAt`) depend on the origin a runner
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
