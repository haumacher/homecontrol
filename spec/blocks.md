# Blocks: virtual devices on the controller

*Specification of issue #60. A block is a reusable function with typed inputs, outputs, parameters
and internal state (a room controller, a heating demand, sun shading, a house mode), instantiated
per room, facade or circuit, run by the controller and simulated by E-Home (haumacher/ehome#75).
Implemented by the core's block runtime (`core/include/hc/blocks/`); the instances travel in the
runtime image as the member `blocks` (`spec/runtime-image.md`, schema 1.3); their API is in
`spec/api.md` "Blocks". The conformance corpus is `spec/fixtures/blocks/` (format in its
`README.md`), run by the C++ core and by E-Home's simulation. This document follows the conventions
of `spec/rules.md` (camelCase members, the JSON literal values of its 3.1, labels of its 2.2,
refusal reasons of its 10.1).*

## 1. Introduction

A *block type* is a function with a *manifest*: its ports (inputs and outputs), its parameters, its
state and what calls it wants. A *block instance* (a *block* for short) is one use of a type in the
image: it binds the input ports to datapoints, optionally the output ports too, and sets parameter
values. The controller calls the instance on the event loop when an input changes, when one of its
timers expires, periodically, at its start and when a parameter changes; the instance reads its
inputs and parameters, keeps its state and writes its outputs.

The rule language (`spec/rules.md`) stays for simple glue between datapoints. Everything with state
and numerics (a PI controller, a heating curve, a shading algorithm) is a block: in the rule
language it would need helper datapoints and subexpressions beyond the limits of `spec/rules.md`
10.2.

### 1.1 Decisions of the author (2026-11-03)

1. **Rules stay glue; everything with state and numerics is a block.**
2. **Blocks start as a built-in library** of C++ types in the firmware.
3. **Loadable blocks follow as WebAssembly modules** against the same contract (the spike #59,
   `doc/blocks-wasm.md`).
4. **The project is open: anybody may write blocks.** The security boundary is the sandbox and the
   image signature (section 10).

### 1.2 Decisions of the specification session (2026-11-03, the comment "Design decisions" of #60)

- **B1. Schema 1.3; E-Home is the guard.** The member `blocks` is new in image schema 1.3, and
  E-Home always writes 1.3. A firmware that reads only up to 1.2 ignores unknown top-level members
  (`spec/runtime-image.md` I5) and loads a newer minor version (I4): it would load an image with
  blocks and silently run no room controller. Therefore E-Home refuses to deploy an image with
  blocks to a controller that reports a schema below 1.3 (a refusal, not the warning of #58); a
  controller that reports no schema counts as at most 1.1. From 1.3 on, `blocks` is strict inside
  (unknown members refused, with the "update the firmware" hint of I4), like rules and schedules.
- **B2. One namespace `block/<id>/<x>`** for the outputs, the aggregated inputs, the adjustable
  parameters and the enable switch of an instance (section 6). The manifest refuses a clash of ids.
  An input gets a mirror datapoint only when it aggregates several datapoints; a single-datapoint
  input gets none. Every instance has `block/<id>/enabled`; the id `enabled` is reserved.
- **B3. Aggregation is chosen per instance** in the image (`"aggregation": {"temperature": "min"}`)
  from the set the port allows; it is not a parameter.
- **B4. Persistence: one record per instance**, written at most every `persistEvery` of the type
  (the room controller: 10 minutes) and at an orderly reboot; an unorderly reset loses at most that
  interval. A change of the type's major version starts the state fresh; a change of the minor
  version keeps it. Parameter overrides use the store of the rule parameters; an override survives
  a new image only while its parameter exists with a compatible type and its value is within the
  new bounds, otherwise it is dropped and logged, never clamped.
- **B5. The corpus** `spec/fixtures/blocks/` uses the format of the rules corpus, with `blocks`
  beside `rules` and an expected `final.blocks` (state, status); the device and E-Home's simulation
  run it.

### 1.3 Decisions taken in this specification

Each names its alternatives; the sections state the decision as fact.

- **B6. Values are numbers at the block's interface.** A block reads and writes IEEE 754 doubles:
  a boolean is 0 or 1, an integer and an enumeration their number, a duration milliseconds, a time
  of day the seconds since midnight. Port types are `boolean`, `integer`, `real` and `enumeration`;
  parameter types add `duration` and `timeOfDay`. This is the WebAssembly ABI of the spike
  (`f64 read(port)`, `f64 param(id)`, `write(port, f64)`), so a loaded block and a built-in one see
  the same values. *Alternatives:* the full value types of `spec/rules.md` 3.1 (strings, colors,
  dates: no block needs them yet, and each would need its own ABI encoding).
- **B7. Blocks are called after the rules.** For each event of a cascade the rules react first
  (`spec/rules.md` 7.2), then the blocks in block order. At a start the rules' start cascades come
  first, then one start cascade per block. At equal deadlines a rule timer or silence period comes
  before a block's timer or period. *Alternative:* blocks first (no gain: a block's outputs reach
  the rules as derived events either way; rules first keeps every image without blocks exactly as
  before).
- **B8. A parameter change is a call.** Besides `start`, `input`, `timer` and `period`, a block
  receives `parameter(id)` when an adjustable parameter changes, so a room controller reacts to a
  new setpoint at once. *Alternative:* the block notices at its next period (a setpoint change
  would wait up to a period).
- **B9. A write commands every time; the mirror follows changes.** `write(port, v)` commands the
  bound datapoint like a rule's `write` (`spec/rules.md` 7.3, without `onlyOnChange`), and then
  publishes the output's datapoint `block/<id>/<port>` when its value differs. A block that sends
  only on change (the room controller's `minChange`) does so itself. *Alternative:* the runtime
  filters equal values (a block could not resend cyclically, which KNX actuators with a
  surveillance time need).
- **B10. Stopping writes the failsafe values once.** A fault, a disable and a disabled instance at
  its start write each failsafe output's failsafe value once; while stopped the instance writes
  nothing. Diagnostic outputs have no failsafe and keep their last value. *Alternative:* the
  outputs keep their last values (a valve stays where a faulty controller left it).
- **B11. A restart is a boot for the instance.** `restart` after a fault restores the persisted
  record (section 8) as at boot, because the state in RAM may be what faulted; a faulted instance
  is not persisted any more. A non-finite state field or output value is a fault.
- **B12. The state is a record of numbered numeric fields.** The manifest declares the fields by id
  (each a double, persisted or not); the record is kept by field id across minor versions.
  Fields that are not persisted start at their initial value whenever the record comes from flash.
  *Alternative:* opaque bytes (the spike's `hc_state`): no diagnostics by name, no minor-version
  compatibility.
- **B13. The flash budget paces the records.** The runtime writes at most 30 records an hour in
  all: an instance's record is written at most every `max(persistEvery, n × 120 s)`, where `n` is
  the number of instances with persistent state, and only when it changed (`doc/persistence.md`
  "The flash wear budget").
- **B14. One writer per output datapoint.** An output may bind a datapoint the application may
  command (`writable`): a KNX, Zigbee or virtual datapoint. Refused: one that another output (of any
  block) binds, a schedule entry's output, the time master's, and the implied datapoints of rules,
  schedules and blocks. A rule may still write a datapoint a block output binds (E-Home warns).
- **B15. The image's parameter values are the instance's defaults.** The manifest's default is
  replaced by the image's value; an inhabitant's override is kept against that default as for rule
  parameters (`spec/rules.md` D5): setting the default removes the override, and an untouched
  parameter follows every new image.
- **B16. Versions match by major.** The image names `<major>.<minor>`; the firmware's type must
  have the same major and at least that minor. Built-in types keep one version per major.
- **B17. Cross-parameter checks belong to the type.** Constraints the manifest cannot express (the
  room controller's `outputMin` below `outputMax`) are checked by the type at load and refused
  with `invalidParameter`.
- **B18. A conformance type exists only in the test harnesses.** The contract cases need a block
  that is simple enough to compute by hand and that faults on demand: `org.homecontrol.conformance`
  (section 15). The device's firmware does not contain it; the C++ test runner and E-Home's
  simulation register it for the corpus.
- **B19. Determinism.** Built-in blocks use only `+`, `-`, `*`, `/`, `sqrt`, comparisons, `floor`
  and `fabs` on IEEE 754 doubles with round-to-nearest-even, evaluated in the order this document
  writes them, without fused multiply-add (`-ffp-contract=off`), so Java (`double`, no `Math.fma`)
  and C++ give the same bits. `sqrt` is one of IEEE 754's correctly rounded basic operations like
  the four others (Java's `Math.sqrt`, C's `sqrt`). No transcendental functions of a library:
  where a type needs a sine or an arc tangent, it uses the exact trigonometry of 13.1, built from
  those operations alone. The corpus compares reals with a relative tolerance of 1e-9 and its cases
  keep threshold decisions away from that tolerance.
- **B20. An input binds up to 32 datapoints, and a block may read each of them.** `multiple` goes up
  to 32, and besides the aggregated `read(port)` a call may ask `count(port)` and `readAt(port, k)`,
  the k-th datapoint of the binding as a single-datapoint input reads it (3.1). The heating demand
  (section 16) combines the valves of up to 32 rooms with rules of its own (the mean of the n
  highest, a stale room counted as fully open) that no aggregation of 5.3 expresses: the runtime's
  aggregations stay the simple, shared ones, and a type that needs more reads the datapoints one by
  one. The compiled bindings take two bytes per bound datapoint, so the higher limit costs nothing
  where it is not used. *Alternatives:* 32 single ports `valve1` .. `valve32` (beyond the 24 ports of a
  type, and an instance would show 32 ports for a house of six rooms); new aggregations `topMean`
  and a stale policy in the runtime (rules of one type moved into the contract of every type).
- **B21. The sun's position is a service of the controller.** A call may ask `sun()`: the sun's
  azimuth and elevation at the cascade's wall time and the controller's location (5.5), computed by
  the algorithm of 13.2 on the exact trigonometry of 13.1, so the device and E-Home's simulation
  agree to the bit; none while the wall clock is not synchronized or no location is set. The sun
  shading (section 18) needs it. *Alternatives:* azimuth and elevation as inputs from a weather
  station (a second source for what the controller knows from its clock and location; not every
  station has them, KNX stations send whole degrees, and the simulation would need them simulated);
  the location as parameters of each instance (it belongs to the installation, and the image
  carries it already in `settings.location`); a library's `sin` and `atan2` (they differ in the
  last bits between Java, glibc and newlib, and a threshold decision such as "the sun is on the
  facade" could then differ between the device and the simulation).
- **B22. A block does not read its own outputs.** An input may not bind a datapoint that an output
  of the same block binds (`forbidden`). A write commands the datapoint, and the command is an update
  of it (`spec/rules.md` 7.3): the block would read its own write as an event of its input, a
  feedback through the loop guard at best. The sun shading's `manual` input, which detects
  operations of the blind, would take the block's own `upDown` for an operation. *Alternative:* each
  type filters its echoes (every type would need its own rule, and the call cannot see an update's
  origin).

### 1.4 Terms

| Term | Definition |
|---|---|
| block type | A function with a manifest, identified by `type` and `version` (section 3). |
| manifest | The declaration of a type's ports, parameters, state, timers and period. |
| block, instance | One use of a type in the image, with an id, bindings and parameter values (section 4). |
| port | An input or output of a type: one value with a type and a unit. |
| binding | The datapoint(s) an instance attaches to a port. |
| mirror | The datapoint `block/<id>/<x>` the runtime keeps for an output, an aggregated input, an adjustable parameter or the enable switch (section 6). |
| call | One entry of the runtime into the type: `start`, `input`, `timer`, `period`, `parameter`. |
| state record | The numeric fields of an instance's state (section 8). |
| failsafe value | What an output is set to when its instance stops. |

## 2. Overview of the runtime

- **Single-threaded, on the event loop, inside the rules' cascades.** A call is part of a cascade
  (`spec/rules.md` 7.1) like a rule firing: what it writes enqueues derived events, and the loop
  guard counts calls caused by derived events.
- **Bounded.** A call takes bounded time and allocates nothing. A built-in type is held to this by
  review; a loaded one by instruction metering (`doc/blocks-wasm.md`).
- **Isolated.** A fault stops one instance (for a loaded type: every instance of the type, which
  share one module instance); the rest of the controller runs on.

## 3. The block type and its manifest

The manifest is a JSON object; built-in types carry it in the firmware (`GET /api/blocks/types`
returns them), a loaded module as its custom section `hc.manifest`.

| Member | Type | Required | Meaning |
|---|---|---|---|
| `type` | reverse-DNS id: 3..63 characters of `a-z 0-9 . -`, at least two dots | yes | `org.homecontrol.room-controller` |
| `version` | `"<major>.<minor>"` | yes | Semantic: a minor version only adds (ports, parameters, state fields, choices) or widens bounds; anything else is a new major. |
| `label` | label (`spec/rules.md` 2.2) | yes | |
| `description` | label | no | |
| `ports` | array of ports | yes | Inputs and outputs, at most 24. |
| `parameters` | array of parameters | no | At most 32. |
| `state` | array of state fields | no | At most 32. |
| `timers` | array of identifiers | no | At most 4; the ids of the type's own one-shot timers. |
| `period` | integer ms, 1000..86 400 000, or 0 | no, 0 | The interval of `period()` calls; 0: none. |
| `persistEvery` | integer ms, 60 000..86 400 000 | with persistent state | The shortest interval between two writes of the record (section 8). |

Ids of ports, parameters, state fields and timers are identifiers of `spec/rules.md` 2.1. Ports and
parameters share one namespace (they become `block/<id>/<x>`, section 6), which also excludes
`enabled`; state fields and timers have their own.

**A port:**

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | identifier | yes | |
| `direction` | `in` or `out` | yes | |
| `type` | `boolean`, `integer`, `real`, `enumeration` | yes | B6. |
| `label`, `unit` | label; string 1..15 | no | |
| `required` | boolean | no, false | Inputs: the image must bind it. |
| `multiple` | integer 1..32 | no, 1 | Inputs: how many datapoints the image may bind (B20). |
| `aggregations` | array of `mean`, `min`, `max` (numeric) or `any`, `all` (boolean) | with `multiple` > 1 | Inputs: what the image may choose; the first is the default. |
| `trigger` | `change`, `update`, `none` | no, `change` | Inputs: which updates call `input(port)` (5.2). |
| `failsafe` | number of the port's type | outputs, unless `diagnostic` | The value written when the instance stops (7). |
| `diagnostic` | boolean | no, false | Outputs: a value for diagnostics and charts, without failsafe. |
| `min`, `max` | numbers | no | Outputs: the range a value must lie in (a write outside is a fault). |

**A parameter** is a rule parameter of `spec/rules.md` 8.1 with these differences: `type` is
`boolean`, `integer`, `real`, `duration`, `timeOfDay` or `enumeration`; `default` is required;
`choices` are `{"value": n, "id": identifier, "label": label}` for `integer` and `enumeration`,
and the image may name a choice by its `id` or its `value`; `adjustable` (default false) says
whether the inhabitant may change it (section 8.2). A `timeOfDay` is written in the image as
`{"hour", "minute", "second"}` and reaches the block as seconds since midnight.

**A state field:** `id`, `persist` (boolean, default false) and `initial` (number, default 0).

### 3.1 The calls

The interface of a type, the same for a built-in C++ type and for a loaded module (whose exports are
`hc_start(self, reason, restored)`, `hc_input(self, port)`, `hc_timer(self, timer)`,
`hc_period(self)`, `hc_parameter(self, parameter)` with the indices of the manifest's order):

| Call | When (section 5) |
|---|---|
| `start(reason, restored)` | The instance starts: `reason` is `boot` (0), `image` (1), `enable` (2) or `restart` (3); `restored` whether its state record came from before (8.1). |
| `input(port)` | An update of an input port's binding, per the port's `trigger`. |
| `timer(timer)` | One of the instance's timers expired. |
| `period()` | The type's period elapsed. |
| `parameter(parameter)` | An adjustable parameter changed (B8). |

What a call may use (the imports of a loaded module, the `BlockContext` of a built-in one):

| Service | Meaning |
|---|---|
| `now()` | The cascade time on the monotonic clock in ms (`spec/rules.md` 7.1): control uses differences of it. |
| `wall()` | The local wall time (date, time, day of the week 1..7) or none while the clock is not synchronized (calendars, `spec/rules.md` D9). |
| `sun()` | The sun's azimuth (degrees from north through east, 0..360) and elevation (degrees above the horizon, -90..90) at the instant of `wall()`, or none while the clock is not synchronized or the controller has no location (B21, 5.5). |
| `read(port)` | An input's value, its quality (`good`, `stale`, `unknown`), the monotonic time of its latest update and whether it is bound (5.3). |
| `count(port)` | The number of datapoints bound to an input, 0 when it is unbound (B20). |
| `readAt(port, k)` | The k-th of them (0-based, in binding order) as an input bound to that datapoint alone reads it (5.3): its value, quality, update time, bound. Outside 0..`count(port)` - 1: not bound, unknown. For an output or an index the type does not declare, `count` is 0. (A loaded module's imports for both follow with the WebAssembly runtime.) |
| `param(parameter)` | A parameter's current value (B6). |
| `write(port, v)` | Writes an output (5.4). |
| `setTimer(timer, ms)`, `cancelTimer(timer)` | Starts (or restarts) a timer 1 ms .. 30 d after the cascade time; stops it. |
| `state` | The instance's state record, read and written in place (8). |
| `log(text)` | A log line of the block (code `log`, level info). |
| `fault(reason)` | Stops the instance (7). |

A type may also check the parameter values at load (B17): `check(parameters)` returns a refusal
reason or none.

## 4. The instance in the image

The image carries `"blocks": [ ... ]`; the order is the *block order*. Members not defined here are
refused (B1), except `description` and `comment`.

```json
"blocks": [
  {"id": "wz-heating", "type": "org.homecontrol.room-controller", "version": "1.0",
   "label": {"en": "Living room heating", "de": "Heizung Wohnzimmer"},
   "inputs": {"temperature": ["1/3/0", "1/3/1"], "window": "wz.window", "mode": "1/3/5"},
   "aggregation": {"temperature": "min"},
   "outputs": {"valve": "1/3/30", "state": "wz.heating.state"},
   "parameters": {"system": "radiator", "comfort": 21.0}}
]
```

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | 1..32 characters of `A-Z a-z 0-9 _ -`, unique among the blocks | yes | Stable identity assigned by E-Home: the key of state, overrides and mirrors. |
| `type` | a type id | yes | |
| `version` | `"<major>.<minor>"` | yes | B16. |
| `label` | label | yes | |
| `enabled` | boolean | no, true | The default of the enable switch (section 6.1). |
| `enabledAdjustable` | boolean | no, true | Whether the inhabitant may switch it. |
| `inputs` | object port id → datapoint name, or array of 1..`multiple` names | no | Every `required` input must be bound. |
| `aggregation` | object port id → one of the port's `aggregations` | no | Only for ports bound to more than one datapoint; default the port's first aggregation. |
| `outputs` | object port id → datapoint name | no | An unbound output has only its mirror. |
| `parameters` | object parameter id → value | no | The instance's defaults (B15); a choice by its `id` or `value`; a duration in ms; a time of day as `{"hour", "minute", "second"}`. |

## 5. Semantics

### 5.1 Calls and cascades

The rules' event processing (`spec/rules.md` 7.2) gains a third step: after the rules, for each
instance in block order (running or not) and each of its input ports in manifest order whose binding
contains the updated datapoint, the runtime recomputes the port's value (5.3) and, when the instance
runs (it has been started and is neither disabled nor faulted) and the port's `trigger` matches,
calls `input(port)`:

- `change`: the port's value or quality differs from its value at its previous recomputation (at
  the load, then at every update of its binding; the model as it is now, not the event's old value);
- `update`: every update of a bound datapoint;
- `none`: never; the block reads the input when it is called otherwise.

An aggregated port's mirror (6) is published right after the recomputation, before the call, when
its value or quality changed.

Every call made for a derived event counts as one derived firing of the cascade
(`spec/rules.md` 7.2): the 33rd is refused with `loopGuard`, which names the block refused and the
culprit, the rule or block with the most derived firings or calls in the cascade (the first in rule
order, then block order, on a tie). Calls for an external event, timers, periods, starts and
parameter changes from the UI or API are not derived.

Timer expiries and periods are external events, ordered with the rule timers by deadline (B7: a
rule timer or silence first at an equal deadline; among blocks, block order; within an instance,
its timers in manifest order before its period). Each is its own cascade. A timer is one-shot;
`setTimer` of a running timer restarts it. The period's deadlines lie on a grid from the instance's
start, `start + k × period`; a period that passed while the loop was busy is called once, and the
next deadline is the next grid point after the call.

Parameter changes from the UI, the API or the parameter's datapoint are external events (a command
from a rule's `write` is a derived one). A change to the current value changes nothing. Otherwise the
parameter takes its value at once (and is persisted, 8.2), and a parameter event is queued; it
publishes the mirror (after an API change; a command has updated it already, origin `command`) and
then calls `parameter(id)` if the instance runs. So the queue holds the mirror's update before what
the call writes. The enable switch is changed the same way (6.1).

### 5.2 Starting

A block *starts* at boot, at every image swap (every block of the new image), when it is enabled and
at a restart after a fault. At boot and at a swap, after the rules' start cascades
(`spec/rules.md` 7.5), each block in block order gets a cascade of its own:

1. If it is disabled: it writes its failsafe values (B10) and is not called.
2. Otherwise the runtime sets up its period grid from the cascade time and calls
   `start(reason, restored)`.

Enabling starts the block with the reason `enable` in the cascade of the change; a restart (API)
with the reason `restart` in a cascade of its own.

### 5.3 Inputs

A bound input reads its datapoints. A single datapoint gives its value converted to a number (B6)
and its quality and update time. An aggregated port combines its datapoints, in binding order:

- over those with quality `good`; if none, over those with quality `stale` (the result is stale);
  if all are unknown, the input is unknown;
- `mean`: the sum, added left to right, divided by the count; `min`, `max`; `any`: true if one is
  true; `all`: true if all are;
- the update time is the latest of the datapoints combined.

`readAt(port, k)` gives one datapoint of the binding as it is, whatever its quality, without the
aggregation (B20).

Port types accept datapoint types: `real` from `real`, `unsigned`, `signed`; `integer` from
`unsigned`, `signed`; `enumeration` from `enumeration`, `unsigned`; `boolean` from `boolean`.
Another combination refuses the image (`typeError`). An unbound input reads as not bound, unknown.

### 5.4 Outputs

`write(port, v)`:

1. A value that is not finite, outside the port's `min`/`max`, not 0 or 1 for a boolean, or not
   integral for an integer or enumeration (or outside 0..2^32-1 for an enumeration) is a fault
   (7); nothing is written.
2. If the port is bound: the value is converted to the datapoint's type (`real` as is; to
   `unsigned` or `signed` rounded half away from zero, refused outside the type's range; boolean;
   enumeration) and the datapoint is commanded. The output types command these datapoint types,
   any other refuses the image (`typeError`): `real` and `integer` command `real`, `unsigned`,
   `signed`; `enumeration` commands `enumeration`, `unsigned`; `boolean` commands `boolean`. The datapoint is commanded (`DatapointModel::command()`), like a rule's write. A
   refusal of the technology logs `writeRefused` and is no fault. While the technology is not
   connected (the KNX tunnel is down, the Zigbee network is not up) the write is held and sent
   once it connects (spec/rules.md 7.3 step 5): an output written once at the start (the room
   controller's `switch`, the heating demand's `request`, which write only on a change) reaches
   the bus after a boot or a swap during an outage.
3. The output's mirror `block/<id>/<port>` is published (origin `publish`) when its value differs
   or its quality is not good.

Both updates are derived events of the cascade, the bound datapoint's first. Writes after a fault
in the same call are ignored.

### 5.5 Time

`now()` is the cascade time, the same for every call of a cascade; a control law integrates over the
difference to the time of its previous step (B6: monotonic, never the wall clock). `wall()` gives
the local time from the time service, or none while it is not synchronized.

`sun()` (B21) is the sun's position at the cascade's wall time: the UTC instant `u` of the wall
clock that `wall()` shows as local time, in whole seconds (the milliseconds dropped, as `wall()`
drops them; in the repeated hour at the end of summer time it is the clock's own instant, which
`wall()`'s summer-time flag tells apart), and the controller's location, by the algorithm of 13.2.
The location is the image's `settings.location` (`spec/runtime-image.md` 3.2) when the image has
one, which replaces the device's own setting at the load (`doc/time.md`), otherwise the device's
setting; a new location applies from the next cascade on and is no event. `sun()` is the same for
every call of a cascade. None while `wall()` is none or no location is set. The
elevation is geometric (no refraction): near the horizon the sun appears up to 0.6° higher (0.1°
at 10°).

### 5.6 Values the image sets (quiet at load)

The mirrors of the parameters and the enable switch take their values at a load (boot or swap)
without an event, as the rule parameters' datapoints do (`spec/rules.md` 7.6); so do the mirrors of
aggregated inputs, which follow from the kept values of their datapoints. Later changes are events.
Outputs are written by the block's calls, also at its start: those are events.

## 6. Datapoints of a block

The loader derives, for every instance in block order, behind the image's own datapoints, the time
master's, the rule parameters' and the schedules' (`spec/runtime-image.md` 3.3):

| Datapoint | Type | Flags | Value |
|---|---|---|---|
| `block/<id>/enabled` | boolean | writable and transmitting when `enabledAdjustable` | The enable switch. |
| `block/<id>/<parameter>`, each adjustable parameter in manifest order | per 8.6 of `spec/rules.md`: boolean, signed (integer), real, unsigned (duration ms), timeOfDay, enumeration | writable, transmitting | The parameter's current value. |
| `block/<id>/<output>`, each output in manifest order | boolean, signed (integer), real, enumeration | none | The last value written. |
| `block/<id>/<input>`, each input bound to more than one datapoint | as the output types | none | The aggregated value (quality as 5.3). |

The block runtime owns them (technology `blocks`). A command to the enable switch or a parameter's
datapoint is a change as by the API (6.1, 8.2), refused with `parameterRefused` when invalid (the
datapoint keeps its value). They are not persisted by the datapoint model and have no history; a
chart binds an output to a declared datapoint with a history instead. The prefix `block/` is
reserved: the image declares no such datapoint. Rules, schedules, functions, pages and other blocks'
inputs may name them; a block's input may not bind its own (`forbidden`).

### 6.1 The enable switch

Every instance has the boolean parameter `enabled`, default the image's `enabled`, adjustable
unless `enabledAdjustable` is false, persisted like a parameter override (8.2). Disabling stops the
instance: its timers and period stop, its failsafe values are written once (B10), its status is
`disabled`; its state record stays. Enabling starts it (`enable`, restored: the record in RAM). The
switch of a faulted instance changes only the switch: the fault ends with a restart or a new image,
which then find the instance disabled (status `disabled`, nothing written) or enabled (started).

## 7. Faults

A fault is a `fault(reason)` of the type, an invalid write (5.4), a non-finite state field after a
call (B11), a timer outside 1 ms .. 30 d, and for a loaded type a trap or an exhausted budget. It
stops the instance:

1. the rest of the call is ignored, its timers and period stop;
2. every output with a failsafe value writes it (5.4);
3. the status becomes `fault` with the reason and the time; `fault` is logged (error);
4. its record is not persisted any more.

The instance stays stopped until the next image or a restart (`PUT /api/blocks/<id>`,
`{"restart": true}`), which starts it with the reason `restart` from its persisted record (B11).
The rest of the controller runs on. A loaded type's instances share one module instance: a trap
stops all of them (`doc/blocks-wasm.md`).

## 8. State, parameters and persistence

### 8.1 The state record

A record holds the type's state fields as doubles, by manifest order, initialized to `initial`. The
runtime keeps it in RAM, shows it in the diagnostics and persists the fields with `persist`:

- **Image swap:** an instance whose id, type and major version are unchanged keeps its record in RAM
  (all fields) and starts with `restored`; fields a new minor version adds start at their initial
  value; a faulted one starts from its persisted record instead, as a restart. Any other instance
  starts fresh (`restored` false); a changed type or major version of a kept id logs `stateReset` and
  deletes its persisted record. Every instance's timers and period stop at a swap: its `start` call
  sets what it needs. Instances no longer in the image lose their record and overrides, without
  failsafe writes.
- **Boot and restart:** the persisted record is read; its persisted fields are restored by id,
  every other field starts at its initial value, and the call is `start(boot|restart, true)`;
  without a record, `start(boot|restart, false)` with every field initial. A record of another type
  or major version, or one that does not read, logs `stateReset`, is deleted, and the instance
  starts fresh.
- **Writing:** after a call that changed a persisted field the record is due; the first write after
  a boot happens at once, every later one when `max(persistEvery, n × 120 s)` (B13; `n` the instances
  whose type persists) have passed since the previous, and at an orderly reboot (`flush`). A record
  equal to the stored one is not written.

The record's format (frozen, `doc/blocks.md`): blob `b.<8 lower-case hex digits of the 32-bit
FNV-1a hash of the block id>` in the BlobStore: `HCBS`, format 1, the type id, its major version,
the block id, the count of persisted fields, each as its id and an IEEE 754 double (little-endian),
and a CRC-32.

### 8.2 Parameters

A parameter's value is the image's (B15), or the manifest's default when the image gives none, or
the inhabitant's override. The image's value must be valid (type, `min`..`max`, one of the
`choices`) or the image is refused (`invalidParameter`). A change (the API, a command to the
parameter's datapoint) is refused with `parameterRefused` unless the parameter is adjustable and the
value valid; a value equal to the default removes the override, any other value becomes the
override, persisted under the logical key `@<blockId>/<parameterId>` in the store of the rule
parameters (`spec/rules.md` 8.4; `@` cannot begin a rule id, so the key spaces do not meet).

At a load (swap or boot) each override is checked against the new manifest and image (B4): kept
when the parameter still exists, is adjustable, has the same type and the value is valid; otherwise
it is deleted, the parameter takes the default, and `parameterReset` is logged naming why
(`gone`, `not adjustable`, `type changed`, `out of bounds`); unlike a rule's (`spec/rules.md` 8.4),
the override of a parameter that is gone is logged too (B4). The entries come per instance in block
order, `stateReset` first. Overrides of instances no longer in the image are deleted silently.

## 9. Diagnostics

Per instance (`GET /api/blocks`, the UI's "Blocks" section, E-Home's simulation view): the status
(`running`, `disabled`, `fault`), the fault's reason and time, the number of calls since boot, the
last call (kind and time), the time used by calls (total and longest, ms), the number of logged
problems with the last one (code, text, time), the state record, the inputs with value, quality and
bindings, the outputs with their last value and binding, and the parameters as for rules
(`spec/api.md`).

The runtime logs with the tag `hc.block`; every entry names the block id. Codes (the corpus compares
them):

| Code | Level | When |
|---|---|---|
| `log` | info | a `log` of the block |
| `fault` | error | the instance faulted (7) |
| `writeRefused` | error | the bound datapoint refused a command (5.4) |
| `timerRefused` | error | a timer outside 1 ms .. 30 d (also a fault) |
| `parameterRefused` | warning | a parameter change refused (8.2) |
| `parameterReset` | warning | an override dropped by a new image (8.2) |
| `stateReset` | warning | a state record not kept (8.1) |
| `loopGuard` | error | a call refused by the loop guard (5.1) |

## 10. Trust

Anybody may write a block. A built-in type is firmware: it is reviewed and signed with the
firmware. A loaded module runs in the WebAssembly sandbox with instruction metering
(`doc/blocks-wasm.md`): it can touch only its linear memory and the imports above. The image
references a module by its SHA-256 hash, and E-Home signs the image (`spec/api.md` "Signed runtime
images"), so a module is covered by the installation's signature without a vendor signature. (Loaded
modules are not part of schema 1.3; their member follows with the WebAssembly runtime.)

## 11. Loading: checks and limits

The loader checks every instance after the datapoints, the schedules and the rules; the first
failure of an instance refuses the image with the path and one of the reasons of `spec/rules.md`
10.1, plus `unknownType` and `missingInput`:

| Reason | Examples, with their paths |
|---|---|
| `malformed` | wrong JSON type, an unknown member (with the hint of I4), a malformed id, label or version (`blocks[0].version`), an unknown aggregation name, a datapoint listed twice in one input (`blocks[0].inputs.valves[3]`) |
| `duplicateId` | two blocks with the same id (`blocks[1].id`) |
| `unknownType` | a type the firmware does not have, or not in that major version or minor version (`blocks[2].type`: `unknownType: org.homecontrol.room-controller 1.3 (this firmware has 1.0)`) |
| `unknownReference` | a port or parameter the type does not declare (`blocks[0].inputs.temprature`), a datapoint that does not exist (`blocks[0].inputs.temperature[1]`) |
| `missingInput` | a required input is not bound (`blocks[2].inputs.temperature`) |
| `typeError` | a port's type does not accept the datapoint's type (5.3, 5.4), a parameter value of the wrong type (a string for a number, a fraction for an integer or duration, a number outside the type's range) |
| `invalidParameter` | a parameter value outside its bounds or not a choice (`blocks[0].parameters.comfort`), a cross-parameter check of the type (B17: the path names the parameter the type blames, `blocks[0].parameters.outputMin`) |
| `notWritable` | an output bound to a datapoint the application may not command |
| `outputConflict` | an output bound to a datapoint another output binds, a schedule entry's output, the time master's or an implied datapoint (B14) |
| `forbidden` | an input bound to a mirror of its own block; an output bound to a datapoint an input of the same block binds (B22, `blocks[0].outputs.upDown`; checked after the output's datapoint is found, owned by nobody else (`outputConflict`), writable and of an accepted type, and before another output's binding of it); an aggregation for a port bound to one datapoint, or one the port does not allow; more datapoints than `multiple`; a port's direction wrong for its member (`outputs` naming an input) |
| `limitExceeded` | more than 32 blocks; persistence keys that collide in the device's storage |

| Limit | Value |
|---|---|
| Blocks per image | 32 |
| Datapoints per input | 32 (`multiple`, B20) |
| Ports / parameters / state fields / timers per type | 24 / 32 / 32 / 4 |
| Timer duration | 1 ms .. 30 d |
| Persisted record | 512 bytes |
| Record writes | 30 per hour in all (B13) |

## 12. API and UI

`GET /api/blocks`, `GET /api/blocks/types` and `PUT /api/blocks/<id>` are specified in
`spec/api.md`. The device UI shows a "Blocks" section on its System page (status, last error,
ports, state; the enable switch and the restart of a faulted block); outputs and adjustable
parameters reach the control pages as datapoints (`spec/runtime-image.md` 3.9).

## 13. Determinism (for every implementation)

The corpus pins the exact values. To give the same results, an implementation:

- evaluates the formulas of sections 13.1, 13.2 and 14 to 18 in IEEE 754 binary64 with
  round-to-nearest-even, in the order written (left to right, the parentheses as given), without
  fused multiply-add and without extended precision;
- uses `floor(x)`, `|x|`, `sqrt(x)`, `min`, `max` and comparisons only (no `exp`, `pow`, `log`, no
  library `sin` or `atan2`: the functions of 13.1 instead), with `min(a, b) = (b < a) ? b : a` and
  `max(a, b) = (a < b) ? b : a` (C++'s `std::min`/`std::max`: on a tie, also of 0 and -0, the first
  argument);
- writes a constant such as `1.0 / 6.0` as that division, evaluated once in binary64, and a decimal
  constant (`0.016708634`) as the binary64 value nearest to it (the literal of both languages);
- takes times as integer milliseconds and converts them with `ms / 1000.0` where seconds are
  written;
- orders calls, writes and log entries as section 5 says.

### 13.1 Exact trigonometry

Angles are degrees. The functions use only the operations above, so every implementation gives the
same bits; their error against the true functions is below 1e-13 (sine and cosine) and 1e-12 degrees
(the arc functions), far below what the formulas that use them need. `PI` is the binary64 value
nearest to π (`3.141592653589793`), `RAD = PI / 180.0` and `DEG = 180.0 / PI`.

```
wrap360(x) = x - 360.0 * floor(x / 360.0)                               // 0..360

sinDeg(x), cosDeg(x):
    r  = wrap360(x)
    k  = floor(r / 90.0 + 0.5)                                          // the nearest quadrant, 0..4
    z  = (r - 90.0 * k) * RAD                                           // |z| <= π/4, radians
    z2 = z * z
    s  = z + z * z2 * (S3 + z2 * (S5 + z2 * (S7 + z2 * (S9 + z2 * (S11 + z2 * S13)))))
    c  = 1.0 + z2 * (C2 + z2 * (C4 + z2 * (C6 + z2 * (C8 + z2 * (C10 + z2 * (C12 + z2 * C14))))))
    k = 0 or 4:  sinDeg = s,   cosDeg = c
    k = 1:       sinDeg = c,   cosDeg = -s
    k = 2:       sinDeg = -s,  cosDeg = -c
    k = 3:       sinDeg = -c,  cosDeg = s

atan2Deg(y, x):                                                         // -180..180
    if x == 0 and y == 0:  return 0
    ax = |x|;  ay = |y|
    swap = ay > ax
    t  = swap ? ax / ay : ay / ax                                       // 0..1
    t  = t / (1.0 + sqrt(1.0 + t * t))                                  // atan(t) = 2 atan(this), <= tan 22.5°
    t  = t / (1.0 + sqrt(1.0 + t * t))                                  // once more, <= tan 11.25°
    t2 = t * t
    a  = t + t * t2 * (A3 + t2 * (A5 + t2 * (A7 + t2 * (A9 + t2 * (A11 + t2 * (A13 + t2 * (A15 + t2 * A17)))))))
    d  = 4.0 * a * DEG
    if swap:   d = 90.0 - d
    if x < 0:  d = 180.0 - d
    if y < 0:  d = -d
    return d

asinDeg(v):  w = min(max(v, -1.0), 1.0);  return atan2Deg(w, sqrt((1.0 - w) * (1.0 + w)))
```

The Taylor coefficients, each written as the division and evaluated once: `S3 = -1.0 / 6.0`,
`S5 = 1.0 / 120.0`, `S7 = -1.0 / 5040.0`, `S9 = 1.0 / 362880.0`, `S11 = -1.0 / 39916800.0`,
`S13 = 1.0 / 6227020800.0`; `C2 = -1.0 / 2.0`, `C4 = 1.0 / 24.0`, `C6 = -1.0 / 720.0`,
`C8 = 1.0 / 40320.0`, `C10 = -1.0 / 3628800.0`, `C12 = 1.0 / 479001600.0`,
`C14 = -1.0 / 87178291200.0`; `A3 = -1.0 / 3.0`, `A5 = 1.0 / 5.0`, `A7 = -1.0 / 7.0`,
`A9 = 1.0 / 9.0`, `A11 = -1.0 / 11.0`, `A13 = 1.0 / 13.0`, `A15 = -1.0 / 15.0`, `A17 = 1.0 / 17.0`.
`ax / ay` with `swap` and the halvings keep the series' argument at most tan 11.25° (0.199), where
its first omitted term is below 3e-15 radians; the sine's and cosine's below 2e-14 at π/4. With the
rounding of the steps the functions are within 1e-13 (sine, cosine) and 1e-12 degrees (the arc
functions) of the true values, so they are not exact at exact angles (`atan2Deg(1, 1)` is not
exactly 45): no decision of a type or a corpus case may rest on one. `wrap360` may give exactly 360
for an argument just below 0, and `atan2Deg(0, -0.0)` is 0 (the first line); nothing corrects
either, the values are used as computed.

### 13.2 The sun's position

`sun()` (B21) at the UTC instant `u` (integer seconds since 1970-01-01) and the location `lat`,
`lon` (degrees, north and east positive) follows the NOAA solar calculator (Meeus, *Astronomical
Algorithms*, the low-precision solar coordinates; the algorithm of `doc/time.md` for the sun's
events, evaluated at the instant itself):

```
d   = (u - 946728000) / 86400.0                                         // days since J2000.0, 2000-01-01 12:00 UTC
T   = d / 36525.0                                                       // Julian centuries
L0  = wrap360(280.46646 + T * (36000.76983 + T * 0.0003032))            // mean longitude
M   = 357.52911 + T * (35999.05029 - 0.0001537 * T)                     // mean anomaly
e   = 0.016708634 - T * (0.000042037 + 0.0000001267 * T)                // eccentricity
C   = sinDeg(M) * (1.914602 - T * (0.004817 + 0.000014 * T)) + sinDeg(2.0 * M) * (0.019993 - 0.000101 * T)
      + sinDeg(3.0 * M) * 0.000289                                      // equation of the centre
Om  = 125.04 - 1934.136 * T
lam = L0 + C - 0.00569 - 0.00478 * sinDeg(Om)                           // apparent longitude
eps = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0
      + 0.00256 * cosDeg(Om)                                            // obliquity
dec = asinDeg(sinDeg(eps) * sinDeg(lam))                                // declination
y   = sinDeg(eps / 2.0) / cosDeg(eps / 2.0);  y = y * y
E   = 4.0 * (y * sinDeg(2.0 * L0) - 2.0 * e * sinDeg(M) + 4.0 * e * y * sinDeg(M) * cosDeg(2.0 * L0)
      - 0.5 * y * y * sinDeg(4.0 * L0) - 1.25 * e * e * sinDeg(2.0 * M)) * DEG    // equation of time, minutes
s   = u - 86400 * floor(u / 86400)                                      // seconds of the UTC day, 0..86399: integer floor division (also for u < 0)
H   = (s / 60.0 + E + 4.0 * lon) / 4.0 - 180.0                          // hour angle, degrees
up    = sinDeg(lat) * sinDeg(dec) + cosDeg(lat) * cosDeg(dec) * cosDeg(H)
north = cosDeg(lat) * sinDeg(dec) - sinDeg(lat) * cosDeg(dec) * cosDeg(H)
east  = -(cosDeg(dec) * sinDeg(H))
elevation = atan2Deg(up, sqrt(north * north + east * east))
azimuth   = wrap360(atan2Deg(east, north))
```

The sums are written as NOAA writes them and evaluated left to right (`L0 + C - 0.00569 - ...` is
`((L0 + C) - 0.00569) - ...`). `(u - 946728000)` and `s` are integer operations; their results are
converted to binary64 exactly. Accuracy: the low-precision coordinates are within 0.01° of the
sun's true position for the years 1950..2050 (Meeus), within about 0.02° for 1900..2100; the host
tests check the result against the NREL Solar Position Algorithm's published example (within 0.02°
after its refraction and parallax are taken out) and against the Astronomical Almanac's
low-precision formula over the seasons and latitudes (within 0.02°). The elevation is geometric:
refraction lifts the visible sun by up to 0.6° at the horizon, 0.1° at 10°, which a shading
threshold of a few degrees absorbs.

## 14. The room controller `org.homecontrol.room-controller` 1.0

A PI(D) room temperature controller for one heating circuit: a continuous valve (KNX 5.001), a
switching actuator with thermal drives (PWM) or a two-point heater.

### 14.1 Manifest

Period 60 000 ms, `persistEvery` 600 000 ms, timers `window` and `pwm`.

| Input | Type | Unit | | Multiple, aggregations | Trigger | Meaning |
|---|---|---|---|---|---|---|
| `temperature` | real | °C | required | 4: `mean`, `min`, `max` | none | The actual value. |
| `setpoint` | real | °C | | 1 | change | An external base setpoint; replaces the mode's setpoint while good. |
| `mode` | enumeration | | | 1 | change | KNX HVAC mode (DPT 20.102): 1 comfort, 2 standby, 3 night, 4 frost/building protection; 0 (auto) and others: from `presence`/`heatingPeriod`. |
| `presence` | boolean | | | 1 | change | |
| `heatingPeriod` | boolean | | | 1 | change | The output of a schedule entry: false is night. |
| `window` | boolean | | | 4: `any` | change | Open windows. |
| `enable` | boolean | | | 1 | change | False: off (frost protection only). |
| `summer` | boolean | | | 1 | change | True: the valve at `summerPosition`. |
| `forced` | boolean | | | 1 | change | True: the valve at `forcedPosition`. |

| Output | Type | Unit | Failsafe | Meaning |
|---|---|---|---|---|
| `valve` | real 0..100 | % | 30 | The valve position (whole percent). |
| `switch` | boolean | | false | The switching output (PWM, two-point; continuous: valve > 0). |
| `demand` | boolean | | false | The room wants heat. |
| `activeSetpoint` | real | °C | diagnostic | |
| `state` | enumeration | | diagnostic | 0 off, 1 heating, 2 window, 3 frost, 4 boost, 5 fault, 6 protection, 7 summer, 8 forced. |
| `p`, `i`, `d` | real | % | diagnostic | The terms of the last PI(D) step. |
| `error` | real | K | diagnostic | Setpoint minus filtered value. |
| `filtered` | real | °C | diagnostic | The filtered actual value. |

| Parameter | Type | Default | Range | Adjustable | Meaning |
|---|---|---|---|---|---|
| `system` | enumeration | `radiator` (0) | `radiator` 0, `underfloor` 1, `convector` 2, `custom` 3 | | The preset of the gains (14.2). |
| `controlType` | enumeration | `continuous` (0) | `continuous` 0, `pwm` 1, `twoPoint` 2 | | |
| `xp` | real, K | 3 | 0.5..20 | | Proportional band (`custom` only). |
| `tn` | real, min | 90 | 1..600 | | Reset time (`custom` only). |
| `td` | real, min | 0 | 0..60 | | Derivative time, 0: PI (`custom` only). |
| `filter` | real, min | 2 | 0..60 | | Time constant of the actual value's filter (`custom` only). |
| `cycle` | duration | 900 000 | 300 000..3 600 000 | | PWM cycle. |
| `hysteresis` | real, K | 0.5 | 0.1..5 | | Two-point. |
| `outputMin`, `outputMax` | real, % | 0, 100 | 0..100 | | The valve's range while regulating; `outputMin` < `outputMax` (B17). |
| `minChange` | real, % | 2 | 0..50 | | The smallest change of `valve` that is sent. |
| `resendInterval` | duration | 0 | 0..86 400 000 | | Resend `valve` after this time; 0: never. |
| `comfort`, `standby`, `night` | real, °C | 21, 19, 17 | 5..30 | yes | Setpoints. |
| `frost` | real, °C | 7 | 4..15 | yes | Frost protection setpoint. |
| `windowDelay` | duration | 60 000 | 0..3 600 000 | | How long a window is open before it counts. |
| `boostThreshold` | real, K | 0 | 0..5 | | A setpoint step of at least this error boosts; 0: no boost. |
| `boostDuration` | duration | 1 800 000 | 60 000..10 800 000 | | Longest boost. |
| `sensorTimeout` | duration | 1 800 000 | 0..86 400 000 | | No update for this long is a sensor fault; 0: never. |
| `fallbackPosition` | real, % | 30 | 0..100 | | The valve while the sensor fails. |
| `forcedPosition` | real, % | 100 | 0..100 | | The valve while `forced`. |
| `summerPosition` | real, % | 0 | 0..100 | | The valve in summer. |
| `protectionDay` | integer | 0 | 0..7 | | Valve protection on this weekday (1 Monday .. 7 Sunday); 0: never. |
| `protectionTime` | timeOfDay | 10:00:00 | | | From when on that day. |
| `protectionDuration` | duration | 300 000 | 60 000..1 800 000 | | How long the valve is opened fully. |

State: `integral` and `protectedOn` (the day of the last protection run) persisted, and not persisted `filtered`, `hasFiltered`, `lastStep`, `lastSetpoint`,
`hasLastSetpoint`, `lastKp`, `hasLastKp`, `windowActive`, `boostUntil`, `protectionUntil`, `cycleStart`, `hasCycle`, `lastState`, `sentValve`, `hasSent`,
`sentAt`, `switchOn`, `demandOn`.

### 14.2 Presets

| `system` | `xp` K | `tn` min | `td` min | `filter` min | Tested on (host, 14.6) | Band |
|---|---|---|---|---|---|---|
| `radiator` | 3 | 90 | 5 | 2 | τ 2 h, dead time 10 min, gain 30 K | ±0.2 K within 2 h |
| `underfloor` | 3 | 180 | 10 | 2 | τ 8 h, dead time 45 min, gain 20 K | ±0.2 K within 8 h |
| `convector` | 2 | 45 | 5 | 2 | τ 1 h, dead time 3 min, gain 25 K | ±0.2 K within 1 h |
| `custom` | the parameters `xp`, `tn`, `td`, `filter` | | | | | |

### 14.3 The calls

| Call | What it does |
|---|---|
| `start(reason, restored)` | Without `restored`, every field at its initial value. Then: `lastStep = now`, `hasLastSetpoint = 0`, `hasCycle = 0`, `hasSent = 0`, `boostUntil = 0`, and `hasFiltered = 0` unless the reason is `image`; `windowActive` = 1 if the window input is good and true, else 0 (no delay), the timer `window` cancelled; then the step. |
| `input(window)` | Open (good and true) while `windowActive` is 0: with `windowDelay` 0 `windowActive = 1`, otherwise `setTimer(window, windowDelay)` (unless it runs). Closed or not good: `windowActive = 0`, `cancelTimer(window)`. Then the step. |
| `timer(window)` | `windowActive = 1` if the window is good and true; then the step. |
| `timer(pwm)` | `switchOn = 0`, write `switch` false. No step. |
| every other `input`, `period()`, `parameter` | The step. |

### 14.4 The step

With `t` = now (ms) and the parameters of the preset (14.2) or, for `custom`, `xp`, `tn`, `td`,
`filter`:

```
dt   = (t - lastStep) / 1000.0;  lastStep = t                      // seconds, >= 0
Kp   = 100.0 / xp;  Ti = tn * 60.0;  Td = td * 60.0;  Tf = filter * 60.0
T    = read(temperature)
ok   = T.quality == good and (sensorTimeout == 0 or t - T.updated <= sensorTimeout)
if ok:
    x = T.value
    if hasFiltered == 0 or Tf == 0:  filtered = x;  hasFiltered = 1
    else:                             filtered = filtered + (x - filtered) * (dt / (Tf + dt))
(sp, regState) = the setpoint and state of 14.5
w = wall()                                                          // valve protection
if protectionDay > 0 and w is known and w.dayOfWeek == protectionDay
        and w.hour * 3600 + w.minute * 60 + w.second >= protectionTime and day(w) != protectedOn:
    protectionUntil = t + protectionDuration;  protectedOn = day(w)
P = 0;  D = 0
if forced is good and true:          state = forced;     u = forcedPosition;   boostUntil = 0
else if t < protectionUntil:         state = protection; u = 100;              boostUntil = 0
else if summer is good and true:     state = summer;     u = summerPosition;   boostUntil = 0
else if not ok:                      state = fault;      u = fallbackPosition; boostUntil = 0
else:                                state = regState;   u = regulate()                    // 14.6
lastSetpoint = sp;  hasLastSetpoint = 1
valve = floor(u + 0.5)
```

`day(w)` is the number of days since 1970-01-01 of the wall date. Then the outputs, in this order,
with `first = (hasSent == 0)`:

1. `valve`, when `first`; or `valve != sentValve` and (`|valve - sentValve| >= minChange`, or `valve`
   is `outputMin`, `outputMax`, 0 or 100); or `resendInterval > 0` and
   `t - sentAt >= resendInterval`. Then `sentValve = valve`, `sentAt = t`, `hasSent = 1`.
2. `switch` = `on`: for `pwm` per 14.6; for `twoPoint` in a regulating state (`heating`, `window`,
   `off`, `frost`) the two-point output; otherwise `valve > 0`. Written when `first` or
   `on != switchOn`; `switchOn = on`.
3. `demand` = `valve > 0` and the state is none of `forced`, `protection`, `summer`. Written when
   `first` or it differs from `demandOn`; `demandOn` follows.
4. `activeSetpoint` = `sp`, `state`, `p` = `P`, `i` = `integral`, `d` = `D`,
   `error` = `sp - filtered` when `hasFiltered`, else 0, `filtered` = `filtered`: written at every
   step (their mirrors change only when the values do, B9).
5. `lastState = state`.

### 14.5 Setpoint and modes

The first that applies:

1. `windowActive` is 1: `frost`, state `window`;
2. `enable` good and false: `frost`, state `off`;
3. `mode` good and 4: `frost`, state `frost`;
4. `setpoint` good: its value, state `heating`;
5. `mode` good and 1, 2 or 3: `comfort`, `standby`, `night`, state `heating`;
6. otherwise `heatingPeriod` good and false: `night`; else `presence` good and false: `standby`;
   else `comfort`; state `heating`.

### 14.6 Regulating

`twoPoint`: with `h = hysteresis / 2.0`: `on = 1` if `filtered <= sp - h`, `on = 0` if
`filtered >= sp + h`, otherwise `on = switchOn`; `u = outputMax` when on, else `outputMin`. No boost
(`boostUntil = 0`), the integral frozen.

`continuous` and `pwm`, the PI(D) step, with `e = sp - filtered`:

```
if boostThreshold > 0 and hasLastSetpoint == 1 and sp > lastSetpoint and e >= boostThreshold:
    boostUntil = t + boostDuration
if boostUntil > t and e >= boostThreshold / 2.0:
    state = boost;  u = outputMax                                   // the integral frozen
else:
    boostUntil = 0
    if hasLastKp == 1 and Kp != lastKp:  integral = integral + (lastKp - Kp) * e   // bumpless
    lastKp = Kp;  hasLastKp = 1
    P = Kp * e
    if Td > 0 and Tf > 0:  D = -(Kp * Td * (x - filtered) / Tf)     // on the filtered value
    v = P + integral + D
    if not ((v >= outputMax and e > 0) or (v <= outputMin and e < 0)):     // conditional integration
        integral = integral + Kp * (dt / Ti) * e
    integral = min(max(integral, outputMin), outputMax)
    u = min(max(P + integral + D, outputMin), outputMax)
```

`(x - filtered) / Tf` is the derivative of the filtered value (`(filtered_new - filtered_old) / dt`
of the filter's backward-Euler step), so the D term does not depend on the step's length and a
setpoint step causes no derivative kick. The integral is never reset. It is frozen in the states
`forced`, `protection`, `summer`, `fault` and `boost` and with `twoPoint`, and while the output is
saturated in the direction of the error (conditional integration). In `window`, `off` and `frost`
the controller regulates to the frost setpoint: with the room above it the output saturates at
`outputMin` and the integral stays where it was, so a window opening winds nothing up. A change of
`xp` (or of the preset, with a new image: `lastKp` survives a swap) is absorbed by the integral so
that the output does not jump (bumpless); a change of `tn` changes only the rate from then on;
returning to regulation continues from the integral as it is. The proportional term acts on the
error, so a setpoint step moves the valve at once.

PWM (`controlType` `pwm`, in every state): a cycle starts at a step when `hasCycle` is 0, when
`t - cycleStart >= cycle`, or when the state differs from `lastState`: `cycleStart = t`,
`hasCycle = 1`, the on-time `(cycle * valve) / 100.0` ms; `on = 1` when it is above 0, and when it is
also below `cycle` the timer `pwm` is set to `floor(on-time)`, otherwise cancelled; `on = 0` (and the
timer cancelled) with an on-time of 0. Between cycle starts `on = switchOn`. Over a cycle the
switch's duty equals `valve`. (Times are compared only as differences: an implementation may count
the monotonic clock from any origin.)

### 14.7 Tests

The host tests (`core/test/room_controller_test.cpp`) drive the controller with a simulated room of
first order plus dead time per preset (14.2; outside 5 °C, a sensor resolution of 0.01 K): the 2 K
setpoint step settles within the band with an overshoot of at most 0.3 K; a 2 h window opening winds
nothing up; a stale or silent sensor leads to the fallback after `sensorTimeout`; doubling every
report gives the same bits and reporting three times as often moves the trajectory by less than
0.1 K; the PWM's on-time over a cycle equals the continuous output; the two-point output respects
its hysteresis; boost and valve protection; the integral survives a restart. The corpus has the
contract cases of these behaviors (`spec/fixtures/blocks/README.md`).

## 15. The conformance type `org.homecontrol.conformance` (test harnesses only, B18)

Versions 1.1 and 2.0, the same behavior. Period 10 000 ms, `persistEvery` 60 000 ms, timer `t`.

| Port | Direction, type | Details |
|---|---|---|
| `x` | in, real | required, multiple 4: `mean`, `min`, `max`; trigger change |
| `arm` | in, boolean | trigger change |
| `tick` | in, boolean | trigger update |
| `y` | out, real | failsafe -1 |
| `pulse` | out, boolean | failsafe false |
| `calls` | out, integer | diagnostic |
| `ticks` | out, integer | diagnostic |

| Parameter | 1.1 | 2.0 |
|---|---|---|
| `gain` | real, default 1, 0..10, adjustable | real, default 1, 0..5, adjustable |
| `offset` | real, default 0, -100..100 | the same |
| `faultAbove` | real, default 1000 | the same |
| `delay` | duration, default 5000, 100..60 000, adjustable | (none) |
| `level` | — | integer, default 0, 0..9, adjustable |

State: `count` (persisted), `ticks` (persisted), `pulseOn`.

Every call first adds 1 to `count`. Then:

- `start`, `input(x)`, `parameter`: *update*;
- `input(arm)`: when `arm` is good and true `setTimer(t, delay)` (2.0: 5000), otherwise
  `cancelTimer(t)`; then *update*;
- `input(tick)`: *update*;
- `timer(t)`: `pulseOn = 1 - pulseOn`; write `pulse`; then write `calls`;
- `period()`: `ticks = ticks + 1`; write `ticks`; then write `calls`.

*update*: if `x` is good: if `x > faultAbove` then `fault("x above faultAbove")`; otherwise write
`y = gain * x + offset`. Then write `calls = count`.

## 16. The heating demand `org.homecontrol.heating-demand` 1.0

Combines the rooms' demands into a request to the heat source and its circulation pump (issue #61):
an on/off request with a hysteresis and minimum on and off times against cycling, the demand level
for a modulating source, the pump with its run-on, and an offset for the heating curve (section 17)
that raises the flow while the demand stays high. It reads what room controllers emit
(`block/<id>/valve`, `block/<id>/demand`) or KNX valve positions and heat-request flags directly.

### 16.1 Manifest

Period 60 000 ms, `persistEvery` 600 000 ms, timer `wake`.

| Input | Type | Unit | | Multiple, aggregations | Trigger | Meaning |
|---|---|---|---|---|---|---|
| `valves` | real | % | | 32: `max` | update | Valve positions 0..100 %, e.g. the room controllers' `valve`. |
| `demands` | boolean | | | 32: `any` | update | Heat requests, e.g. the room controllers' `demand`. |
| `enable` | boolean | | | 1 | change | False: no request (heating off). |
| `summer` | boolean | | | 1 | change | True: no request (e.g. the heating curve's `summer`). |

| Output | Type | Unit | Failsafe | Meaning |
|---|---|---|---|---|
| `request` | boolean | | true | The heat source on. |
| `level` | real, `min` 0, `max` 100 | % | 100 | The demand: the highest valve, or the mean of the `topCount` highest. |
| `pump` | boolean | | true | The circulation pump. |
| `flowSetpointOffset` | real, `min` 0, `max` 20 | K | 0 | For the heating curve's `offset` input. |
| `state` | enumeration, `min` 0, `max` 5 | | diagnostic | 0 off, 1 on, 2 held on (off wanted, `minOn` runs), 3 held off (on wanted, `minOff` runs), 4 off by `enable`, 5 summer. |
| `staleInputs` | integer | | diagnostic | The bound datapoints of `valves` and `demands` that are not good. |

| Parameter | Type | Default | Range | Adjustable | Meaning |
|---|---|---|---|---|---|
| `mode` | enumeration | `max` (0) | `max` 0, `topMean` 1 | | The level: the highest input, or the mean of the `topCount` highest. |
| `topCount` | integer | 3 | 1..32 | | For `topMean`. |
| `onThreshold` | real, % | 20 | 0..100 | | The request goes on at a level at or above it. |
| `offThreshold` | real, % | 5 | 0..100 | | It goes off at a level at or below it; `offThreshold` < `onThreshold` (B17, blamed: `offThreshold`). |
| `minOn` | duration | 300 000 | 0..3 600 000 | | The shortest time the request stays on. |
| `minOff` | duration | 300 000 | 0..3 600 000 | | The shortest time it stays off. |
| `pumpRunOn` | duration | 300 000 | 0..3 600 000 | | The pump runs this long after the request went off. |
| `targetLevel` | real, % | 70 | 10..100 | | The level above which the offset rises (at least 10, so that it falls while the request is off). |
| `offsetGain` | real, K/(% h) | 0.1 | 0..1 | | How fast it rises (and falls) per % of level above (below) `targetLevel`; 0: no offset. |
| `maxOffset` | real, K | 5 | 0..20 | | The highest offset. |
| `staleInput` | enumeration | `ignore` (0) | `ignore` 0, `full` 1 | | An input that is not good: left out, or counted as fully open (100 %, a true demand). |

State: `offset` persisted, and not persisted `lastStep`, `lastLevel`, `heat`, `switchedAt`,
`hasSwitched`, `hasSent`, `sentRequest`, `sentPump`, `sentLevel`, `sentOffset`.

### 16.2 Decisions

- **D1. One level over valves and demands.** A true demand counts as a valve at 100 %, a false one as
  0 %; the level is taken over all of them together. So a room that has only a heat-request flag (a
  two-point thermostat, a radiator thermostat with a call-for-heat bit) and a room with a valve
  position combine under one rule and one hysteresis. A room is bound by its valve or by its demand,
  not by both: its true demand would count as 100 % beside its valve. *Alternative:* a separate
  "any demand" condition beside the level (a second switching rule, and with room controllers it
  would switch on at every valve above 0 %).
- **D2. The inputs are read one by one** (B20), with trigger `update`: a change of a valve that is not
  the highest moves the mean of the highest, and a datapoint that goes stale changes nothing in the
  aggregated value while another is good. The ports' aggregations (`max`, `any`) serve only their
  mirrors, which show the highest valve and whether any room asks.
- **D3. "Not good" is stale or unknown.** A datapoint without a value since the load and one whose
  technology lost contact are treated alike by `staleInput`: `full` heats on missing information.
  After a reboot the restored values are stale until their next report, so `full` requests heat
  until the rooms have reported; that is its purpose (better heated than frozen). Times of updates
  are not checked: a room controller sends its valve only on a change, so a valve that has not
  changed for hours is not missing.
- **D4. The minimum times apply to every switching**, also to `enable` and `summer`: they protect the
  heat source, whatever asks for the change. A switch they hold back happens when they expire (the
  timer `wake`), not at the next period. They count from the last switching of this instance; after a
  boot (no switching yet) the first switching is not held.
- **D5. The offset integrates the level.** `flowSetpointOffset` rises by `offsetGain` K per hour for
  every % the level lies above `targetLevel` while the request is on, and falls the same way below
  it; while the request is off the level counts as 0 (no flow, the offset winds down). It stays in
  0..`maxOffset`. The level of an interval is the one at its start (the step integrates over the time
  since the previous step with the level and request that held during it), so the result does not
  depend on how often the inputs report. The offset is persisted: a reboot during a cold spell does
  not drop the raised flow.
- **D6. Failsafe: the heat source on its own control.** A fault or a disabled instance writes
  `request` true, `pump` true, `level` 100 and the offset 0: with an external request, a boiler or
  heat pump runs on its own controller, which keeps the house from freezing; a pump never stays off
  while the source runs. Heating is switched off with the `enable` input, not with the instance's
  enable switch.
- **D7. Writes on change.** `request`, `pump` and `level` are written at the start and when they
  change; the offset when it moved by at least 0.1 K since it was last written, or reached 0 or
  `maxOffset`; the diagnostic outputs at every step. Nothing is rounded: `level` is the mean as
  computed (33.333... %); bound to an integer datapoint it gets the rounding of 5.4.
- **D8. Neither input is required.** An instance may bind only valves or only demands; without any
  bound or good input the level is 0 and the request goes off (after `minOn`).
- **D9. The minimum times count this instance's own switchings.** The failsafe phase of a disable or
  fault is none: the heat source ran on its own control then, with its own protection against
  cycling. So at enabling the first step writes `request` = `heat` at once, and after a boot or a
  restart (`heat` and `hasSwitched` are not persisted) the first switching is not held.
- **D10. A start integrates nothing.** `start` sets `lastStep` to the cascade time, as the room
  controller does: the time since the last step before an image swap (at most a period) is not
  added to the offset.

### 16.3 The calls

| Call | What it does |
|---|---|
| `start(reason, restored)` | Without `restored`, every field at its initial value. Then `lastStep = now`, `hasSent = 0`, the timer `wake` cancelled; then the step. |
| every `input`, `timer(wake)`, `period()`, `parameter` | The step. |

At an image swap the record in RAM is kept (8.1), with `heat`, `switchedAt` and `hasSwitched`: a
deploy does not cut short a minimum time, and the step sets `wake` again.

### 16.4 The step

With `t` = now (ms):

```
dt = (t - lastStep) / 1000.0;  lastStep = t                       // seconds, >= 0
e  = (heat == 1 ? lastLevel : 0) - targetLevel                    // the interval just ended (D5)
offset = offset + offsetGain * e * (dt / 3600.0)
offset = min(max(offset, 0), maxOffset)

values = [];  staleCount = 0                                      // the level (D1, D2, D3)
for k in 0 .. count(valves) - 1:
    v = readAt(valves, k)
    if v.quality == good:          append min(max(v.value, 0), 100)
    else:                          staleCount = staleCount + 1;  if staleInput == full: append 100
for k in 0 .. count(demands) - 1:
    v = readAt(demands, k)
    if v.quality == good:          append (v.value != 0 ? 100 : 0)
    else:                          staleCount = staleCount + 1;  if staleInput == full: append 100
m = the length of values
if m == 0:            level = 0
else if mode == max:  level = the largest of values
else:                 n = min(topCount, m)
                      level = (the n largest of values, added from the largest down, left to right) / n

off    = enable is good and false
summer = summer is good and true                                  // the input
if off or summer:                  want = 0
else if level >= onThreshold:      want = 1
else if level <= offThreshold:     want = 0
else:                              want = heat

held = 0;  holdUntil = 0                                          // the minimum times (D4)
if want != heat:
    hold = (heat == 1 ? minOn : minOff)
    if hasSwitched == 0 or t - switchedAt >= hold:
        heat = want;  switchedAt = t;  hasSwitched = 1
    else:
        held = 1;  holdUntil = switchedAt + hold

pump = heat == 1 or (hasSwitched == 1 and t - switchedAt < pumpRunOn)
wakeAt = holdUntil when held, else none
if heat == 0 and pump:  wakeAt = the earlier of wakeAt and switchedAt + pumpRunOn
if wakeAt is set: setTimer(wake, wakeAt - t)  else: cancelTimer(wake)
lastLevel = level

state = held == 1 ? (heat == 1 ? 2 : 3) : off ? 4 : summer ? 5 : heat
```

`pump` with `pumpRunOn` 0 follows `heat`. Then the outputs, in this order, with `first = (hasSent == 0)`:

1. `request` = `heat`, when `first` or `heat != sentRequest`; `sentRequest = heat`.
2. `level`, when `first` or `level != sentLevel`; `sentLevel = level`.
3. `pump`, when `first` or `pump != sentPump`; `sentPump = pump`.
4. `flowSetpointOffset` = `offset`, when `first`, or `offset != sentOffset` and
   (`|offset - sentOffset| >= 0.1` or `offset` is 0 or `maxOffset`); only when it is written,
   `sentOffset = offset` (so small moves add up to the 0.1 K).
5. `state`, `staleInputs` = `staleCount`: at every step.
6. `hasSent = 1`.

### 16.5 Cost

One instance with 8 valves and 4 demands bound: about 2.0 KB of heap on the host (64-bit; 2013 B),
against 3.3 KB (3255 B) of a room controller measured the same way (doc/blocks.md "Memory"): 11
state fields, 11 parameters, 9 mirror datapoints (enabled, 6 outputs, the two aggregated inputs), two
bytes per bound datapoint. The C6's 32-bit pointers make it slightly smaller.

### 16.6 Tests

The host tests (`core/test/heating_test.cpp`) drive the block with a fluctuating demand and check
the hysteresis and the minimum times (no switching closer than `minOn`/`minOff`, held switchings
happen when they expire), a stale valve per `staleInput`, the mean of the highest, the pump's
run-on, the offset rising under a sustained high demand and falling again, its persistence over a
reboot, and no allocation in steady state. The corpus has the contract cases
(`spec/fixtures/blocks/README.md`).

## 17. The heating curve `org.homecontrol.heating-curve` 1.0

The weather-compensated flow setpoint of a heat source or a mixing circuit that takes one (KNX DPT
9.001, later Modbus and others; issue #62): the curve over the filtered outside temperature, shifted
by the room setpoint and an offset (the heating demand's `flowSetpointOffset`), limited, with the
summer/winter switch derived from the filtered outside temperature and frost protection.

### 17.1 Manifest

Period 60 000 ms, `persistEvery` 1 800 000 ms, no timers.

| Input | Type | Unit | | Multiple, aggregations | Trigger | Meaning |
|---|---|---|---|---|---|---|
| `outside` | real | °C | required | 4: `mean`, `min` | none | The outside temperature: sensors, or a weather value (`spec/external-values.md`). |
| `roomSetpoint` | real | °C | | 1 | change | Shifts the curve (17.4); unbound or not good: `roomTemperature`. |
| `offset` | real | K | | 1 | change | Added to the flow while good, e.g. the heating demand's `flowSetpointOffset`. |
| `enable` | boolean | | | 1 | change | False: off (frost protection only). |
| `summerSwitch` | boolean | | | 1 | change | An external summer/winter switch: while good it decides instead of the derived one. |

| Output | Type | Unit | Failsafe | Meaning |
|---|---|---|---|---|
| `flowSetpoint` | real (no `min`/`max`) | °C | 40 | The flow setpoint. |
| `summer` | boolean | | false | The summer/winter switch, e.g. for the room controllers' and the heating demand's `summer`. |
| `outsideFiltered` | real | °C | diagnostic | The filtered outside temperature. |
| `state` | enumeration, `min` 0, `max` 4 | | diagnostic | 0 heating, 1 summer, 2 off, 3 frost protection, 4 fault (no outside temperature). |

| Parameter | Type | Default | Range | Adjustable | Meaning |
|---|---|---|---|---|---|
| `curve` | enumeration | `slope` (0) | `slope` 0, `design` 1, `points` 2 | | The form of the curve (17.4). |
| `slope` | real | 1.2 | 0.2..4 | yes | K of flow per K the outside is below the room (`slope`). |
| `level` | real, K | 0 | -15..15 | yes | A parallel shift of the curve (every form). |
| `roomTemperature` | real, °C | 20 | 10..30 | | The room temperature the curve is drawn for. |
| `designOutside` | real, °C | -12 | -30..10 | | The design point (`design`): the outside temperature ... |
| `designFlow` | real, °C | 55 | 20..90 | | ... and the flow it needs. |
| `pointCount` | integer | 6 | 2..6 | | The points used (`points`). |
| `outside1` .. `outside6` | real, °C | -15, -10, -5, 0, 10, 20 | -40..40 | | The points' outside temperatures, rising (`points`). |
| `flow1` .. `flow6` | real, °C | 60, 55, 49, 43, 32, 20 | 10..90 | | Their flows. |
| `minFlow` | real, °C | 25 | 10..60 | | The lowest flow while heating; the flow of frost protection. |
| `maxFlow` | real, °C | 60 | 20..90 | | The highest flow; `minFlow` < `maxFlow` (B17). |
| `minChange` | real, K | 0.5 | 0..5 | | The smallest change of `flowSetpoint` that is sent. |
| `outsideFilter` | duration | 43 200 000 | 0..172 800 000 | | The filter's time constant (the building's inertia, 12 h); 0: none. |
| `summerOn` | real, °C | 18 | 5..30 | | Summer when the filtered temperature stays above it ... |
| `summerOff` | real, °C | 15 | 5..30 | | ... winter when it falls below; `summerOff` < `summerOn` (B17). |
| `summerDays` | integer | 3 | 0..14 | | ... for this many days (0: at once). |
| `frostOutside` | real, °C | 3 | -20..10 | | Frost protection below it (with 1 K hysteresis). |
| `offFlow` | real, °C | 0 | 0..40 | | The flow setpoint in summer and when off, without frost. |
| `sensorTimeout` | duration | 10 800 000 | 600 000..86 400 000 | | No update of `outside` for this long is a fault. |

The points are written in the manifest in the order `outside1`, `flow1`, `outside2`, `flow2`, ...,
`outside6`, `flow6`, between `pointCount` and `minFlow`. Cross-parameter checks (B17), in this
order: `minFlow` < `maxFlow` (blamed: `minFlow`); `summerOff` < `summerOn` (`summerOff`); with
`curve` `design`: `designOutside` < `roomTemperature` (`designOutside`) and `designFlow` >
`roomTemperature` (`designFlow`); with `points`: `outside<i+1>` > `outside<i>` for the first
`pointCount` points (`outside<i+1>`).

State: `filtered`, `hasFiltered`, `aboveFor` and `derivedSummer` persisted, and not persisted
`lastStep`, `frostOn`, `hasSent`, `sentFlow`, `sentSummer`.

### 17.2 Decisions

- **D1. A linear curve, three ways to give it.** `slope`: flow = room + level + slope × (room -
  outside), the classic slope and level of a boiler's heating curve; `design`: the line through the
  design point and (room, room); `points`: 2..6 points joined by straight lines, which model the
  bend of a radiator curve without the exponent B19 forbids. Beyond the first and the last point the
  flow stays at theirs (no extrapolation).
- **D2. The room setpoint moves the curve along the diagonal.** A room `d` K warmer than
  `roomTemperature` gets the flow the curve gives for an outside `d` K colder, plus `d`. For the
  linear forms this is the same as putting the room setpoint into the formula; it holds for the
  points the same way.
- **D3. The filter is the room controller's** (14.4, backward Euler over the monotonic dt), fed only
  by good values that are not older than `sensorTimeout`, and persisted: a reboot does not restart a
  12 h filter from the momentary temperature.
- **D4. The summer switch counts time, not calendar days.** `aboveFor` adds up the time the filtered
  temperature is above `summerOn` and is reset when it is not; summer begins when it reaches
  `summerDays` × 24 h. The filter already smooths the day and the night, so a run of warm days is a
  run of time above; no wall clock is needed, and a clock that is not synchronized changes nothing.
  An interval counts by the filtered value at its end (the step's new value), and one step at or
  below `summerOn` resets the count. `summerDays` 0 switches at the first step above.
  Winter begins as soon as the filtered temperature falls below `summerOff`. Both are persisted.
- **D5. Frost protection on the momentary outside temperature**, not the filtered one: frost is a
  matter of the hour. Below `frostOutside` the flow is at least `minFlow` also in summer and when off;
  it ends at `frostOutside` + 1 K. A stale value younger than `sensorTimeout` is the last known
  outside temperature and counts; without a fresh value the filtered one stands in.
- **D6. A missing outside temperature holds the filter.** An outside temperature that is unknown
  (after a boot until its first report) or not updated for `sensorTimeout` (whether good or stale)
  shows the state `fault`: the curve runs on the held filtered value (persisted, so a reboot keeps
  it), the summer switch neither counts nor switches. A stale value younger than the timeout is not
  fed but not a fault either. The state is diagnostic: the instance keeps running, this is no fault
  of section 7 and writes no failsafe. Without any value since the first start, the filtered value is
  its initial 0 °C (a moderate winter flow). The steps that do not feed the filter still advance
  `lastStep`: after an outage the filter moves on from where it was held, by the weight of one
  interval at a time.
- **D7. Failsafe 40 °C, winter.** A fault writes a moderate flow that a radiator circuit lives with;
  a floor circuit needs its own high-limit thermostat in any case. The failsafe of `summer` is
  winter, which lets the rooms heat.
- **D8. The flow is sent on a change of `minChange`**, and at once when it reaches `offFlow`,
  `minFlow` or `maxFlow`; the filter moves it a little every period.

### 17.3 The calls

| Call | What it does |
|---|---|
| `start(reason, restored)` | Without `restored`, every field at its initial value. Then `lastStep = now`, `hasSent = 0`; then the step. |
| every `input`, `period()`, `parameter` | The step. |

### 17.4 The step

With `t` = now (ms):

```
dt = (t - lastStep) / 1000.0;  lastStep = t
Tf = outsideFilter / 1000.0
X  = read(outside)
fresh = X.quality != unknown and t - X.updated <= sensorTimeout
if fresh and X.quality == good:
    x = X.value
    if hasFiltered == 0 or Tf == 0:  filtered = x;  hasFiltered = 1
    else:                             filtered = filtered + (x - filtered) * (dt / (Tf + dt))
    if filtered > summerOn:  aboveFor = min(aboveFor + dt, summerDays * 86400.0)
    else:                    aboveFor = 0
    if derivedSummer == 0 and filtered > summerOn and aboveFor >= summerDays * 86400.0:  derivedSummer = 1
    else if derivedSummer == 1 and filtered < summerOff:                                derivedSummer = 0
fx = fresh ? X.value : filtered                                   // frost (D5)
if fx < frostOutside:               frostOn = 1
else if fx >= frostOutside + 1.0:   frostOn = 0

S = read(summerSwitch);  summer = S.quality == good ? S.value != 0 : derivedSummer == 1

R  = roomTemperature
RS = read(roomSetpoint);  Tr = RS.quality == good ? RS.value : R
curve slope:   flow = Tr + level + slope * (Tr - filtered)
curve design:  s = (designFlow - R) / (R - designOutside);  flow = Tr + level + s * (Tr - filtered)
curve points:  d = Tr - R;  flow = interpolate(filtered - d) + d + level
O = read(offset);  if O.quality == good:  flow = flow + O.value
flow = min(max(flow, minFlow), maxFlow)

off = enable is good and false
if off or summer:
    if frostOn == 1:  out = minFlow;  state = 3
    else:             out = offFlow;  state = off ? 2 : 1
else:                 out = flow;     state = 0
if not fresh:  state = 4
```

`interpolate(x)` over the points (`outside<i>`, `flow<i>`), i = 1 .. n with n = `pointCount`: `flow1` when
x <= `outside1`; `flow<n>` when x >= `outside<n>`; otherwise, for the first i with x <=
`outside<i+1>`: `flow<i> + (flow<i+1> - flow<i>) * ((x - outside<i>) / (outside<i+1> - outside<i>))`.

Then the outputs, in this order, with `first = (hasSent == 0)`:

1. `flowSetpoint` = `out`, when `first`, or `out != sentFlow` and (`|out - sentFlow| >= minChange`
   or `out` is `offFlow`, `minFlow` or `maxFlow`); only when it is written, `sentFlow = out`.
2. `summer`, when `first` or `summer != sentSummer`; `sentSummer = summer`.
3. `outsideFiltered` = `filtered`, `state`: at every step.
4. `hasSent = 1`.

### 17.5 Cost

One instance with two outside sensors: about 2.2 KB of heap on the host (64-bit; 2170 B), against
3.3 KB of a room controller: 9 state fields, 29 parameters, 8 mirror datapoints (enabled, `slope`,
`level`, 4 outputs, the aggregated outside temperature).

### 17.6 Tests

The host tests (`core/test/heating_test.cpp`) check the curve's points and the interpolation between
them and the linear forms, the filter's lag of 12 h on a ramp of the outside temperature, the summer
switch (consecutive days, the hysteresis, a reboot in the middle of the count), frost protection in
summer, a stale or silent outside temperature (the held value, `fault` after `sensorTimeout`), and no
allocation in steady state. The corpus has the contract cases (`spec/fixtures/blocks/README.md`).

## 18. The sun shading `org.homecontrol.sun-shading` 1.0

Automatic shading of one facade or window group with a venetian blind, a roller shutter or an
awning (issue #63): it lowers the blind while the sun shines on the facade and it is bright enough,
tracks the slats so that they just block the direct sun, closes for heat protection while nobody is
at home, retracts for wind, rain and frost, and steps back for a while after an inhabitant operated
the blind. Its outputs fit KNX blind actuators: the position and the slat position (DPT 5.001, 0 %
open/up, 100 % closed/down) or, for an actuator without positions, up/down (DPT 1.008). The times of
opening and closing in the morning and the evening are schedule entries (`spec/schedules.md`), not
this block.

### 18.1 Manifest

Period 60 000 ms, timer `wake`, no persisted state.

| Input | Type | Unit | | Multiple, aggregations | Trigger | Meaning |
|---|---|---|---|---|---|---|
| `brightness` | real | lx | | 4: `max`, `mean` | change | The facade's brightness; unbound: the sun's position decides alone. |
| `wind` | real | m/s | | 4: `max` | update | The wind speed. |
| `rain` | boolean | | | 4: `any` | update | Rain sensors. |
| `frost` | boolean | | | 1 | change | A frost alarm. |
| `outside` | real | °C | | 1 | change | The outside temperature: frost below `frostLimit`. |
| `manual` | boolean | | | 8: `any` | update | The blind's operations: the up/down and stop/step datapoints of its wall switches, and the outputs of schedule entries that move it (18.2 D6). A new value is an operation. |
| `presence` | boolean | | | 1 | change | False: nobody at home (heat protection). |
| `summer` | boolean | | | 1 | change | True: summer (heat protection); unbound: always. |
| `roomTemperature` | real | °C | | 4: `max`, `mean` | change | For heat protection. |
| `enable` | boolean | | | 1 | change | False: the automatic is off (shading and heat protection); the alarms stay. |

| Output | Type | Unit | Failsafe | Meaning |
|---|---|---|---|---|
| `position` | real, `min` 0, `max` 100 | % | 0 | The blind's position (DPT 5.001: 0 up, 100 down). |
| `slat` | real, `min` 0, `max` 100 | % | 0 | The slat position (DPT 5.001). |
| `upDown` | boolean | | false | For an actuator without positions (DPT 1.008: false up, true down). |
| `state` | enumeration, `min` 0, `max` 7 | | diagnostic | 0 idle, 1 shading, 2 wind, 3 rain, 4 frost, 5 manual, 6 heat protection, 7 off. |
| `sunAzimuth` | real | ° | diagnostic | `sun()`'s azimuth (written while known). |
| `sunElevation` | real | ° | diagnostic | `sun()`'s elevation (written while known). |

| Parameter | Type | Default | Range | Adjustable | Meaning |
|---|---|---|---|---|---|
| `facadeAzimuth` | real, ° | 180 | 0..360 | | The direction the facade faces (its normal), from north through east. |
| `openingAngle` | real, ° | 180 | 10..180 | | The sun is on the facade while its azimuth lies within half this angle of `facadeAzimuth`. |
| `minElevation` | real, ° | 5 | 0..90 | | The sun is on the facade only above this elevation. |
| `overhangAngle` | real, ° | 90 | 10..90 | | An overhang (a balcony, a roof) shades the window above this profile angle (18.4); 90: none (no test: near the facade's edge the profile angle nears 90°). |
| `horizonPoints` | integer | 0 | 0..4 | | The points of the horizon used; 0: none. |
| `horizonAngle1` .. `horizonAngle4` | real, ° | -60, -20, 20, 60 | -90..90 | | The points' directions relative to `facadeAzimuth` (negative: towards the lower azimuth), rising. |
| `horizonElevation1` .. `horizonElevation4` | real, ° | 0 | 0..90 | | The horizon there (buildings, trees opposite): the sun is on the facade only above it. |
| `brightnessOn` | real, lx | 30 000 | 0..200 000 | | Bright at or above it ... |
| `brightnessOff` | real, lx | 20 000 | 0..200 000 | | ... dark below it; `brightnessOff` < `brightnessOn` (B17). |
| `brightnessOnDelay` | duration | 120 000 | 0..3 600 000 | | How long it must be bright before it counts. |
| `brightnessOffDelay` | duration | 900 000 | 0..7 200 000 | | How long it must be dark before it counts. |
| `shadePosition` | real, % | 100 | 0..100 | yes | The position while shading. |
| `slatMode` | enumeration | `fixed` (0) | `fixed` 0, `tracking` 1 | | The slats while shading: at `shadeSlat`, or following the sun (18.4). |
| `shadeSlat` | real, % | 75 | 0..100 | | The slat position of `fixed`. |
| `slatRatio` | real | 0.9 | 0.2..1 | | The slats' spacing divided by their width (`tracking`). |
| `slatOpen` | real, % | 0 | 0..90 | | The slat position of horizontal slats; 100 % is closed (`tracking`; 50 for an actuator whose slats turn through 180°). |
| `slatStep` | real, % | 10 | 1..50 | | The slat positions of `tracking` go in steps of it from `slatOpen`. |
| `heatLimit` | real, °C | 26 | 18..40 | yes | Heat protection above it (with 1 K hysteresis). |
| `windLimit` | real, m/s | 10 | 1..40 | | Wind alarm at or above it. |
| `windTimeout` | duration | 0 | 0..86 400 000 | | A bound wind input without an update for this long, or without any value, is a wind alarm (a failed sensor); 0: never. |
| `frostLimit` | real, °C | 2 | -20..10 | | Frost alarm below this outside temperature. |
| `alarmRelease` | duration | 600 000 | 0..7 200 000 | | An alarm ends when its condition has not held for this long. |
| `manualLock` | enumeration | `duration` (0) | `duration` 0, `sunChange` 1, `midnight` 2 | | How long an operation suspends the automatic (18.2 D7). |
| `manualDuration` | duration | 7 200 000 | 60 000..86 400 000 | | The suspension of `duration`, the longest of `sunChange`, and that of `midnight` without a wall clock. |
| `minMoveInterval` | duration | 300 000 | 0..3 600 000 | | The shortest time between two moves of the automatic (alarms excepted). |

The horizon points are written in the manifest in the order `horizonAngle1`,
`horizonElevation1`, ..., `horizonAngle4`, `horizonElevation4`, between `horizonPoints` and
`brightnessOn`. Cross-parameter checks (B17), in this order: `brightnessOff` < `brightnessOn`
(blamed: `brightnessOff`); `horizonAngle<i+1>` > `horizonAngle<i>` for the first `horizonPoints`
points (`horizonAngle<i+1>`).

State (nothing persisted): `lastState` (initial -1), `windOn`, `windCalmAt` (-1), `rainOn`,
`rainDryAt` (-1), `frostOn`, `frostClearAt` (-1), `bright`, `brightAt` (-1), `heat`, `manualAt`
(-1), `manualSunny`, `manualDay` (-1), `manualSeen` (-1), `holding`, `due`, `movedAt` (-1),
`sentPosition` (-1), `sentSlat` (-1), `sentUpDown` (-1); the others initial 0. A time field at -1 is
none.

### 18.2 Decisions

- **D1. The sun from the controller** (B21): its azimuth and elevation follow from the wall clock and
  the location; the block compares them with the facade. Without a synchronized clock or a location
  there is no sun: no shading (state `idle`), while heat protection and the alarms work on. A
  brightness sensor alone does not shade: a roof sensor would lower every facade.
- **D2. The automatic moves on changes.** The block writes when its target changes (shading begins,
  a slat step, heat protection), and once when shading or heat protection ends (it retracts the
  blind it lowered: `holding`). In `idle` it writes nothing else, so a schedule that closes the
  blinds in the evening, or an inhabitant, is not overridden; a start in `idle` writes nothing (a
  reboot at night leaves the blinds closed). A move writes the outputs that differ from what was
  last written, all three when the state just became `shading` or `heat protection` or the blind
  is released (`due`: after an operation or a start the blind may stand anywhere).
- **D3. Fixed priorities: wind > rain > frost > manual > off > heat protection > shading > idle.**
  The alarms retract the blind (position 0, slat 0, up), the safe position for wind (a lowered
  blind is what a storm breaks), for rain (an awning's fabric) and for frost (a blind that freezes
  in the down position cannot be raised); they are entered at once and end when their condition has
  not held for `alarmRelease`. They need no wall clock: they count monotonic time.
- **D4. An alarm counts every known value, of every sensor.** Wind at or above `windLimit`, a true
  rain or frost alarm, an outside temperature below `frostLimit`: good or stale (a sensor that lost
  contact keeps its last value, and a stale storm is a storm); only an unknown value has no alarm.
  `wind` and `rain` are read one by one (B20): one sensor's storm is an alarm whatever the others
  say (their `max` and `any` aggregate the good values first and would hide a stale storm behind a
  good calm; they serve the mirrors). `windTimeout` makes a silent or missing wind sensor an alarm,
  each of them: an unattended blind is retracted when one of its wind sensors fails, and at a start
  until every sensor has a value (then `alarmRelease` more). After a reboot the datapoints whose
  values the device persists are restored stale (`doc/persistence.md`), so a storm before the
  reboot is one after it; the others are unknown (the corpus's `reboot` makes every value unknown).
  Because the sensors are read one by one, `wind` and `rain` have the trigger `update`: with
  `change`, a report of one sensor that leaves the aggregate as it was (a calm after a stale storm,
  behind a good sensor at a higher speed) would call nothing, and the alarm would end a period late;
  every report also refreshes `windTimeout`'s view of its sensor.
- **D5. Failsafe: retracted.** A fault or a disabled instance writes position 0, slat 0 and up, the
  safe position, like an alarm (B10).
- **D6. An operation is a new value of `manual`**: an update (trigger `update`) after which one of its
  datapoints has an update time later than any the block has seen (`manualSeen`), by a report, a
  command of a rule or a schedule, or the UI. A datapoint going stale is no operation (it keeps its
  update time); neither is the value a datapoint has at the start. Bind the blind's wall-switch
  datapoints there (DPT 1.008, 1.007), which the actuator obeys besides the block's outputs, and
  also the datapoints by which schedule entries close the blinds in the evening: such a closing then
  locks the automatic too (`midnight`: until the next day), so shading does not retract the blind at
  the end of the afternoon sun. The block's own outputs cannot be among them (B22): where a wall
  switch and the automatic would share one up/down group address, the block's `upDown` gets a group
  address of its own that the actuator's object also listens to (or the block drives `position`).
  An operation during an alarm does not lock; the block writes the retracted position again at
  once, so the alarm keeps the blind up. The port's aggregation `any` serves only its mirror (the
  last value means nothing to the block).
- **D7. The lock.** `duration`: `manualDuration` from the last operation. `sunChange`: until the
  shading wish (the sun on the facade and bright) differs from what it was at the operation, at most
  `manualDuration`. `midnight`: until the wall date differs from the date of the operation; without
  a wall clock at the operation or now, `manualDuration`. A lock is not persisted: a reboot ends it,
  an image swap keeps it (the record in RAM, 8.1). When it ends in `shading` or `heat protection`,
  the block moves (`due`); in `idle` it writes nothing.
- **D8. The sun on the facade** is a geometric test: the azimuth within `openingAngle / 2` of the
  facade's normal, the elevation above `minElevation` and above the horizon at the sun's direction
  (up to 4 points relative to the facade, joined by straight lines and flat beyond the first and
  the last, like the heating curve's points), and the profile angle below `overhangAngle`. The
  profile angle is the sun's elevation projected into the vertical plane normal to the facade:
  what a horizontal overhang and horizontal slats see.
- **D9. Brightness with hysteresis and delays.** Bright when the brightness stays at or above
  `brightnessOn` for `brightnessOnDelay`, dark again when it stays below `brightnessOff` (or not
  good) for `brightnessOffDelay`. A value that is not good counts as dark: shading is comfort, not
  safety. Unbound, it is always bright.
- **D10. Slat tracking at the cut-off angle.** For slats of width `w` at spacing `s`, the sun's
  rays at the profile angle `α` just do not pass between them at the slat angle
  `β = asin((s / w) cos α) - α` (from horizontal, outer edge down; derivation in 18.4), the most open
  angle that still blocks the direct sun. `β` is limited to 0..90°, mapped linearly onto
  `slatOpen` .. 100 % and rounded up to the next step of `slatStep` from `slatOpen` (towards closed:
  never let the sun through; horizontal slats stay reachable), so that the slats move in steps, not
  every minute.
- **D11. Heat protection** while nobody is at home (`presence` good and false), in summer (`summer`
  good and true, or unbound) and the room is warmer than `heatLimit` (good, `roomTemperature`
  above it; off below `heatLimit - 1`): the blind closes fully (position 100, slat 100), whether or
  not the sun is on the facade, and needs no wall clock. Unbound `presence` or `roomTemperature`: no
  heat protection.
- **D12. A minimum time between moves.** The automatic's moves (shading, slat steps, heat
  protection, the release) keep `minMoveInterval` from the previous move; a move held back happens
  when it expires (the timer `wake`), with the target as it is then. Alarms move at once. A start
  moves at once.
- **D13. `enable` false switches the automatic off** (state `off`): no shading and no heat
  protection, nothing written; the alarms work on. The instance's enable switch instead stops the
  block with the failsafe (D5). `enable` true again continues: in `shading` it moves, in `idle` after
  shading it releases (`holding` stayed).
- **D14. Nothing is persisted.** The state follows from the inputs within the delays; a reboot loses
  only a lock (D7) and the brightness delays. So the type writes no record to flash.
- **D15. Bind either `position` or `upDown`.** A positioning actuator moves to the position (and the
  slat after it); `upDown` drives one without positions to its end (`true` when the target position
  is above 0). Both bound, a "down" after a position would drive the blind fully down.
- **D16. Smaller points.** `enable` and `presence` count only when good (a stale `enable` false
  turns the automatic on again, as for the other types). Disabling and enabling the instance keeps
  the record in RAM (6.1): a lock, the alarms and `holding` survive it, and the start moves at once
  with every output (`due`), so the failsafe's retraction is not mistaken for the last written
  position; a release of `holding` may write the retracted position a second time. `state` and the
  sun's mirrors are written at every step (B9: bound, they command every time); without a sun they
  keep their last value. A held move is no promise: the step at its expiry moves to the target as it
  is then, or not at all.

### 18.3 The calls

| Call | What it does |
|---|---|
| `start(reason, restored)` | Without `restored`, every field at its initial value. Then `lastState = -1`, `movedAt = -1`, `manualSeen = latest(manual)`, the timer `wake` cancelled; then the step with `operated = 0`. |
| `input(manual)` | `operated = latest(manual) > manualSeen ? 1 : 0`; `manualSeen = max(manualSeen, latest(manual))`; then the step with `operated`. |
| every other `input`, `timer(wake)`, `period()`, `parameter` | The step with `operated = 0`. |

`latest(manual)` is the largest update time of the datapoints bound to `manual` that are not unknown
(`readAt`, B20), or -1 when there is none.

### 18.4 The step

With `t` = now (ms) and `Q.known` meaning `Q.quality != unknown`:

```
alarm(on, at, cond):                                                    // D3, D4: the three alarms
    if cond:            on = 1;  at = -1
    else if on == 1:    if at < 0: at = t
                        if t - at >= alarmRelease:  on = 0;  at = -1

windCond = 0                                                            // every wind sensor (D4)
for k in 0 .. count(wind) - 1:
    Wk = readAt(wind, k)
    if (Wk.known and Wk.value >= windLimit) or (windTimeout > 0 and (not Wk.known or t - Wk.updated >= windTimeout)):
        windCond = 1
alarm(windOn, windCalmAt, windCond == 1)
rainCond = 0
for k in 0 .. count(rain) - 1:
    Rk = readAt(rain, k);  if Rk.known and Rk.value != 0:  rainCond = 1
alarm(rainOn, rainDryAt, rainCond == 1)
F = read(frost);  O = read(outside)
alarm(frostOn, frostClearAt, (F.known and F.value != 0) or (O.known and O.value < frostLimit))

B = read(brightness)                                                    // D9
if not B.bound:  bright = 1;  brightAt = -1
else:
    if bright == 0:  change = B.good and B.value >= brightnessOn;  delay = brightnessOnDelay
    else:            change = not B.good or B.value < brightnessOff;  delay = brightnessOffDelay
    if not change:   brightAt = -1
    else:            if brightAt < 0: brightAt = t
                     if t - brightAt >= delay:  bright = 1 - bright;  brightAt = -1

sunOn = 0;  profile = 0                                                 // D1, D8
if sun() is known as (az, el):
    g = az - facadeAzimuth;  g = g - 360.0 * floor((g + 180.0) / 360.0)        // -180 <= g < 180
    profile = atan2Deg(sinDeg(el), cosDeg(el) * cosDeg(g))
    sunOn = |g| < openingAngle / 2.0 and el > minElevation
            and (horizonPoints == 0 or el > horizon(g)) and (overhangAngle == 90 or profile < overhangAngle)
sunny = sunOn == 1 and bright == 1

P = read(presence);  S = read(summer);  RT = read(roomTemperature)     // D11
if P.good and P.value == 0 and (not S.bound or (S.good and S.value != 0)) and RT.good:
    if RT.value > heatLimit:               heat = 1
    else if RT.value < heatLimit - 1.0:    heat = 0
else:                                      heat = 0

alarmed = windOn == 1 or rainOn == 1 or frostOn == 1                   // D6, D7
w = wall()
if operated == 1 and not alarmed:
    manualAt = t;  manualSunny = sunny;  manualDay = w is known ? day(w) : -1;  holding = 0
if manualAt >= 0:
    if manualLock == midnight and manualDay >= 0 and w is known:  end = day(w) != manualDay
    else:  end = t - manualAt >= manualDuration or (manualLock == sunChange and sunny != manualSunny)
    if end:  manualAt = -1;  manualDay = -1

E = read(enable)
state = windOn == 1 ? wind : rainOn == 1 ? rain : frostOn == 1 ? frost : manualAt >= 0 ? manual
      : (E.good and E.value == 0) ? off : heat == 1 ? heatProtection : sunny ? shading : idle
```

`horizon(g)` is the heating curve's `interpolate` (17.4) over the points (`horizonAngle<i>`,
`horizonElevation<i>`), i = 1 .. `horizonPoints`, at `g`. `day(w)` is the number of days since
1970-01-01 of the wall date (14.4). Then the target (D2, D3, D10, D11, D12), with `retracted` =
(0, 0, 0) as (position, slat, upDown):

```
isAlarm(x)  = x is wind, rain or frost;   isActive(x) = x is shading or heatProtection
target = none
if isAlarm(state):
    if not isAlarm(lastState) or operated == 1:
        write position 0, slat 0, upDown 0;  sentPosition = 0;  sentSlat = 0;  sentUpDown = 0
        movedAt = t;  holding = 0;  due = 0
else if isActive(state):
    if not isActive(lastState):  due = 1
    if state == heatProtection:  target = (100, 100, 1)
    else:                        target = (shadePosition, slatTarget, shadePosition > 0 ? 1 : 0)
else if state == idle and holding == 1:
    target = retracted;  due = 1
else:
    due = 0
heldUntil = none
if target is (p, q, u) and (due == 1 or p != sentPosition or q != sentSlat or u != sentUpDown):
    if movedAt < 0 or t - movedAt >= minMoveInterval:
        if due == 1 or p != sentPosition:  write position p;  sentPosition = p
        if due == 1 or q != sentSlat:      write slat q;      sentSlat = q
        if due == 1 or u != sentUpDown:    write upDown u;    sentUpDown = u
        movedAt = t;  due = 0;  holding = isActive(state) ? 1 : 0
    else:
        heldUntil = movedAt + minMoveInterval
```

`slatTarget` is `shadeSlat` with `slatMode` `fixed`; with `tracking`:

```
beta = asinDeg(slatRatio * cosDeg(profile)) - profile
beta = min(max(beta, 0.0), 90.0)
q    = slatOpen + (100.0 - slatOpen) * (beta / 90.0)
slatTarget = min(slatOpen + slatStep * -floor(-((q - slatOpen) / slatStep)), 100.0)   // up to the next step
```

The cut-off angle (D10): a slat of width `w` turned by `β` (outer edge down) around its centre has
its edges at `±(w / 2)(cos β, -sin β)`, x outwards; the next slat up lies `s` higher. Direct light
passes just not when the ray at the profile angle `α` runs from the upper slat's outer edge to the
lower slat's inner edge: `w cos β sin α = (s - w sin β) cos α`, so `sin(α + β) = (s / w) cos α`.

Then the timer and the remaining outputs:

```
wakeAt = the earliest of:
    windCalmAt + alarmRelease     when windOn == 1 and windCalmAt >= 0;  the same for rain and frost
    Wk.updated + windTimeout      for each k with windTimeout > 0, Wk.known and t - Wk.updated < windTimeout
    brightAt + (bright == 0 ? brightnessOnDelay : brightnessOffDelay)    when brightAt >= 0
    manualAt + manualDuration     when manualAt >= 0, unless manualLock == midnight with manualDay >= 0 and w known
    heldUntil                     when set
if wakeAt is set:  setTimer(wake, wakeAt - t)  else:  cancelTimer(wake)
write state;  if sun() is known as (az, el):  write sunAzimuth az, sunElevation el
lastState = state
```

Every deadline lies after `t`, so the timer's duration is at least 1 ms. A `midnight` lock with a
wall clock ends at the first step on the next date (the period, within a minute).

### 18.5 Cost

One instance with every input bound (`manual` to two datapoints) and the three moving outputs:
about 2.8 KB of heap on the host (64-bit; 2760 B), against 3.3 KB (3271 B) of a room controller
measured the same way: 20 state fields, 31 parameters, 10 mirror datapoints (enabled,
`shadePosition`, `heatLimit`, 6 outputs, the aggregated `manual`); each further input bound to
several datapoints adds its mirror. No flash: nothing is persisted. A step costs the sun's position
(13.2: about 20 sines and cosines and three arc tangents; 0.25 µs on the host, about 0.7 ms on the
C6, which has no FPU, estimated from its ratio to the sun's events, which take 5 ms there); the
engine computes it once per wall second for every block of every cascade in it.

### 18.6 Tests

The host tests (`core/test/sun_shading_test.cpp`) drive the block over simulated days at a place
and check the sun on the facade by azimuth and elevation, with the horizon points and the overhang;
the slat angle against the cut-off formula over a day; the brightness hysteresis and both delays; a
wind alarm that overrides every other state and releases after `alarmRelease`, a stale storm, a
failed wind sensor by `windTimeout`, rain and frost; a manual operation that locks the automatic for
each `manualLock`, and an operation during an alarm; heat protection with its hysteresis; no two
moves closer than `minMoveInterval`; a reboot in `idle` that writes nothing; the failsafe; the
cost; and no allocation in steady state. `core/test/astro_test.cpp` checks `sun_position` (13.2)
against the reference values, and the exact trigonometry (13.1) against the library's functions.
The corpus has the contract cases (`spec/fixtures/blocks/README.md`).

## 19. Changes

- 1.0 (schema 1.3, issue #60): first version; the room controller 1.0.
- 1.1 (schema 1.3, issues #61, #62): the heating demand 1.0 (section 16) and the heating curve 1.0
  (section 17); an input binds up to 32 datapoints and a call reads each with `count`/`readAt` (B20).
- 1.2 (schema 1.3, issue #63): the sun shading 1.0 (section 18); the service `sun()` (B21) with the
  exact trigonometry and the sun's position of 13.1 and 13.2, `sqrt` among the operations of B19; a
  block does not read its own outputs (B22).
