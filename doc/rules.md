# The rule interpreter

The semantics are fixed by `spec/rules.md`; this document describes the C++ implementation in the
portable core: `hc::rules`, headers in `core/include/hc/rules/`, sources in `core/src/rules/`. The
conformance corpus `spec/fixtures/rules/` runs as `core/test/rules_corpus_test.cpp`, which E-Home's
Java reference interpreter (issue #13) runs as well.

## Parts

| File | Contents |
|---|---|
| `hc/json_value.h` | `JsonValue`/`JsonDocument`: a read-only, allocation-free view of a parsed JSON tree. The loader reads the image only through it; whatever parsed the JSON implements `JsonDocument` (the tests adapt their tree reader, the image loader of issue #8 can adapt its `JsonSpan`). |
| `rules/value.h` | `Val`, the expression value (40 bytes, no heap; strings and raw bytes inline), the conversions of spec 3.2, the JSON literals of 3.1, the formatting of 4.9 and the canonical literal text of 4.2. |
| `rules/rule_set.h` | `RuleSet`: the compiled `rules` member, loaded and checked (spec section 10). |
| `rules/engine.h` | `Engine`: the runtime (section 7), the parameters (section 8), the diagnostics (section 9). |
| `rules/parameter_store.h` | The persisted parameter overrides and rule states in `Settings`. |
| `rules/printer.h` | The canonical text of an expression (4.8), for diagnostics and the API. |

## Using it

```cpp
hc::rules::Engine engine(loop, model, clock, settings, time_source);
hc::rules::LoadResult result = engine.load(rules_json, &schedules); // the image's `rules`
if (result.ok()) engine.start(hc::rules::StartReason::boot);         // image after a swap
// later: engine.schedule_fired("morning"); engine.set_parameter("hall", "timeout", value);
```

- The engine registers itself as a `DatapointListener` of the model. A listener must not change
  the model, so the listener only records change times and queues the update; a posted callback
  runs the cascades.
- `TimeSource` gives the local wall time (with the summer-time flag), the sun times, and the
  conversions between a local date-time and its UTC instant (`local_to_utc`, `utc_to_local`) that
  date-time arithmetic and comparisons use (spec 4.5, 4.6). `hc::image::RulesClock` implements it
  over the time service of issue #9 and its `TimeZone`; the defaults (UTC) serve tests without a
  zone. `schedule_fired(id)` is called by the scheduler of issue #10.
- `load()` installs a new set without starting it. The first load is the boot and reads the
  persisted overrides and states; later loads are image swaps (7.6). Labels are resolved in the
  language of the setting `ui.lang`, read at every load. A refused load changes nothing and logs
  `rules refused: rule '<id>' at <json path>: <reason>: <text>`.
- `flush()` processes the queued external events now; call it before reloading the datapoint
  model.
- `EngineObserver` receives:
  - every log entry with a code of 9.2;
  - every command a rule made;
  - every change of a parameter's value. This is the hook for the parameter datapoints of
    decision ticket #28: a later `ParameterTechnology` subscribes here and calls `set_parameter()`
    the other way;
  - `loop_guard(rule, culprit, firings)` right after the `loopGuard` entry.
- `persist_now()` writes the delayed state changes at once; the destructor calls it, so an orderly
  shutdown loses nothing.

The Controller does not own an engine yet, and the ESP main does not create one. Wiring it in
comes with the runtime image (issue #8).

## Compiled form and memory

`RuleSet::load()` runs a builder twice over the JSON:

1. A counting pass validates everything and counts every array.
2. A build pass fills one block allocated in between.

The builder (about 6 KB: the rule ids of the image and the per-rule scope) lives on the heap only
during the load. The block holds flat arrays with 16-bit indices instead of pointers:

| Array | Bytes per entry | Notes |
|---|---|---|
| rules | 24 | ids and names are offsets into the string pool |
| reactions | 16 | trigger range, condition root, `from` bit mask, `to`, two action lists |
| triggers | 4 | kind, flags (`on`, `origin`, start reasons), one reference |
| actions | 6 | kind, flags, target, value root (a log action: first part, part count) |
| log parts | 8 | text offset and length, or an expression root |
| parameters | 24 | the declared type, `adjustable`, choices, and constant indices for default, min and max |
| timers, states | 8, 12 | the duration root; entry and exit action lists |
| nodes | 16 | see below |
| constants | 40 | literals that do not fit a node (strings, raw, date-times), and the values of parameter declarations |
| strings | as needed | NUL-terminated |
| schedules, tracked datapoints, match triggers | 4, 2, 4 | lookup tables for the event paths |

A node is 16 bytes: kind, operator or function, static type, argument count, the first child
(`a`), the subtree size, and an 8-byte literal union. Expressions are stored in pre-order, and the
children of a node are contiguous at `a .. a+argc-1`. So an expression is the contiguous range
`[root, root+size)`, and a match trigger finds the datapoints it depends on by scanning that range.
`state("id")` keeps the target rule's index in the literal field.

`Engine::load()` allocates a second block for the runtime:

| What | Bytes (64-bit host) | |
|---|---|---|
| per rule | 120 | state, firing counters, the last trigger, action and error |
| per parameter | 48 | current value and the override flag |
| per timer | 16 | deadline, sequence, rule, running |
| per node | 1 | the "once" flags of an expression root, the match memory |
| per tracked datapoint | 16 | the time of the last change (for `sinceChange`, `changedWithin`) |
| per datapoint of the model | 1 bit | interest: referenced by a trigger or a match condition |

The `Engine` object holds the queues: 64 inbound and 128 cascade events of 8 bytes each. It is
1984 bytes on the host.

Nothing is allocated while events are processed; a test counts the allocations to check it.
A swap allocates the new blocks before it frees the old ones.

Measured for a typical 50-rule image (`RulesEngine memory accounting`). Each rule has two
parameters plus `enabled`, a timer, and two reactions (a condition, two writes, a `restartTimer`);
in all 400 nodes:

- rule set block: 27,600 bytes, of which nodes 6,400, constants 10,000 and strings 3,091;
- runtime block: 14,520 bytes;
- total: about 42 KB, within the 64 KB the spec allows.

The constant pool dominates. Default, min and max of a parameter are full 40-byte values, which
makes a parameter cost about 190 bytes in all, against the spec's estimate of 48.

## Evaluation

`Engine::eval()` walks the nodes recursively. The recursion depth is bounded by the expression
depth (12), and each frame holds a few `Val`s. `toString()` formats in a non-inlined helper, so
its 200-byte buffer is not on every frame.

- **No value** is a `Val` with `present == false`.
- **An evaluation error** (spec 4.6: overflow, division by zero, `clamp` with low > high, a string
  over 28 bytes, ...) also yields no value, and sets an error flag with the first error's text.
  - `known()` and `coalesce()` absorb the missing value, but the error is still logged as
    `evalError`.
  - `conditionNoValue` and `valueNoValue` are logged only when the expression had no error.
- **The "once" flags** live per expression root, in the runtime's node flags. They are cleared
  when the root next evaluates without the problem.
- **The arithmetic** is that of spec 4.4 and 4.5:
  - integers overflow-checked;
  - `/` always real;
  - `%` with the dividend's sign;
  - durations in milliseconds;
  - time of day wrapping at midnight;
  - date-time ± duration on the UTC instant: `TimeSource::local_to_utc` (the summer-time flag
    picks the occurrence of an ambiguous time), plus the whole seconds, `utc_to_local` back; the
    result keeps only the summer-time flag. Date-time minus date-time and the comparisons of
    date-times use the instants too.
- **Mixed integer and real** branches of `if`, `coalesce`, `min`, `max` and `clamp` are promoted
  to real.
- **Formatting a real** (4.9) rounds the exact binary value with a big-integer helper, ties away
  from zero. A real literal's canonical text is the shortest decimal that reads back exactly.

All functions of spec 4.7 are implemented; none is cut.

## Cascades

- **External events**:
  - a datapoint update that did not come from a rule;
  - a schedule firing;
  - a parameter change from the UI or API;
  - a start.

  They wait in an inbound ring of 64 events (`queueOverflow` beyond). A posted callback processes
  them one cascade each, in arrival order.
- **Timers**: before each event, every rule timer that is due runs its own cascade, in (deadline,
  sequence) order (7.8). One event-loop timer covers all rule timers and is rescheduled to the
  earliest deadline after each cascade.
- **Derived events** are updates caused by a rule's command and `setParameter` actions. They are
  recognised by arriving while a cascade runs, and go into the cascade queue (128 events,
  `queueOverflow` once per cascade).
- **Processing an event** (7.2):
  1. Re-evaluate the match conditions that reference the event's datapoint or parameter, in every
     rule, enabled or not, and update their memories.
  2. Visit the enabled rules in rule order. A rule whose reactions (in its current state) have a
     matching trigger fires once; then its reactions run in order. A transition ends the rule's
     processing for this event.
- **The cascade time** is the monotonic clock when the cascade begins. The wall time is a snapshot
  of the `TimeSource` at the same moment, so every expression of a cascade sees one time.
- **The loop guard**: every firing on a derived event counts. Firing number 33 is refused: the
  queue is emptied, and `loopGuard` is logged for the rule that would have fired. The text names
  the culprit, the rule with the most derived firings in this cascade (the first in rule order on a
  tie): `cascade refused after 32 derived firings (most: ping with 16)`; the observer gets it as
  `loop_guard(rule, culprit, firings)`. Each rule's diagnostics count
  the cascades it was refused in and its peak firings in one cascade.
- **Start** (7.5): `start(boot|image)` first re-baselines every match memory without firing. Then
  each rule starts in its own cascade, in rule order. A rule with states enters `initialState`
  (entry actions) unless its state was preserved by the swap or restored at boot; then the `start`
  trigger fires.
- **Enabling** a rule runs start(`enable`) inside the cascade of the parameter change.
- **Disabling** stops the rule's timers at once.

## Parameters and persistence

Parameter 0 of every rule is the implicit `enabled`:

- label `Enabled`;
- adjustable as declared by `enabledAdjustable`;
- default as declared by `enabled`.

`set_parameter()` (from the UI or API) and the `setParameter` action follow the same steps:

1. Convert the value to the parameter's type (3.2), and check it against min, max and choices.
2. Refuse an invalid value with `parameterRefused`.
3. A value identical to the declared default removes the override and erases a stored one
   (spec 8.2, decision 15). Any other value becomes the override and is written to the store
   immediately, unless the store already holds it.
4. If the value changed, queue a parameter event: external for the UI and API, derived for the
   action.

`reset_parameter()` removes the override.

**Key mapping.** NVS keys hold at most 15 characters, and `<ruleId>/<parameterId>` takes up to 57.
An override is therefore stored under

    rp.<12 lowercase hex digits>

The digits are the 64-bit FNV-1a hash of `<ruleId>/<parameterId>`, folded to 48 bits with
`(h ^ (h >> 48)) & 0xFFFFFFFFFFFF`.

The stored string carries the full logical key, so a slot can always be traced back to its
parameter: `<ruleId>/<parameterId>=<code>:<text>`, at most 116 bytes. The codes are:

| Code | Type | Text |
|---|---|---|
| `b` | boolean | `0` or `1` |
| `i` | integer | decimal |
| `r` | real | `%.17g` |
| `s` | string | hex bytes |
| `d` | duration | milliseconds |
| `t` | time of day | `HH:MM:SS/dow` |
| `e` | enumeration | decimal |
| `c` | color | `rrggbbww/valid`, hex |

Why this mapping:

- **It survives renumbering.** It depends only on the two ids, which E-Home assigns once and keeps.
  Reordering rules or parameters, or inserting new ones, moves nothing. A mapping by index, or by
  a table of allocated slots, would have to be migrated on every edit.
- **Collisions are handled.**
  - Reading a slot whose logical key differs counts as no override.
  - `RuleSet::load()` refuses an image in which two persistence keys share a slot: every declared
    parameter and `<ruleId>/.state` of every rule with states (`limitExceeded` at `rules[i]` of
    the second, naming both). The check is part of the rule set's load, so every loader sees it.
    At 48 bits the chance for an image's few hundred keys is about 1 in 10^10. The unit tests use
    a real pair found by a cycle search.

**After a load**, every stored override that no current parameter holds is deleted: removed rules,
undeclared parameters, overrides that no longer fit, unreadable `rp.` entries.

- At boot, an override that no longer fits its declaration (type, range or choices) is reset with
  `parameterReset`. An override of an undeclared parameter is pruned silently.
- At a swap, an override of a kept parameter survives when it still fits; otherwise it is reset
  with `parameterReset`.

**Rule states** (spec 8.5) use the same store and slots under the logical key `<ruleId>/.state`
(a parameter id cannot start with a dot), the value a string: `stairs/.state=s:6f6e` for `on`.

- A transition, and the initial state at a start, marks the rule dirty and arms one event-loop
  timer 10 s ahead (`kStatePersistDelayMs`), unless it is armed already. When it fires, every dirty
  rule's state is written unless the store holds it already, so a state that changed and changed
  back within the window costs no write. When the timer cannot be armed the write happens at once.
- At boot, `install()` reads the stored state before the start: a declared state is taken without
  entry actions; an undeclared one, or one of a rule without states, logs `stateReset`, and the
  start enters `initialState`.
- After the load, the prune keeps a `.state` entry only for an existing rule with states whose
  value names a declared state.
- Parameter writes stay immediate (people change parameters rarely); state writes are delayed
  because transitions can be frequent.

## Labels

A rule's name, a parameter's label and a choice's label may be a language map (spec 2.2). The
builder resolves the text once (exact tag, base language, `en`, first entry, tags compared without
case) into the string pool, and stores the map, re-serialized as compact JSON, as a second string
(`Rule::name_json`, `Parameter::label_json`, `Choice::label_json`; 0 for a plain string).
`RuleSet::label_json()` returns it; the web API writes it raw, so the UI can resolve it in the
viewer's language with `label()` of `model.js`.

## Normalization

The loader compiles only normalized trees (spec 4.8), so the printer's text parses back to the
same nodes:

- a chain of unary minus over an integer, real or duration literal compiles into one literal
  (`folded_negation`);
- an enumeration literal compiles as an integer; an integer literal in 0..2^32-1 becomes an
  enumeration literal when its partner is an enumeration (`==`/`!=`, `if`, `coalesce`, `toInt`,
  `setParameter` to an enumeration parameter; `retype_enumeration`);
- a controlled literal compiles as the call `controlled` with two literal children;
- the canonical text of a time of day with a day of the week is `mon 07:30`.

## Image swap

The swap is by id (7.6):

- **Kept rules** keep:
  - their diagnostics;
  - their overrides (as above);
  - their running timers with the same id, with deadline and sequence;
  - their current state when the new image still has that state id. Otherwise `stateReset` is
    logged, and the start enters `initialState`.
- **Change records** carry over by datapoint id. A newly tracked datapoint starts from the model's
  update time.
- **Removed rules** lose their timers, and their overrides are deleted.

## Diagnostics

`Engine::diagnostics(rule, out)` returns spec 9.1:

- the enabled state, the firing counter and the error counter (logged warnings and errors, not
  `log` actions);
- the last trigger, as monotonic and wall time with a text such as `datapoint hall.presence`,
  `timer off` or `start boot`;
- the last action (`write hall.light = true`, `restartTimer off 3min`);
- the last error, with its code and text;
- the current state;
- the loop-guard counters.

`parameter_value()`, `parameter_overridden()`, `timer_remaining()` and `current_state()` give the
rest. The engine logs with the tag `hc.rule`: `<ruleId> <code>: <text>`.

## Interpretations

Points the spec leaves open, decided here (E-Home's interpreter should agree):

- **Errors counter**: `log` actions do not count; every logged warning or error with a code does.
- **`stateReset`**: logged whenever a kept rule's current state id is missing from the new image.
- **`state()` in a rule without states** is refused as `forbidden`.
- **A declared parameter named `enabled`** is refused as `duplicateId`.
- **A parameter of a type without parameters** (for example `raw`) is refused as
  `invalidParameter`.
- **Change times**:
  - a timer cascade's time is the clock at processing, which equals the deadline when the event
    loop is on time;
  - a datapoint's change time is the clock when the listener is told.
- **Persistence**: parameter writes go to the store immediately; NVS wear is bounded by how
  often people change parameters. State writes are delayed by 10 s and coalesced.
- **An ambiguous date-time literal** (the hour that occurs twice when summer time ends) has no
  summer-time flag and therefore means the standard-time occurrence.
- **Labels** fall back to `en` before the first entry, as the UI's `label()` does.
- **The loop-guard culprit** on a tie is the first rule in rule order.
- **More than 16 choices** are refused as `limitExceeded`.

## Tests

- **`rules_corpus_test.cpp`** runs the 41 corpus cases in the README's procedure. Time 0 is
  monotonic 1,000,000 ms. A reboot rebuilds the model and the engine, and keeps the settings. The
  wall clock is the device's `RulesClock` over a fake time service whose zone evaluates the case's
  POSIX TZ string itself (`PosixRuleZone` in `rules_test_support.h`), never the host's zone.
- **`rules_unit_test.cpp`**:
  - the printer round trip (every corpus expression is printed, parsed back by a small parser of
    `spec/rules-grammar.ebnf`, and compared structurally with the compiled nodes), plus the spec's
    examples and parenthesization cases;
  - every refusal reason of 10.1, and every limit of 10.2 at and beyond the limit;
  - the key mapping, its encoding, the collision slot, pruning, and the collision refusal;
  - the memory accounting of the 50-rule image;
  - no allocation while events are processed;
  - a self-triggering rule ending in `loopGuard`;
  - the diagnostics texts;
  - the four normalized shapes, the label resolution chain, the collision refusal of
    `RuleSet::load`, delayed and coalesced state writes without allocation;
  - `RulesClock` across both summer-time changes;
  - the facts of spec 7.6: values kept by a swap and read-on-start after the KNX adapter's
    reconfiguration.
