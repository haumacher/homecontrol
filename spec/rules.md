# Rules: model, expression language and semantics

*Specification of issue #11 (phase 3). Implemented by the C++ interpreter of the core (issue #12)
and the Java reference interpreter of E-Home (issue #13); both are tested against the conformance
corpus `spec/fixtures/rules/` (format in `spec/fixtures/rules/README.md`). The text grammar of
expressions is also in `spec/rules-grammar.ebnf`. Rules travel in the runtime image (issue #8);
this document defines the `rules` member of the image and everything the interpreters do with it.*

## 1. Introduction

A *rule* is engineered in E-Home and executed by the controller. It reacts to *events* (a datapoint
changed, a schedule fired, a timer expired, the rules started, a parameter changed), checks
*conditions* written in a small typed *expression language*, and performs *actions* (command a
datapoint, start or cancel a timer, change a parameter, write a log line). A rule may own *timers*,
*adjustable parameters* and the *states* of a small state machine.

### 1.1 Decisions of the author

These are the author's decisions; this specification implements them.

1. **Rules are declarative data, not scripts.** A rule is a structure of triggers, conditions,
   actions, timers and states; expressions have no loops, no assignments, no user-defined
   functions, and evaluate in bounded time without allocation.
2. **One semantics in two interpreters.** The C++ core runs rules on the device; E-Home's Java
   interpreter simulates them against the virtual installation before deployment. The shared
   conformance corpus keeps both identical: a semantic change lands with new corpus cases.
3. **Engineering happens in E-Home; the device executes.** The device never edits a rule. Its UI
   and API change only the *adjustable parameters* a rule declares (times, setpoints, the enable
   switch); everything else is refused with a reason.

### 1.2 Decisions taken in this specification

Each decision names the alternatives that were considered and why they lost. They favour the
long-term benefit of the project over the ease of implementation. The points the first draft left
to the author were decided on 2026-10-18 (issues #25 to #28); section 12 records each decision, and
the sections it touches state it as fact.

**D1. Expressions are a JSON AST in the image and a text syntax in E-Home.** The runtime image
stores every expression as a tree of JSON nodes (section 5); E-Home shows and edits a text syntax
(section 4) defined by a grammar, with exactly one canonical text per tree. *Alternatives:* text in
the image, parsed on both sides (two parsers to keep identical, a parser on a 512 KB device,
ambiguities discovered in the field); AST only, edited as a form tree in E-Home (unusable for real
expressions). The AST costs a printer and a parser in E-Home only; the device reads plain JSON it
must parse anyway.

**D2. The type system follows `hc::Value`, with one integer type, a duration type and "no value".**
Expression values have the types `boolean`, `integer`, `real`, `string`, `duration`, `timeOfDay`,
`date`, `dateTime`, `enumeration`, `controlled`, `color` and `raw` (section 3). Datapoints of the
value types `unsigned` and `signed` read as `integer` (64-bit signed); a command converts back
exactly or is refused. *No value* is the result of reading a datapoint whose quality is not good
(unknown or stale) and propagates like SQL NULL; a condition that has no value is not satisfied
and is logged once, never a crash. *Alternatives:* keep `unsigned` and `signed` distinct in
arithmetic (C-style promotion rules are a known source of wrong results; nothing in a house needs
both 32-bit ranges at once); a default value instead of no value (a missing sensor would silently
act as 0 °C); an exception that stops the rule (the same effect with less control). Typing is
static: E-Home and the device check every expression against the declared datapoint and parameter
types when the image is built and loaded, so a type error is an engineering error, never a
runtime surprise.

**D3. Evaluation is event-driven on the single-threaded event loop, with a queue and a loop
guard.** An event enqueues; actions that change datapoints enqueue further events (a queue, not
recursion); the events caused by one external event form a *cascade*, processed to completion
before the next external event; a cascade is refused after 32 rule firings caused by rule actions
(section 7). *Alternatives:* recursive evaluation (unbounded stack, order depends on nesting);
cyclic evaluation every N ms like a PLC (latency, wasted work, no natural notion of "changed");
a depth limit instead of a count (a wide cascade of legitimate firings would pass while a long
ping-pong between two rules is the same harm).

**D4. Timers and state machines are first-class.** A rule declares named timers and, optionally,
named states with entry and exit actions; its reactions can be transitions between states guarded
by conditions (section 2.6). Blinds, heating, staircase lights and presence logic need memory and
time; expressing them with helper datapoints and extra rules is fragile and invisible in the UI.
*Alternatives:* timers only, state kept in virtual datapoints (no entry/exit semantics, state
spread over several objects); a full statechart (hierarchy, parallel regions, history: too much to
specify twice and verify on a small device).

**D5. Adjustable parameters are declared, typed, bounded and persisted by rule id and parameter
id.** A parameter declares its type, range or choices, unit, default and label; a value set by the
inhabitant persists on the device under `<ruleId>/<parameterId>` and survives a new image while the
declaration still exists with a compatible type and the value is still valid; otherwise it is reset
to the new default with a log line (section 8). Only values the inhabitant (or a rule action) set
are persisted, and only while they differ from the default: a parameter never touched, or set back
to its default, follows the default of each new image. The adjustable members of the schedule
entries follow the same rule (`spec/schedules.md` S4, 4.2). *Alternatives:*
persist every value (E-Home could never change a default again); key by position (any edit in E-Home
would scramble the stored values); expose arbitrary datapoints for editing (contradicts decision 3
of the author).

**D6. Every rule has a stable id, a name, an enabled switch and a diagnostics contract.** E-Home
assigns the id; it never changes for the life of the rule and is the key for state, timers and
parameters across image swaps. The enabled flag is an adjustable boolean parameter named `enabled`
unless the rule declares it fixed. Every interpreter provides, per rule, the last trigger, the last
action and the last error with their times (section 9). *Alternatives:* names as keys (renaming
would lose the inhabitant's settings); diagnostics as an optional extra (the first question about
an automation in a house is always "why did it (not) do that?").

**D7. A rule is a list of reactions sharing timers, parameters and states.** A *reaction* is
"when (triggers) if (condition) then (actions) else (actions)"; in a rule with states, a reaction
can name the states it applies in and the state it leads to, which makes it a transition. One
structure covers simple event-condition-action rules, rules that need a timer from two sides (the
presence light: one reaction starts the timer, another reacts to its expiry) and state machines.
*Alternatives:* a single trigger-condition-action triple per rule (timers could not be shared
between the reaction that starts them and the one that handles their expiry); separate classes for
plain rules and state machines (two structures with the same parts).

**D8. Datapoints are referenced by name in the image.** The image loader resolves names to ids
when it loads; a dangling name refuses the image. *Alternatives:* numeric ids (the AST would be
unreadable in a diff and break when E-Home reorders datapoints).

**D9. Time is monotonic for durations and local wall time for the calendar.** Timers, `sinceChange`
and the loop run on the monotonic clock; `now()`, `time()`, `weekday()` and the sun functions use
the local wall time of the time service (issue #9) and have no value while the wall clock is not
synchronized. Within one cascade every expression sees the same time. Date-time arithmetic and
comparisons work on the instant, so they are correct across a change of summer time (4.5, 4.6).

### 1.3 Terms

| Term | Definition |
|---|---|
| rule | A unit of automation with a stable id: parameters, timers, optional states and reactions (section 2). |
| reaction | Triggers, an optional condition, actions and optional else-actions; with states also `from` and `to`. |
| trigger | Describes which events activate a reaction (section 2.3). |
| event | Something that happened: a datapoint update, a schedule firing, a timer expiry, a rule start, a parameter change. |
| external event | An event not caused by a rule action in the current cascade: a report or command from outside the rule engine, a schedule firing, a timer expiry, a rule start, a parameter change from the UI or API. |
| derived event | A datapoint update or parameter change caused by a rule action. |
| cascade | An external event and all events derived from it, processed to completion (section 7). |
| firing | The activation of one rule by one event: at least one trigger of the rule matched (section 7.2). |
| condition | A boolean expression; *satisfied* only when it evaluates to `true`. |
| no value | The result of an expression that cannot be computed from what is known (section 3.3). |
| parameter | A named, typed value of a rule, adjustable on the device when declared so (section 8). |
| timer | A named one-shot countdown of a rule on the monotonic clock (section 2.5). |
| state | A named state of a rule with states; the rule is in exactly one of its states (section 2.6). |
| image swap | Replacing the loaded runtime image by a new one without a reboot (section 7.6). |

## 2. The data model

The runtime image holds a member `rules`, an array of rule objects. All member names are
camelCase and chosen to be usable unchanged as attribute names of E-Home's model (the classes
`Rule`, `RuleParameter`, `RuleTimer`, `RuleState`, `Reaction`, `Trigger`, `Action` and their
subclasses). Members not defined here are refused by the loader, except `description` and
`comment`, which every object may carry and the interpreters ignore. The values of `kind` are
scoped to the object family the position admits (trigger, action, expression node): a trigger of
kind `datapoint` and an expression node of kind `datapoint` are different things.

### 2.1 Identifiers

| What | Form | Unique within |
|---|---|---|
| rule id | 1..32 characters of `A-Z a-z 0-9 _ -` | the image |
| parameter id | 1..24 characters of `A-Z a-z 0-9 _ -` (those of a rule id), not a keyword (schema 1.5, issue #81) | the rule |
| timer and state id | an *identifier*: 1..24 characters, `[A-Za-z_][A-Za-z0-9_]*`, not a keyword | the rule (each kind separately) |
| schedule id | the id of a schedule entry of the image (issue #10), same form as a rule id | the image |
| datapoint name | as in the datapoint model: 1..63 printable bytes | the image |

The keywords are `and`, `or`, `not`, `true` and `false`. A parameter id that is not an identifier
(`max-helligkeit`, `2nd`) is written `$"max-helligkeit"` in the text (4.3); the length of 24 keeps
the parameter's datapoint name `rule/<ruleId>/<parameterId>` within 63 bytes (5 + 32 + 1 + 24 = 62,
8.6), and `/` stays excluded because it separates the parts of that name and of the persistence key
(8.4). Umlauts and spaces belong in the label. An image with a parameter id that is not an
identifier needs a firmware that reads schema 1.5 (`spec/runtime-image.md` section 8): an older one
refuses it.

The parameter id `enabled` is reserved for the enable switch (section 8.3). Datapoint names that
start with `rule/` are reserved for the datapoints of the rule parameters (section 8.6), those that
start with `schedule/` for the adjustable schedule times (`spec/schedules.md` 2.2), those that start
with `block/` for the datapoints of the blocks (`spec/blocks.md` 6); the image declares none.

### 2.2 Rule

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | rule id | yes | Stable identity assigned by E-Home. |
| `name` | label | yes | Shown in the UI, the log and the diagnostics. |
| `description` | string | no | For people; ignored. |
| `enabled` | boolean | no, `true` | Default of the enable switch. |
| `enabledAdjustable` | boolean | no, `true` | Whether the inhabitant may switch the rule on and off (section 8.3). |
| `parameters` | array of parameter | no | Section 8. |
| `timers` | array of timer | no | Section 2.5. |
| `states` | array of state | no | Section 2.6; present and non-empty makes this a *rule with states*. |
| `initialState` | state id | with `states` | The state the rule enters when it starts without a preserved state. |
| `reactions` | array of reaction | yes, 1 or more | Section 2.4. |

A *label* (the rule's `name`, a parameter's `label`, a choice's `label`) is a string of 1..63 bytes
or an object from language tags to such strings, as in the runtime image
(`spec/runtime-image.md`): `{"en": "Staircase", "de": "Treppenhaus"}`. A language tag has 2..8
characters of `A-Z a-z 0-9 -`; an object has 1..16 entries. The device resolves a label once, when
the image is loaded, in its UI language (the setting `ui.lang`, default `en`): the entry whose tag
equals the language (letters compared without case), else the entry of its base language (the part
before `-`: `de` for `de-CH`), else `en`, else the first entry. The resolved text appears in the log
and the diagnostics; the API passes the label through as the image gives it, and the UI resolves it
in the viewer's language. E-Home's simulation resolves it the same way in the engineer's language.
(Decision 11, section 12.)

### 2.3 Triggers

A trigger is an object with a `kind`:

| `kind` | Members | Matches |
|---|---|---|
| `datapoint` | `datapoint` (name); `on`: `change` (default) or `update`; `origin`: `any` (default), `report` or `command` | An update of the datapoint. With `change`, only when value or quality differ from before the update (`DatapointUpdate::changed()`); with `update`, every update, also a repeated report of the same value. `origin` restricts to updates reported by the technology (`report`, also a change to stale) or commanded by the application (`command`: a rule or the UI). |
| `match` | `condition` (expression) | The condition becomes true: it evaluates to `true` now and did not (false or no value) at its previous evaluation. Section 7.4. |
| `schedule` | `schedule` (schedule id) | The scheduler fired this schedule entry (issue #10). |
| `timer` | `timer` (timer id of this rule) | The timer expired. |
| `start` | `reasons`: array of `boot`, `image`, `enable` (default: all three) | The rule starts: after a boot, after an image swap (every rule of the new image), when the rule is enabled. |
| `parameter` | `parameter` (parameter id of this rule) | The parameter's value changed (from the UI or API, or by a `setParameter` action). |
| `silence` | `datapoint` (name); `after` (expression of type duration) | The datapoint has had no update for the duration `after`: once per silent period, until the next update re-arms it. Section 7.11. |

A `match` condition may reference datapoints, parameters of the rule, literals and the pure
functions of section 4.7 (no time, history, timer or state functions; refused at load), because it
is evaluated only when one of its inputs changes. The `after` of a `silence` trigger may reference
parameters of the rule, literals and the pure functions (no datapoints and no time, history, timer
or state functions; refused at load), because it is evaluated when the period is armed.

### 2.4 Reaction

| Member | Type | Required | Meaning |
|---|---|---|---|
| `triggers` | array of trigger | yes, 1 or more | Any one matching activates the reaction. |
| `condition` | expression of type boolean | no (always satisfied) | Checked when the reaction is activated. |
| `actions` | array of action | yes (may be empty) | Run in order when the condition is satisfied. |
| `elseActions` | array of action | no | Run in order when the condition evaluates to `false`; not when it has no value. Not allowed together with `to`. |
| `from` | array of state ids | no (all states) | Only in a rule with states: the reaction applies only while the rule is in one of these states. |
| `to` | state id | no | Only in a rule with states: after the actions, the rule changes to this state (a *transition*, section 2.6). |

### 2.5 Timer

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | identifier | yes | |
| `duration` | expression of type duration | yes | Evaluated when a `startTimer` or `restartTimer` action without its own `duration` runs. |

A timer is *running* or *stopped*; it starts stopped. A running timer has a deadline on the
monotonic clock; when the deadline passes the timer stops and a timer-expired event is processed
as an external event (its own cascade). A timer duration must lie in 100 ms .. 30 d; another value
refuses the starting action (`timerRefused`).

### 2.6 States and transitions

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | identifier | yes | |
| `entry` | array of action | no | Run when the rule enters the state. |
| `exit` | array of action | no | Run when the rule leaves the state. |

A reaction with `to` is a transition. When it runs: the exit actions of the current state, the
reaction's actions, the change of the current state to `to`, the entry actions of `to`. A
transition to the current state (a *self-transition*) leaves and re-enters the state, running exit
and entry actions. A reaction without `to` in a rule with states is an *internal reaction*: it runs
its actions and the state does not change.

When a rule with states starts without a preserved state (a boot without a persisted state, a new
rule, a state not preserved by an image swap), its current state becomes `initialState` and that
state's entry actions run (section 7.5). A preserved state is one kept by an image swap (7.6) or
restored at boot from the persisted state (8.5); the rule takes it without running entry actions. There are no other ways to change the state: there is no "set state" action, because a state
change must come from a guarded transition with defined exit and entry actions.

### 2.7 Actions

An action is an object with a `kind`:

| `kind` | Members | Effect |
|---|---|---|
| `write` | `datapoint` (name); `value` (expression); `onlyOnChange` (boolean, default `false`) | Commands the datapoint with the value (section 7.3). With `onlyOnChange`, nothing happens when the datapoint's quality is good and its value equals the converted value. |
| `startTimer` | `timer` (id); `duration` (expression, optional) | Starts the timer if it is stopped; no effect if it is running. |
| `restartTimer` | `timer` (id); `duration` (expression, optional) | Starts the timer, or restarts a running one with a new deadline. |
| `cancelTimer` | `timer` (id) | Stops the timer; no effect if it is stopped. The timer does not fire. |
| `setParameter` | `parameter` (id of this rule); `value` (expression) | Sets the parameter like the UI does (validated, persisted, a parameter-changed event when the value differs). |
| `read` | `datapoint` (name) | Asks the technology owning the datapoint for its current value (section 7.12). The answer arrives later as an ordinary update of the datapoint; the action itself changes no value. |
| `log` | `level`: `info`, `warning` or `error`; `message`: array of strings and expressions | Writes a log line: the strings verbatim and each expression formatted (section 4.9), concatenated. |

Actions of one list run in order; each sees the effects of the previous ones (a written value, a
started timer). A failing action (no value, refusal) is logged and skipped; the following actions
still run.

## 3. Types and values

### 3.1 Value types

| Expression type | Values | Read from datapoints of type | JSON literal value |
|---|---|---|---|
| `boolean` | `true`, `false` | `boolean` | `true` / `false` |
| `integer` | 64-bit signed | `unsigned`, `signed` | a JSON number without fraction, at most 2^53-1 in magnitude |
| `real` | IEEE 754 double, finite | `real` | a JSON number |
| `string` | UTF-8, at most 28 bytes | `string` | a JSON string |
| `duration` | signed milliseconds (64-bit) | — | integer milliseconds |
| `timeOfDay` | hour, minute, second, day of week 0..7 (0: none) | `timeOfDay` | `{"hour", "minute", "second"}`, optional `"dayOfWeek"` (default 0) |
| `date` | year, month, day | `date` | `{"year", "month", "day"}` |
| `dateTime` | date, time of day, day of week, flags | `dateTime` | `{"year", "month", "day", "hour", "minute", "second"}` |
| `enumeration` | element number 0..2^32-1 | `enumeration` | a JSON number |
| `controlled` | control bit, number 0..255 | `controlled` | `{"control": bool, "value": n}` |
| `color` | red, green, blue, white 0..255, valid channels | `color` | `{"red", "green", "blue"}` (RGB valid), optional `"white"` (all four valid) |
| `raw` | at most 14 bytes | `raw` | hex octets separated by single spaces, `"0A FF"` |

The datapoint value types are those of `doc/datapoints.md` written in camelCase (`boolean`,
`unsigned`, `signed`, `real`, `string`, `timeOfDay`, `date`, `dateTime`, `enumeration`,
`controlled`, `color`, `raw`). A `dateTime` is a local time; it denotes an instant through the
device's time zone (4.5, 4.6). Its *summer-time flag* (KNX SUTI) tells which of the two
occurrences of a local time that occurs twice (the hour before the end of summer time) is meant:
set, the occurrence in summer time; clear, the one in standard time. A `dateTime` read from a
datapoint keeps its fields and flags as received; comparisons and arithmetic use only the
summer-time flag. A literal has no flags (an ambiguous literal is the standard-time occurrence);
`now()` and the result of arithmetic carry the summer-time flag when summer time is in effect at
their instant; all have the day of week of their date.

### 3.2 Conversions

There are only these implicit conversions:

- `integer` to `real` where an operator or function mixes the two (exact up to 2^53).
- On reading: `unsigned` and `signed` to `integer`.
- On writing (a `write` action, a parameter value): to the target type as listed below. A
  conversion that is not exact at run time refuses the write (`writeRefused`) or the parameter
  value (`parameterRefused`).

| Expression type | Datapoint or parameter types it may be written to |
|---|---|
| `boolean` | `boolean` |
| `integer` | `unsigned` (0..4294967295), `signed` (-2147483648..2147483647), `real`, `enumeration` (0..4294967295), parameter `integer` and `real` |
| `real` | `real`; `unsigned`, `signed`, `enumeration`, parameter `integer` only when integral and in range |
| `enumeration` | `enumeration`, `unsigned`, `signed` |
| `duration` | parameter `duration` (no datapoint holds a duration; write `d / 1s` or `round(d / 1min)`) |
| every other type | the same type |

A combination outside the table is a type error and refuses the image (section 10). Explicit
conversions are functions (section 4.7): `round`, `floor`, `ceil`, `trunc` (real to integer),
`toInt` (enumeration or boolean to integer), `toString`.

### 3.3 No value

*No value* is not a type; any expression of any type may have no value. It arises from:

- a datapoint reference whose datapoint has quality `unknown` or `stale`;
- a time function while the wall clock is not synchronized, a sun function without a result
  (`sunrise()` and the like when the event does not occur, `sunAzimuth()` and `sunElevation()`
  without a location);
- `remaining(t)` of a stopped timer; `weekday(t)` of a time of day without a day;
- an *evaluation error*: integer overflow, division or remainder by zero, a result that is not
  a finite real, a string longer than 28 bytes, a function argument outside its domain (`clamp`
  with `low > high`, `hysteresis` with `low >= high`, `roundTo` with a step not above 0, a color
  level or controlled number outside 0..255). An evaluation error also logs `evalError` (once,
  section 9.2).

Propagation: an operator or function with an argument that has no value has no value, except:

| Construct | With no value |
|---|---|
| `a and b` | `false` if either operand is `false`, otherwise no value if either has no value |
| `a or b` | `true` if either operand is `true`, otherwise no value if either has no value |
| `known(x)` | `false` |
| `coalesce(x, y)` | `y` when `x` has no value |
| `if(c, a, b)` | no value when `c` has no value; only the selected branch is evaluated |
| `hysteresis(x, low, high, previous)` | `previous` is needed only when `low < x < high` |
| `changedWithin(@d, t)` | `false` when no change of the datapoint is known |
| `quality(@d)`, `running(t)`, `lastKnown(@d)` | never caused by the datapoint's quality or the timer being stopped |

Where no value meets a decision:

- A reaction's condition with no value is *not satisfied*: neither `actions` nor `elseActions` run.
  The interpreter logs `conditionNoValue` once (section 9.2).
- A transition's condition with no value: the transition is not taken (and logged the same way);
  the next reaction is considered.
- A `match` trigger with no value counts as not true.
- A `write` whose value has no value does not command anything and logs `valueNoValue` (once).
- A timer duration with no value refuses the timer action (`timerRefused`).
- A log message formats no value as `none`.

## 4. The expression language

### 4.1 Overview

An expression is side-effect free and evaluates to one value (or no value). It is statically typed:
the type of every node is known from the image (datapoint types, parameter types, literal types),
and E-Home and the loader refuse an expression with a type error. Evaluation needs no allocation:
the interpreters size an evaluation stack from the maximum expression depth (section 10).

### 4.2 Literals

| Literal | Examples | Type | Canonical text |
|---|---|---|---|
| boolean | `true`, `false` | boolean | as written |
| integer | `0`, `42`, `-7` | integer | decimal, no leading zeros; a negative literal is a minus folded into the literal |
| real | `21.5`, `0.25`, `1.5e-5` | real | the shortest decimal that reads back as the same double, with at least one digit after the point; exponent form (`1.5e20`, `1.0e-5`) when the magnitude is at least 1e15 or below 1e-4 |
| duration | `500ms`, `45s`, `30min`, `1h30min`, `2d` | duration | normalized into `d h min s ms`, largest unit first, zero parts omitted, `0s` for zero |
| time of day | `20:30`, `06:05:30`, `6:05`, `mon 07:30` | timeOfDay (day 0, or the day written) | two-digit hour, `HH:MM`, `:SS` only when the seconds are not zero; with a day of the week its name first and one space: `mon`, `tue`, `wed`, `thu`, `fri`, `sat`, `sun` (`mon 07:30`) |
| date | `2026-10-17` | date | `YYYY-MM-DD` |
| date and time | `2026-10-17T20:30`, `2026-10-17T20:30:15` | dateTime | `YYYY-MM-DDTHH:MM`, `:SS` only when not zero |
| string | `"Hall"`, `"say \"hi\""` | string | double quotes; `\"`, `\\`, `\n`, `\t` escaped, nothing else |
| color | `#ff8000`, `#ff8000c0` | color | lowercase; six digits (RGB valid) or eight (RGBW, all valid) |
| raw | `x"0AFF"`, `x""` | raw | uppercase hex, no spaces |

The text has no literal for enumerations (compare with integers, `@mode == 3`; decision 6), for
`controlled` values (`controlled(true, 3)` is a call) or for no value. The AST has literals of type
`enumeration` and `controlled` (3.1); the loader normalizes them (4.8), so their text is the
number and the call.

The lexer takes the longest token; `2026-10-17` is a date, not a subtraction (canonical text
always puts spaces around binary operators), `30min` is a duration, `30 min` an error.

### 4.3 References

| Text | Meaning | Type |
|---|---|---|
| `@living.light`, `@"Living room light 1"` | The current value of the datapoint (no value unless the quality is good). A name that is not an identifier with dot-separated parts is written as a string. | from the datapoint type (section 3.1) |
| `$timeout`, `$"max-helligkeit"` | The current value of the rule's parameter. An id that is not an identifier (2.1) is written as a string, with the escapes of a string literal (4.2), like a datapoint name. | the parameter's type |
| `running(off)`, `remaining(off)` | Whether the rule's timer runs; the time until it expires (no value when stopped). | boolean; duration |
| `state()`, `state("stairs")` | The current state id of this rule, or of the rule with the given id (a literal; that rule must have states). | string |

Parameters, timers and states are always those of the rule the expression belongs to; another
rule's state is readable by its id, and another rule's parameters through their datapoints
(`@"rule/heat/setpoint"`, 8.6); nothing else of another rule is.

### 4.4 Operators

From the loosest to the tightest binding:

| Level | Operators | Associativity | Operand types → result |
|---|---|---|---|
| 1 | `or` | left | boolean, boolean → boolean (three-valued, section 3.3; the right operand is not evaluated when the left is `true`) |
| 2 | `and` | left | boolean, boolean → boolean (three-valued; the right operand is not evaluated when the left is `false`) |
| 3 | `not` (prefix) | — | boolean → boolean |
| 4 | `==` `!=` `<` `<=` `>` `>=` | none (`a < b < c` is a syntax error) | see 4.5 → boolean |
| 5 | `+` `-` | left | see 4.6 |
| 6 | `*` `/` `%` | left | see 4.6 |
| 7 | `-` (prefix) | — | integer, real, duration → same |

Parentheses group. `not a == b` is `not (a == b)`; `-x * y` is `(-x) * y`.

### 4.5 Comparisons

| Operand types | `==` `!=` | `<` `<=` `>` `>=` |
|---|---|---|
| integer, real (mixed allowed) | numeric (integer converted to real when mixed) | numeric |
| duration | yes | yes |
| timeOfDay | hour, minute, second (the day of week is ignored) | by seconds since midnight |
| date | the fields | chronological |
| dateTime | the instants (the local time through the device's time zone, an ambiguous time by its summer-time flag; the day of week and the other flags ignored) | by instant |
| string | bytes | bytewise lexicographic (UTF-8) |
| boolean, enumeration, controlled, color, raw | all fields (color: levels and valid channels) | type error |
| enumeration with integer | the element number with the integer | type error |

Any other combination is a type error. Reals compare exactly (E-Home should warn about `==` on
reals).

### 4.6 Arithmetic

| Expression | Types | Result |
|---|---|---|
| `a + b`, `a - b` | integer, integer | integer (overflow: evaluation error) |
| | integer/real mixed, real | real |
| | duration, duration | duration |
| | timeOfDay ± duration | timeOfDay, modulo 24 h; the duration is truncated to whole seconds toward zero first; the day of week is kept |
| | dateTime ± duration | dateTime: the local time converted to its UTC instant (an ambiguous time by its summer-time flag, a time in the gap at the start of summer time as the end of the gap), the duration truncated to whole seconds toward zero added, the instant converted back to local time with its summer-time flag; a year outside 1900..2155 is an evaluation error. `now() + 1h` at 01:30 CEST on the day summer time ends is 02:30 CEST, at 02:30 CEST it is 02:30 CET; `2026-10-24T12:00 + 24h` in Europe/Berlin is `2026-10-25T11:00` |
| `a - b` | timeOfDay − timeOfDay | duration, −24 h < d < 24 h, no wrap (`(a - b + 1d) % 1d` wraps) |
| | dateTime − dateTime | duration between the instants, whole seconds |
| | date − date | duration, whole days of 24 h |
| `a + b` | string, string | string (longer than 28 bytes: evaluation error) |
| `a * b` | integer, integer | integer |
| | integer/real mixed, real | real |
| | duration × integer or real, integer or real × duration | duration, rounded to the millisecond, half away from zero |
| `a / b` | integer or real, integer or real | **real**, also for two integers (`7 / 2` is `3.5`); division by zero: evaluation error |
| | duration / integer or real | duration, rounded as for `*` |
| | duration / duration | real |
| `a % b` | integer, integer | integer, truncated remainder (sign of `a`, `-7 % 3` is `-1`) |
| | duration, duration | duration, likewise |
| `-a` | integer, real, duration | same type |

Every other combination is a type error. A real result that is not finite is an evaluation error.
Numbers become durations by multiplication (`$minutes * 1min`) and durations numbers by division
(`d / 1s`).

### 4.7 Function library

Arguments are evaluated left to right; `if`, `coalesce`, `and`, `or` and `hysteresis` evaluate only
what they need. "numeric" is integer or real (mixed promotes to real); "ordered" is any type with
`<` (section 4.5), all arguments of one kind. Unless the table says otherwise, a function has no
value when an argument has no value.

**Logic and no value**

| Function | Types | Result |
|---|---|---|
| `if(c, a, b)` | boolean, T, T | `a` if `c` is true, `b` if false, no value if `c` has none |
| `known(x)` | any | `true` if `x` has a value |
| `coalesce(x, y)` | T, T | `x` if it has a value, else `y` |
| `between(x, low, high)` | ordered | `low <= x and x <= high` (false when `low > high`) |
| `hysteresis(x, low, high, previous)` | numeric or duration ×3, boolean | `true` if `x >= high`, `false` if `x <= low`, otherwise `previous`; `low >= high` is an evaluation error |

`hysteresis` keeps no hidden memory: the previous result is an argument, usually the output
datapoint (`hysteresis(@fan.humidity, 60, 70, @fan.on)` switches on at 70, off at 60).

**Numbers**

| Function | Types | Result |
|---|---|---|
| `abs(x)` | numeric, duration | same type |
| `min(a, b, ...)`, `max(a, b, ...)` | 2..8 ordered arguments | the smallest, largest (mixed integer/real gives real) |
| `clamp(x, low, high)` | ordered | `max(low, min(x, high))`; `low > high` is an evaluation error |
| `round(x)` | real | integer, half away from zero (`round(-2.5)` is `-3`) |
| `floor(x)`, `ceil(x)`, `trunc(x)` | real | integer |
| `roundTo(x, step)` | numeric, numeric > 0 | real: `round(x / step) * step` (`roundTo(21.26, 0.5)` is `21.5`) |
| `toInt(x)` | enumeration, boolean | integer (`true` is 1) |

`round`, `floor`, `ceil` and `trunc` accept an integer argument and return it unchanged.

**Time** (wall clock, local time; no value while not synchronized)

| Function | Types | Result |
|---|---|---|
| `now()` | — | dateTime of the cascade |
| `today()` | — | date |
| `time()` | — | timeOfDay, with the day of week |
| `weekday()`, `weekday(x)` | —, date / dateTime / timeOfDay | integer 1 (Monday) .. 7 (Sunday); no value for a time of day without a day |
| `hour(x)`, `minute(x)`, `second(x)` | timeOfDay, dateTime | integer |
| `year(x)`, `month(x)`, `day(x)` | date, dateTime | integer |
| `dateOf(x)`, `timeOf(x)` | dateTime | date; timeOfDay (with the day of week) |
| `inWindow(t, from, until)` | timeOfDay or dateTime, timeOfDay, timeOfDay | `from <= t < until` when `from < until`; `t >= from or t < until` when `from > until` (across midnight); `false` when equal; a dateTime uses its time of day |
| `sunrise()`, `sunset()`, `dawn()`, `dusk()` | — | timeOfDay of today's event (dawn and dusk: civil twilight) from the time service; no value without coordinates or when the event does not occur |
| `sunAzimuth()`, `sunElevation()` | — | real: the sun's azimuth (degrees from north through east, 0..360) and elevation (degrees above the horizon, -90..90, geometric: no refraction) at the cascade's wall time and the controller's location; no value without a location |

**Datapoint history** (the first argument must be a datapoint reference)

| Function | Types | Result |
|---|---|---|
| `sinceUpdate(@d)` | any datapoint | duration since the datapoint's last update; no value before the first |
| `sinceChange(@d)` | any datapoint | duration since the last change of value or quality (section 7.7); no value when none is known |
| `changedWithin(@d, t)` | any datapoint, duration | `sinceChange(@d) <= t`; `false` when no change is known |
| `lastKnown(@d)` | any datapoint | the value also when stale; no value only when unknown |
| `quality(@d)` | any datapoint | string `"good"`, `"stale"` or `"unknown"` |

**Timers and states**: `running(t)`, `remaining(t)`, `state()`, `state("id")` (section 4.3).

**Strings**

| Function | Types | Result |
|---|---|---|
| `length(s)` | string | integer, bytes |
| `contains(s, part)`, `startsWith(s, part)`, `endsWith(s, part)` | string, string | boolean |
| `toString(x)` | any | string formatted as in section 4.9 (longer than 28 bytes: evaluation error) |

**Colors and controlled values**

| Function | Types | Result |
|---|---|---|
| `rgb(r, g, b)`, `rgbw(r, g, b, w)` | integers 0..255 (outside: evaluation error) | color (RGB valid; all four valid) |
| `red(c)`, `green(c)`, `blue(c)`, `white(c)` | color | integer; no value when the channel is not valid |
| `controlled(bit, n)` | boolean, integer 0..255 | controlled |
| `controlBit(x)`, `controlValue(x)` | controlled | boolean; integer |

`sunAzimuth()` and `sunElevation()` are the two halves of the blocks' `sun()` (`spec/blocks.md`
B21): the position by the algorithm of `spec/blocks.md` 13.2 on the exact trigonometry of 13.1, so
the controller and E-Home's simulation agree to the bit, at the UTC instant of `now()` in whole
seconds (the milliseconds dropped; in the repeated hour at the end of summer time the clock's own
instant) and the location of `spec/blocks.md` 5.5 (the image's `settings.location`, otherwise the
device's setting). The position is computed once per wall second and shared by every rule and
block of the cascades in that second, so both functions and `sun()` of one cascade describe the
same position. Near the horizon the visible sun is up to 0.6° higher than the geometric elevation
(0.1° at 10°).

**Following the sun.** The sun's position is a function of the wall clock, not a datapoint: no
update says that it moved, so no trigger matches it, and like the other time functions it is
refused in a `match` condition (2.3, decision 13; case 63). A rule that follows the sun evaluates
it on a timer that restarts itself, started by the `start` trigger; with `onlyOnChange` it commands
only the changes (case 64):

```
rule facade "Sun on the facade"
  timer tick = 5min
  when start | timer tick
    then restartTimer tick; write facade.sun := between(sunAzimuth(), 90.0, 200.0) and sunElevation() > 10.0 (onlyOnChange)
```

A period of a few minutes suits facades and windows (the sun moves 1° in 4 minutes; its azimuth
near noon in summer about twice as fast); a shading decision with hysteresis, lock times and weather
protection is the sun shading block's (`spec/blocks.md` 18), which runs on the same position.

The pure functions allowed in a `match` trigger are all of the above except the time, history,
timer and state functions; `sunAzimuth()` and `sunElevation()` are time functions.

### 4.8 Canonical text

E-Home prints every AST in exactly one way, and parsing the printed text gives the same AST:

- binary operators with one space on each side; `not` followed by one space; prefix `-` without a
  space; `f(a, b)` with a space after each comma; references and literals as in 4.2 and 4.3;
- a datapoint reference bare (`@hall.light`) when its name is an identifier with dot-separated
  parts, else quoted (`@"1/2/3"`); a parameter reference bare (`$timeout`) when its id is an
  identifier, else quoted (`$"max-helligkeit"`); inside the quotes `"` and `\` are escaped
  (`\"`, `\\`), as in a string literal;
- parentheses only where needed: around an operand of lower level than its operator; around the
  right operand of a left-associative operator at the same level (`a - (b - c)`, `a or (b or c)`);
  around a comparison that is an operand of a comparison; around the operand of prefix `-` when it
  is not a literal, reference or call, or when its text starts with `-` (`-(-5)`);
- a minus applied directly to an integer, real or duration literal is part of the literal (the AST
  has a negative literal, not a `unary` node).

The round trip holds on *normalized* trees: printing a normalized tree and parsing the text gives
the same tree. The loader normalizes every expression of the image before it checks and stores
it, and E-Home's parser produces only normalized trees (decision 6, issue #34):

1. A `unary` minus whose operand is an integer, real or duration literal, also through a chain of
   minus nodes, is a literal with the sign applied (`-(-5)` is the literal `5`, `-1.5` the literal
   `-1.5`); a minus over anything else stays a `unary` node.
2. An integer literal in 0..2^32-1 that is the partner of an enumeration is an enumeration literal:
   the other operand of `==` or `!=`, the other value of `if` or `coalesce`, the argument of
   `toInt`, the value of `setParameter` to an enumeration parameter. An enumeration literal
   anywhere else is an integer literal. The text of both is the number.
3. A `controlled` literal is the call `controlled(b, n)` with a boolean and an integer literal.
4. A time-of-day literal with a day of the week prints as `mon 07:30` (4.2), in expressions and in
   formatted values (4.9).

The limits of 10.2 apply to the normalized tree.

### 4.9 Formatting values

Used by `log` messages and `toString`:

| Type | Format |
|---|---|
| boolean | `true`, `false` |
| integer, enumeration | decimal |
| real | the exact binary value rounded to 6 decimals, a tie away from zero (`0.0078125` gives `0.007813`; C's `printf` rounds ties to even and cannot be used as is), then trailing zeros and a trailing point removed: `21.5`, `21`, `0.333333`, `-0.5`; a result of zero is `0`, never `-0` |
| duration, timeOfDay, date, dateTime, color, raw | the canonical literal text (4.2); a time of day with a day of the week is `mon 07:30` |
| string | the bytes, without quotes |
| controlled | `controlled(true, 3)` |
| no value | `none` |

A formatted log message longer than 200 bytes is cut at a character boundary to at most 197 bytes
and `...` is appended.

## 5. The JSON AST

Every expression in the image (a condition, a value, a duration, a log message part) is one node
object. The member `kind` selects the node kind; the members are exact, others are refused.

| `kind` | Members | Text form |
|---|---|---|
| `literal` | `type` (an expression type of 3.1), `value` (its JSON literal value, 3.1) | a literal (4.2) |
| `datapoint` | `name` (datapoint name) | `@name`, `@"name"` |
| `parameter` | `id` (parameter id of the rule) | `$id` |
| `timer` | `id` (timer id of the rule) | the identifier inside `running(...)` or `remaining(...)`; valid only as their argument |
| `unary` | `op` (`-` or `not`), `operand` (node) | `-x`, `not x` |
| `binary` | `op` (`or`, `and`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `+`, `-`, `*`, `/`, `%`), `left`, `right` (nodes) | `a op b` |
| `call` | `function` (a name of 4.7), `args` (array of nodes, possibly empty) | `f(a, b)` |

The parser builds left-associative chains as left-nested `binary` nodes (`a - b - c` is
`binary(-, binary(-, a, b), c)`), folds a minus before an integer, real or duration literal into the
literal, and drops parentheses. A real literal always has `"type": "real"` (its JSON value may look
like an integer: `{"kind": "literal", "type": "real", "value": 21}` is 21.0). `state()` and
`state("stairs")` are calls with no argument and with one string literal.

Example: `@hall.presence and (@hall.light or @hall.lux < $darkBelow)` is

```json
{"kind": "binary", "op": "and",
 "left": {"kind": "datapoint", "name": "hall.presence"},
 "right": {"kind": "binary", "op": "or",
  "left": {"kind": "datapoint", "name": "hall.light"},
  "right": {"kind": "binary", "op": "<",
   "left": {"kind": "datapoint", "name": "hall.lux"},
   "right": {"kind": "parameter", "id": "darkBelow"}}}}
```

and `remaining(off) > 1min30s` is

```json
{"kind": "binary", "op": ">",
 "left": {"kind": "call", "function": "remaining", "args": [{"kind": "timer", "id": "off"}]},
 "right": {"kind": "literal", "type": "duration", "value": 90000}}
```

A log message is an array whose elements are JSON strings (verbatim text) or nodes:
`["Light off after ", {"kind": "parameter", "id": "timeout"}]`. E-Home shows it as a string with
`{expression}` placeholders (`Light off after {$timeout}`); `{{` and `}}` stand for literal braces.

## 6. The `rules` member

The runtime image (issue #8) carries `"rules": [ ... ]`. The order of the array is significant: it is
the *rule order* in which rules are considered for every event (section 7.2), and the order of
their start cascades. E-Home exports rules in an order the author can see and change.

## 7. Runtime semantics

### 7.1 Events and cascades

The rule engine runs on the event loop's thread and processes one *cascade* at a time:

1. An external event arrives: the datapoint model reports an update not caused by a rule action (a
   telegram, a device report, a stale report, a command from the UI or API), the scheduler fires an
   entry, a timer of a rule expires, the period of a `silence` trigger ends (7.11), a rule starts
   (7.5), or a parameter is changed from the UI or API.
2. The engine puts it into an empty FIFO *event queue* and fixes the *cascade time*: the monotonic
   time and the wall time at which the cascade starts. Every expression in the cascade sees this
   time (`now()`, `sinceChange`, timer deadlines).
3. It takes events from the front of the queue and processes them (7.2) until the queue is empty.
   Datapoint updates caused by `write` actions and parameter changes caused by `setParameter` are
   *derived events*; they are appended to the queue in the order they happen.

A cascade completes before the next external event is taken; nothing runs recursively. Before an
external event with time `t` is processed, the engine processes the expiries of its timers and
silence periods with deadlines up to `t` (each a cascade, in the order of 7.8).

### 7.2 Processing an event

For one event:

1. **Match memories.** Every `match` trigger (of every rule, enabled or not, in rule order) whose
   condition references the updated datapoint or the changed parameter is evaluated against the
   current model; it *matches this event* when it is `true` now and was not at its previous
   evaluation. The result becomes its new memory.
2. **Rules.** For each rule in rule order that is enabled: the rule's *active reactions* are those
   with a trigger matching the event (2.3) whose `from` contains the current state (or have no
   `from`). If there is none, the rule does not fire. Otherwise the rule *fires*:
   - If the event is derived, the cascade's derived-firing count is incremented first; a firing
     that would make it exceed 32 is refused instead: the engine logs `loopGuard` naming this rule
     and the culprit (9.2), empties the queue and ends the cascade (the model keeps the values
     already written).
   - The diagnostics record the trigger (9.1).
   - The reactions of the rule are considered in their order. A reaction is executed when one of
     its triggers matches the event and its `from` contains the current state *at this moment*:
     its condition is evaluated (absent: `true`); `true` runs the actions — for a transition,
     exactly in the order exit actions of the current state, the reaction's actions, change of
     state, entry actions of `to` — and `false` runs the `elseActions`; no value runs nothing and
     logs `conditionNoValue` (once, 9.2). After a transition has run, no further reaction of this
     rule is considered for this event.

Every expression reads the model as it is when it is evaluated: an event identifies *what* happened,
not a snapshot of the values. A condition evaluated for an earlier event of the queue may therefore
see values written later in the same cascade.

A datapoint trigger with `on: change` uses the event's own old and new value and quality (the update
changed something), not the current value.

### 7.3 The write action

1. Evaluate `value`. No value: log `valueNoValue` (once) and skip. Evaluation error: `evalError`
   (once) and skip.
2. Convert to the datapoint's type (3.2). Not exact: log `writeRefused` and skip.
3. With `onlyOnChange`: if the datapoint's quality is good and its value equals the converted value,
   skip (no log).
4. `DatapointModel::command_by_rule()`: the model's `command()`, except that a value of the
   application (a datapoint of the image declared `virtual` or without a technology member,
   `spec/runtime-image.md` 3.3, schema 1.6) that no technology owns takes the value also when it is
   not `writable`. `writable` is the user's permission (the UI, the API, MQTT): a rule's own output,
   which the user must not change, is a value of the application that is not writable
   (haumacher/ehome #138). A refusal (the technology refused it) logs `writeRefused`. On
   success the model stores the value and tells its listeners; the update is a derived event of
   the cascade, even when the value did not change (`on: update` triggers see it, `on: change`
   triggers do not).
5. While the datapoint's technology is not connected (the KNX tunnel is down, the Zigbee network
   is not up) the write is not refused (homecontrol #72): the technology checks the value as if
   it sent it (a value it cannot encode, out of the device's range, or for a device that has not
   joined is refused as in 4) and holds it, one value per datapoint (a later write replaces it,
   there is no queue); the model stores it as in 4, so rules and blocks see the commanded value,
   and the datapoint shows `pending` (spec/api.md "Datapoints"). When the technology connects it
   sends every held value once, in datapoint order, through its normal paced send path, before
   the reads on start; then `pending` ends. A value the bus reports before it went out (another
   device wrote the address, a status object) replaces it and the held one is dropped: the bus is
   newer. A read on start of a datapoint with a held value goes after the write, and its answer is
   an ordinary update: if the bus says something else, the bus's value wins. An image swap keeps
   the held value of a datapoint it keeps by name (with its value, 7.6) and sends it on the next
   connect; a datapoint the new image no longer has, or no longer sends, loses it. A write the
   technology took while connected and lost when the connection dropped is not repeated.

### 7.4 Match triggers

A `match` trigger's memory is set without firing whenever the rules start (boot and image swap): its
condition is evaluated then and the result stored. After that, every event that updates one of the
datapoints or parameters it references re-evaluates it (7.2 step 1); it matches when the result is
`true` and the memory was not `true`. Consequences: a condition already true at a swap does not
fire; at boot every datapoint is unknown, so the first report that makes the condition true fires
it.

### 7.5 Starting rules

A rule *starts* at boot, at every image swap (every rule of the new image) and when it is enabled.
For each starting rule, in rule order, one cascade with an external *start event* for this rule
alone:

1. A rule with states and no preserved state (2.6) takes `initialState`; if the rule is enabled,
   the entry actions of that state run. A rule with a preserved state stays in it; no entry
   actions run.
2. If the rule is enabled, the start event is processed like any event (7.2): it matches the
   rule's `start` triggers whose `reasons` contain the start reason (`boot`, `image` or `enable`).

A disabled rule with states takes its initial state without running entry actions. Enabling a rule
does not re-run the entry actions of its current state.

### 7.6 Image swap

When a new image is loaded without a reboot:

1. The new rules are validated completely (section 10); a refused image changes nothing.
2. Rules whose id is not in the new image: their timers are cancelled, their persisted parameter
   values deleted, their state forgotten.
3. Rules whose id is in both images keep:
   - parameter values set by the inhabitant or a rule, per section 8.4 (otherwise `parameterReset`);
   - running timers whose id is still declared, with their deadlines; other timers are cancelled;
   - the current state when the new rule has a state with that id; otherwise the state is lost,
     `stateReset` is logged and the rule enters its initial state when it starts; the persisted
     state follows (8.5);
   - the diagnostics.
4. Rules with a new id start fresh.
5. The memories of all `match` triggers are re-baselined (7.4); change tracking (7.7) continues
   for datapoints that still exist.
6. Every rule of the new image starts with the reason `image` (7.5).

Datapoint values (decision 2): a datapoint whose name and value type are unchanged keeps its value,
quality and update time across a swap, also when its position in the image changed; a new
datapoint, or one whose value type changed, starts unknown. After the swap the technologies read
the datapoints declared read-on-start again, the new ones among them (KNX: a GroupValueRead each,
spread out as at boot), so new inputs get a value without waiting for the next report.

Values the image itself sets (issue #67): the datapoints whose values follow from the image and the
device's own persisted state, not from a technology's report, take their values at a load (a swap
or the boot) as no event for the rules: the rule parameters' datapoints (8.6), the schedules' time
datapoints and the schedules' output datapoints, whose value then says whether the entry fired
today (`spec/schedules.md` 2.2, 5). No trigger matches on them, and rules see the values when they
start with the reason `image` or `boot` (7.5). An output's value is taken at the first evaluation
with a synchronized wall clock, so after a boot without one, at the synchronization, quietly as
well. Every later change of these datapoints is an event: a parameter change, a changed time, a
firing and the reset of the output at midnight.

### 7.7 Change tracking and time

For every datapoint referenced by `sinceChange` or `changedWithin`, the engine records the
monotonic time of the last update that changed value or quality. When the rules start at boot the
record is the datapoint's last update time if its quality is not unknown, and none otherwise; it
then follows every changing update. It survives image swaps, not reboots. `sinceUpdate` uses the
datapoint model's own update time.

Durations are measured on the monotonic clock. The wall clock (local time, time zone from the
image) serves `now()`, `today()`, `time()`, `weekday()` and the sun functions; while the time
service reports the wall clock as not synchronized they have no value. `dateTime` arithmetic and
comparisons work on the instant through the time zone (4.5, 4.6; decision 7).

### 7.8 Timers

Starting a timer evaluates the action's `duration`, or the timer's declared `duration` when the
action has none, and sets the deadline to cascade time + duration. Expiries are processed in the
order of their deadlines; equal deadlines in the order the timers were (re)started. The expiry
stops the timer before the timer event is processed, so `running(t)` is `false` and `remaining(t)`
has no value in the reactions that handle it. The ends of silence periods (7.11) take part in the
same order. The event loop needs only one platform timer for all rule timers and silence periods:
the engine keeps its own deadline list.

### 7.9 Enabling and disabling

The enable switch is the parameter `enabled` (8.3). When it changes to `false`, the rule's running
timers are cancelled, its silence triggers disarmed, and it stops firing; its state and parameters
stay. When it changes to `true`, the rule's silence triggers are armed anew (7.11) and the rule
starts with the reason `enable` (7.5), in the cascade of the parameter change.

### 7.10 Reboot

A reboot loses timers, match memories, change tracking and the diagnostics. Persisted parameter
overrides and rule states (8.5) survive (decision 3). After the image is loaded at boot:

1. Each rule with states whose persisted state the image still declares takes that state, without
   running its entry actions.
2. A persisted state that the image does not declare, or a persisted state of a rule that now has
   no states, logs `stateReset`; a rule with states then takes `initialState` with its entry
   actions (7.5).
3. Every rule starts with the reason `boot` (7.5); `start` triggers fire as usual.

Timers do not resume. A rule that needs its timer after a reboot restarts it from a reaction to
the `start` trigger with the reason `boot` and `from` the state (corpus case 37).

### 7.11 Silence

A `silence` trigger watches one datapoint for signs of life: it matches once when the datapoint has
had no update for the duration `after`. Its state is a *period* with a *base* time:

1. **Signs of life.** Every update of the datapoint that is not a change to stale (a report, also of
   the same value, a command, a publish) sets the base to the time of the update and arms the
   period anew. A change to stale is no sign of life: a technology reports it when it lost the
   device, which is the silence the trigger is for.
2. **Arming.** Arming evaluates `after` and sets the deadline to base + `after`. A duration that
   has no value or lies outside 100 ms .. 30 d (the limits of a timer) arms nothing and logs
   `timerRefused` (once, until it arms again). A change of a parameter of the rule re-arms the
   periods of its silence triggers that have not ended from their base, with the new duration.
3. **The end.** When the deadline passes, the period ends: the trigger's event is processed as an
   external event (its own cascade, ordered with the timer expiries by deadline, 7.8). The trigger
   then waits for the next sign of life; a datapoint that stays silent fires it once.
4. **Starting.** When the rules start (boot and image swap) every silence trigger is armed with the
   base the later of the datapoint's last update and the boot: after an image swap the period
   counts from the datapoint's last update (a period that ended before the swap ends at once, after
   the start cascades); after a reboot, which loses the update times, it counts from the boot, also
   for a datapoint that never reported.
5. **Enabling.** A disabled rule's silence triggers are not armed; updates meanwhile only move the
   base. Enabling arms them with the base the later of the last update and the enabling.

The event identifies the trigger, not the datapoint: a `datapoint` trigger does not match it. The
diagnostics record it as `silence <datapoint>` (9.1).

### 7.12 Read

The `read` action asks the technology that owns the datapoint for its current value
(`DatapointModel::read()`): KNX sends a GroupValueRead to the datapoint's sending address (else its
first listening address) through the same queue and spacing as the reads on start; Zigbee reads the
attribute behind the datapoint from the device; an external source (`spec/external-values.md`) is
fetched again at its next opportunity (at once, or when one minute after its last attempt has
passed; reads meanwhile join that fetch). The answer arrives later as an ordinary update of the
datapoint (origin `report`), processed as an external event; the action itself changes no value
and is not a derived event. Repeated reads of a datapoint whose read is still waiting are one
read.

A technology that cannot read now refuses the read: KNX while the tunnel is not up, Zigbee a device
that has not joined or has left, a button's or a multistate input's datapoint (no attribute value),
an external source that cannot be fetched (no network, no synchronized clock, no location). A
refused read logs `readRefused` (once per action, until a read of that action is accepted) and the
following actions still run. A read of a datapoint no technology can read at all is refused at load
(`notReadable`, 10.1).

## 8. Adjustable parameters

### 8.1 Declaration

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | parameter id (2.1) | yes | Unique in the rule; not `enabled`. |
| `label` | label (2.2) | yes | Shown in the device UI. |
| `type` | `boolean`, `integer`, `real`, `string`, `duration`, `timeOfDay`, `enumeration`, `color` | yes | |
| `default` | JSON value of the type (3.1) | yes | Must itself be valid. |
| `min`, `max` | JSON value of the type | no | For `integer`, `real`, `duration`, `timeOfDay`; inclusive. |
| `step` | number or duration (ms) | no | A hint for the UI's control; not enforced. |
| `unit` | string, at most 15 bytes | no | Display unit: `°C`, `%`, `lx`; for a duration the unit the UI edits in (`s`, `min`, `h`). |
| `choices` | array of `{"value": v, "label": label}`, at most 16 | no | For `integer`, `string`, `enumeration`; the value must be one of them. |
| `adjustable` | boolean | no, `true` | Whether the UI and API may change it. A parameter that is not adjustable is a named constant E-Home can change with the next image. |

A value is *valid* for a declaration when it has the type (after the conversions of 3.2), lies
within `min` and `max` and is one of the `choices` when they are given.

### 8.2 Changing a value

From the UI or API (`setParameter(rule, parameter, value)`): refused with `parameterRefused` and a
reason returned to the caller when the rule or parameter does not exist, the parameter is not
adjustable or the value is not valid. Otherwise: a value equal to the declared default removes the
override, so the parameter follows the default again, also the default of later images; any other
value becomes the parameter's *override*, which is persisted (decision 15). If the value differs
from the current value, a parameter-changed event starts a cascade. Resetting a parameter removes
the override; the value returns to the default (an event if that changes the value).

From a `setParameter` action: the same, except that `adjustable` is not required; a value that is
not valid (or has no value) logs `parameterRefused`; the change is a derived event of the cascade.

### 8.3 The enable switch

Every rule has the boolean parameter `enabled`, default the rule's `enabled` member, adjustable
unless the rule says `enabledAdjustable: false` (then it is fixed by the image). It is persisted
like any parameter. Expressions, triggers and actions cannot reference it (refused at load); its
effects are those of 7.9. Other rules see it as the datapoint `rule/<ruleId>/enabled` (8.6).

### 8.4 Persistence and image swaps

An override is stored under the logical key `<ruleId>/<parameterId>` with its type. The device's
storage is the platform's choice (NVS keys are limited to 15 characters, so the ESP platform maps
the logical key, for example to a LittleFS file or a hashed NVS key holding the full key; issue
#12). Writes may be delayed by up to 10 s and coalesced to protect the flash; a power loss within
that window loses the latest change. E-Home should warn about a rule that sets a parameter more
often than once a minute.

When a new image is loaded (swap or boot), each override is checked against the new declaration:

| New image | Override |
|---|---|
| declares the parameter with the same type, and the value is valid | kept |
| declares it with another type, or the value is no longer valid (range, choices) | deleted, the value becomes the new default, `parameterReset` logged |
| does not declare the parameter, or the rule is gone | deleted silently (E-Home removed it) |

A parameter that never had an override follows the default of each new image. Only values that
differ from the default are stored (8.2). An override that a new image's default happens to equal
is kept by the table above; it is removed when the parameter is next set to the default or reset.

### 8.5 Persisted rule states

The current state of every rule with states is persisted (decision 3) in the same store as the
overrides, under the logical key `<ruleId>/.state` (no parameter id can be `.state`) with the state
id as the value. Like overrides, its writes may be delayed by up to 10 s and coalesced: a state
that changes and changes back within the window is not written, a power loss within the window
loses the latest change, an orderly shutdown writes it. Timers are not persisted.

At boot the persisted state is restored as section 7.10 describes. When a new image is loaded
(swap or boot), a persisted state is kept when the rule still exists, has states and declares that
state; otherwise it is deleted (with the `stateReset` of 7.6 or 7.10 when the rule still exists).
After a swap that reset the state, the rule's new state is persisted like any change.

### 8.6 Parameters as datapoints

Every parameter of every rule, the enable switch included, is also a datapoint of the device: its
name is `rule/<ruleId>/<parameterId>` (`rule/hall-presence-light/timeout`,
`rule/hall-presence-light/enabled`, `rule/hall-presence-light/max-helligkeit` for the parameter
`$"max-helligkeit"`). The device derives these datapoints from the `rules` member
when it loads the image, after the image's own datapoints and the time master's, in rule order and
within a rule the enable switch first, then the parameters in their order; the image declares none
(the prefix `rule/` is reserved, 2.1). Their value types:

| Parameter type | Datapoint type |
|---|---|
| `boolean` | `boolean` |
| `integer` | `signed` |
| `real` | `real` |
| `string` | `string` |
| `duration` | `unsigned` (milliseconds) |
| `timeOfDay` | `timeOfDay` |
| `enumeration` | `enumeration` |
| `color` | `color` |

The rule engine owns them (technology `rules`):

1. **Value.** The datapoint always carries the parameter's current value, quality good: from the
   start of the rules (the default or the persisted override), and after every change (UI or API,
   a `setParameter` action, a reset, a new image). A change updates the datapoint as an event of
   its own after the parameter-changed event (origin `publish`); taking the values when the rules
   start is no event.
2. **Commands.** A parameter datapoint is writable exactly when the parameter is adjustable. A
   command to it (the UI, the API, MQTT, another rule's `write`) is a parameter change from the UI or
   API (8.2): an invalid value is refused (`parameterRefused`) and the datapoint keeps its value; a
   valid one becomes the parameter's value (an override, or none when it equals the default), and
   the model stores it as the commanded value. From a rule's `write` the parameter change is a
   derived event of the cascade, followed by the datapoint's update.
3. **Other rules** read a parameter datapoint, trigger on it and write it like any datapoint (in the
   text form `@"rule/heat/setpoint"`, 4.3). A rule cannot reference its own parameter datapoints
   (refused at load, `forbidden`): it has the parameters themselves. A `read` of a parameter
   datapoint is refused at load (`notReadable`): there is nothing to fetch.
4. **Persistence.** The parameter store (8.4) keeps the overrides; the datapoints are not persisted
   by the datapoint model and have no history.

A value the datapoint type cannot hold (an integer beyond the signed 32-bit range, a negative
duration or one beyond 2^32-1 ms) is not mirrored: the datapoint keeps its previous value and the
device logs it. E-Home's simulation shows the same datapoints.

The adjustable time of a schedule entry is a datapoint of the same kind, `schedule/<id>/time`
(`spec/schedules.md` 2.2, owned by the scheduler): rules read, trigger on and write it
(`@"schedule/workdayMorning/time"`); its value at the load is no event, a command to it is a change
of the entry within its bounds.

## 9. Diagnostics

### 9.1 Per-rule state

Every interpreter maintains and exposes for each rule (device: `/api/rules` and the UI, issues #12
and #14; E-Home: the simulation view):

| Member | Meaning |
|---|---|
| `id`, `name`, `enabled` | |
| `firings` | Number of firings since the rules started at boot. |
| `lastTriggerAt`, `lastTrigger` | Cascade time of the last firing (wall time when synchronized, uptime always) and what matched: `datapoint hall.presence`, `match`, `schedule evening`, `timer off`, `start boot`, `parameter timeout`, `silence hall.temp`. |
| `lastActionAt`, `lastAction` | The last action that took effect: `write hall.light = true`, `restartTimer off 3min`, `setParameter comfort = 21.5`, `read hall.temp`, `log`. |
| `errors`, `lastErrorAt`, `lastError` | Number of logged errors and warnings of the rule, the last one's time, code (9.2) and text. |
| `state` | The current state id (rules with states). |
| `timers` | For each timer: `id`, `running`, `remaining`. |
| `parameters` | For each parameter: `id`, value, whether it has an override. |

### 9.2 Log entries

The engine logs with the tag `hc.rule`; every entry names the rule id. The codes are part of the
contract (the corpus compares them); the text after the code is free.

| Code | Level | When | Repetition |
|---|---|---|---|
| `log` | the action's | a `log` action | every time |
| `conditionNoValue` | warning | a reaction condition has no value (not by an evaluation error) | once per condition, again only after it had a value |
| `valueNoValue` | warning | a `write` value has no value (not by an evaluation error) | once per action, again only after it had a value |
| `evalError` | error | an evaluation error in any expression (the text names it) | once per expression, again only after it evaluated without error |
| `writeRefused` | error | conversion not exact, or the model refused the command | every time |
| `timerRefused` | error | a timer duration or the period of a `silence` trigger has no value or lies outside 100 ms .. 30 d | every time for a timer; once per silence trigger, again only after it armed (7.11) |
| `readRefused` | warning | the technology refused a `read` (7.12) | once per action, again only after a read of it was accepted |
| `parameterRefused` | warning | a parameter value was refused (UI, API or action) | every time |
| `parameterReset` | warning | an override was deleted by a new image because it no longer fits (8.4) | every time |
| `stateReset` | warning | a new image does not have the rule's current state (7.6); at boot, the persisted state is not declared or the rule has no states (7.10) | every time |
| `loopGuard` | error | a cascade exceeded 32 derived firings (7.2); the entry names the culprit: the rule with the most derived firings in the cascade (the first in rule order on a tie) and their number; with blocks (`spec/blocks.md` 5.1) their calls for derived events count too, and a block that called more than every rule is the culprit | once per cascade |
| `queueOverflow` | error | the event queue was full; the event was dropped | once per cascade |

"Once" keeps a flag per expression site: a site whose problem persists logs it on the first
occurrence only; the diagnostics' `errors` counter counts logged entries only.

## 10. Loading: checks and limits

### 10.1 Checks

The loader (device) and E-Home's export check every rule before the image is accepted; the first
failure refuses the whole image with the rule id, the JSON path and one of these reasons:

| Reason | Examples |
|---|---|
| `malformed` | wrong JSON type, missing or unknown member, unknown `kind`, `function` or `op`; an id outside its form (2.1): `parameter id 'max/helligkeit' is not 1..24 characters of A-Z a-z 0-9 _ -`, `parameter id 'not' is a keyword`, `timer id 'x-y' is not an identifier` |
| `duplicateId` | two rules, or two parameters/timers/states of one rule, with the same id |
| `unknownReference` | a datapoint, schedule, parameter, timer, state or rule (in `state("id")`) that does not exist; a `state("id")` literal whose value is not a JSON string or is longer than 32 bytes (it names no rule) |
| `typeError` | operand or argument types (4.4–4.7), a condition that is not boolean, a write value that does not convert to the datapoint type (3.2), a timer duration that is not a duration |
| `invalidLiteral` | a literal value out of its range (`25:00`, a color level of 256, a string longer than 28 bytes, an integer beyond 2^53-1, a date or date-time outside the years 1900..2155) |
| `invalidParameter` | a default that is not valid, `min > max`, `choices` on a type without them |
| `notWritable` | a `write` to a datapoint without the writable flag that is no value of the application (a status a technology reports, a KNX group address on which no object takes values; also the datapoint of a parameter that is not adjustable, 8.6); a `write` to the output datapoint of a schedule entry (the scheduler owns it). A value of the application (`spec/runtime-image.md` 3.3) is the rules' to write also without the writable flag (7.3, schema 1.6) |
| `notReadable` | a `read` of a datapoint no technology can be asked for: one without a KNX, Zigbee or external-values binding (a virtual datapoint), the output of a schedule entry, a datapoint of the time master, a rule parameter's datapoint (8.6) |
| `forbidden` | a time, history, timer or state function in a `match` condition; a datapoint, or a time, history, timer or state function, in the `after` of a `silence` trigger; a reference to `enabled`; a reference to the rule's own parameter datapoints (8.6); `from` or `to` in a rule without states; `elseActions` together with `to`; a `timer` node outside `running`/`remaining`; `state()` arguments other than one string literal |
| `limitExceeded` | a limit of 10.2; two persistence keys of the image (8.4, 8.5) that collide in the device's storage (the device's mapping, `doc/rules.md`; practically never, but checked) |

Every check belongs to loading the rule set itself, so every loader (the image service, a checker
in E-Home, a test) refuses the same images.

### 10.2 Limits

Chosen for a controller with 512 KB of RAM of which the rule engine may use about 64 KB; the
interpreters refuse beyond them (load refusal `limitExceeded`, or the run-time log code given):

| What | Limit |
|---|---|
| Rules per image | 128 |
| Reactions per rule | 16 |
| Triggers per reaction | 8 |
| Actions per action list (actions, elseActions, entry, exit) | 16 |
| Parameters per rule (with `enabled`) | 8 |
| Timers per rule | 8 |
| States per rule | 16 |
| Nodes per expression | 64 |
| Expression depth (a leaf has depth 1) | 12 |
| Arguments per call | 8 |
| Parts of a log message | 8 |
| Bytes of a string part of a log message | 200 |
| Choices of a parameter | 16 |
| Languages of a label | 16 |
| Nodes in all expressions of an image | 4096 |
| Derived firings per cascade | 32 (`loopGuard`) |
| Events waiting in the queue | 128 (`queueOverflow`) |
| Timer duration, period of a `silence` trigger | 100 ms .. 30 d (`timerRefused`) |
| String value | 28 bytes |
| Formatted log message | 200 bytes (cut, 4.9) |

Budget: a compact node takes 16 bytes on the ESP32 (kind, operator or function, a type, two child
indices or a 64-bit literal; longer literals in a side table), so 4096 nodes take 64 KB at most;
a typical 50-rule image uses under 1500 nodes (about 24 KB) plus about 100 bytes per rule, 24 per
timer and 48 per parameter. The event queue holds 128 entries of 8 bytes. Evaluation uses a stack
of at most 12 frames of values (12 × 32 bytes).

## 11. Worked examples

Each example is shown as an outline (a notation of this document only: E-Home edits rules in forms
and only expressions as text) and as the JSON of the image. Each is also a corpus case that runs it
against a timeline. The outline writes a `silence` trigger as `when silence hall.temp after 30min`
and a `read` action as `read hall.temp`; a datapoint name that is not an identifier with
dot-separated parts is quoted as in the text syntax (`write @"rule/heat/setpoint" := 20.0`).

### 11.1 Presence light with a switch-off delay

Two reactions share the timer `off`: one restarts it when presence ends, the other handles its
expiry. `onlyOnChange` avoids a telegram when the light is already on; the light switches on only
when it is dark, but presence keeps a light that is already on. Corpus:
`spec/fixtures/rules/05-timer-presence-light.json`.

```
rule hall-presence-light "Hall light by presence"
  parameter timeout: duration = 3min, 30s..1h, unit "min", label "Switch-off delay"
  parameter darkBelow: real = 80.0, 0.0..2000.0, unit "lx", label "Dark below"
  timer off = $timeout
  when datapoint hall.presence
    if @hall.presence and (@hall.light or @hall.lux < $darkBelow)
    then cancelTimer off; write hall.light := true (onlyOnChange)
  when datapoint hall.presence
    if not @hall.presence
    then restartTimer off
  when timer off
    then write hall.light := false
```

```json
{
	"id": "hall-presence-light",
	"name": "Hall light by presence",
	"description": "Switch on when someone is present and it is dark (or the light is on already); switch off $timeout after the last presence.",
	"parameters": [
		{
			"id": "timeout",
			"label": "Switch-off delay",
			"type": "duration",
			"default": 180000,
			"min": 30000,
			"max": 3600000,
			"step": 30000,
			"unit": "min"
		},
		{
			"id": "darkBelow",
			"label": "Dark below",
			"type": "real",
			"default": 80.0,
			"min": 0.0,
			"max": 2000.0,
			"step": 10,
			"unit": "lx"
		}
	],
	"timers": [{"id": "off", "duration": {"kind": "parameter", "id": "timeout"}}],
	"reactions": [
		{
			"triggers": [{"kind": "datapoint", "datapoint": "hall.presence"}],
			"condition": {"kind": "binary", "op": "and", "left": {"kind": "datapoint", "name": "hall.presence"}, "right": {"kind": "binary", "op": "or", "left": {"kind": "datapoint", "name": "hall.light"}, "right": {"kind": "binary", "op": "<", "left": {"kind": "datapoint", "name": "hall.lux"}, "right": {"kind": "parameter", "id": "darkBelow"}}}},
			"actions": [
				{"kind": "cancelTimer", "timer": "off"},
				{
					"kind": "write",
					"datapoint": "hall.light",
					"value": {"kind": "literal", "type": "boolean", "value": true},
					"onlyOnChange": true
				}
			]
		},
		{
			"triggers": [{"kind": "datapoint", "datapoint": "hall.presence"}],
			"condition": {"kind": "unary", "op": "not", "operand": {"kind": "datapoint", "name": "hall.presence"}},
			"actions": [{"kind": "restartTimer", "timer": "off"}]
		},
		{
			"triggers": [{"kind": "timer", "timer": "off"}],
			"actions": [
				{
					"kind": "write",
					"datapoint": "hall.light",
					"value": {"kind": "literal", "type": "boolean", "value": false}
				}
			]
		}
	]
}
```

### 11.2 Blind by sun and wind

Strong wind opens the blind and (re)starts a hold timer; while it runs the sun reaction stays quiet;
its expiry re-evaluates the sun. `hysteresis` takes the blind's own position as the previous state,
so no hidden memory is needed: shade at `$sunLux`, open again below half of it, and only between
sunrise and sunset. Corpus: `spec/fixtures/rules/35-example-blind-sun-wind.json`.

```
rule living-blind "Living room blind: sun and wind"
  parameter sunLux: real = 40000.0, 10000.0..100000.0, unit "lx", label "Shade above"
  parameter windLimit: real = 10.0, 5.0..20.0, unit "m/s", label "Wind limit"
  parameter shade: real = 75.0, 0.0..100.0, unit "%", label "Shade position"
  timer windHold = 15min
  when datapoint weather.wind (update)
    if @weather.wind >= $windLimit
    then write living.blind := 0.0 (onlyOnChange); restartTimer windHold
  when datapoint weather.lux | timer windHold
    if not running(windHold) and @weather.wind < $windLimit
    then write living.blind := if(inWindow(time(), sunrise(), sunset()) and hysteresis(@weather.lux, $sunLux / 2, $sunLux, coalesce(@living.blind > 0.0, false)), $shade, 0.0) (onlyOnChange)
```

```json
{
	"id": "living-blind",
	"name": "Living room blind: sun and wind",
	"description": "Wind above the limit opens the blind (0 %) and holds it open for 15 min after the last strong gust; otherwise the blind shades while the sun is up and bright (on at $sunLux, off below half of it).",
	"parameters": [
		{
			"id": "sunLux",
			"label": "Shade above",
			"type": "real",
			"default": 40000.0,
			"min": 10000.0,
			"max": 100000.0,
			"step": 1000,
			"unit": "lx"
		},
		{
			"id": "windLimit",
			"label": "Wind limit",
			"type": "real",
			"default": 10.0,
			"min": 5.0,
			"max": 20.0,
			"step": 0.5,
			"unit": "m/s"
		},
		{
			"id": "shade",
			"label": "Shade position",
			"type": "real",
			"default": 75.0,
			"min": 0.0,
			"max": 100.0,
			"step": 5,
			"unit": "%"
		}
	],
	"timers": [{"id": "windHold", "duration": {"kind": "literal", "type": "duration", "value": 900000}}],
	"reactions": [
		{
			"triggers": [{"kind": "datapoint", "datapoint": "weather.wind", "on": "update"}],
			"condition": {"kind": "binary", "op": ">=", "left": {"kind": "datapoint", "name": "weather.wind"}, "right": {"kind": "parameter", "id": "windLimit"}},
			"actions": [
				{
					"kind": "write",
					"datapoint": "living.blind",
					"value": {"kind": "literal", "type": "real", "value": 0.0},
					"onlyOnChange": true
				},
				{"kind": "restartTimer", "timer": "windHold"}
			]
		},
		{
			"triggers": [
				{"kind": "datapoint", "datapoint": "weather.lux"},
				{"kind": "timer", "timer": "windHold"}
			],
			"condition": {"kind": "binary", "op": "and", "left": {"kind": "unary", "op": "not", "operand": {"kind": "call", "function": "running", "args": [{"kind": "timer", "id": "windHold"}]}}, "right": {"kind": "binary", "op": "<", "left": {"kind": "datapoint", "name": "weather.wind"}, "right": {"kind": "parameter", "id": "windLimit"}}},
			"actions": [
				{
					"kind": "write",
					"datapoint": "living.blind",
					"value": {"kind": "call", "function": "if", "args": [{"kind": "binary", "op": "and", "left": {"kind": "call", "function": "inWindow", "args": [{"kind": "call", "function": "time", "args": []}, {"kind": "call", "function": "sunrise", "args": []}, {"kind": "call", "function": "sunset", "args": []}]}, "right": {"kind": "call", "function": "hysteresis", "args": [{"kind": "datapoint", "name": "weather.lux"}, {"kind": "binary", "op": "/", "left": {"kind": "parameter", "id": "sunLux"}, "right": {"kind": "literal", "type": "integer", "value": 2}}, {"kind": "parameter", "id": "sunLux"}, {"kind": "call", "function": "coalesce", "args": [{"kind": "binary", "op": ">", "left": {"kind": "datapoint", "name": "living.blind"}, "right": {"kind": "literal", "type": "real", "value": 0.0}}, {"kind": "literal", "type": "boolean", "value": false}]}]}}, {"kind": "parameter", "id": "shade"}, {"kind": "literal", "type": "real", "value": 0.0}]},
					"onlyOnChange": true
				}
			]
		}
	]
}
```

### 11.3 Heating setback

The rule computes the setpoint from state, not from moments: the scheduler (issue #10) publishes the
comfort period as a datapoint, so the rule is right after a boot, a swap, a window change or a
parameter change alike. Unknown inputs fall back to comfort and window closed through `coalesce`.
Corpus: `spec/fixtures/rules/34-example-heating-setback.json`.

```
rule living-heating "Living room heating"
  parameter comfort: real = 21.0, 16.0..26.0, unit "°C", label "Comfort temperature"
  parameter setback: real = 17.0, 12.0..22.0, unit "°C", label "Setback temperature"
  parameter frost: real = 7.0, 5.0..12.0, unit "°C", label "Frost protection"
  when start | datapoint schedule.living.comfort | datapoint living.window | parameter comfort | parameter setback | parameter frost
    then write living.setpoint := if(coalesce(@living.window, false), $frost, if(coalesce(@schedule.living.comfort, true), $comfort, $setback)) (onlyOnChange)
```

```json
{
	"id": "living-heating",
	"name": "Living room heating",
	"description": "The setpoint follows the comfort period of the schedule (a datapoint of the scheduler), frost protection while the window is open.",
	"parameters": [
		{
			"id": "comfort",
			"label": "Comfort temperature",
			"type": "real",
			"default": 21.0,
			"min": 16.0,
			"max": 26.0,
			"step": 0.5,
			"unit": "°C"
		},
		{
			"id": "setback",
			"label": "Setback temperature",
			"type": "real",
			"default": 17.0,
			"min": 12.0,
			"max": 22.0,
			"step": 0.5,
			"unit": "°C"
		},
		{
			"id": "frost",
			"label": "Frost protection",
			"type": "real",
			"default": 7.0,
			"min": 5.0,
			"max": 12.0,
			"step": 0.5,
			"unit": "°C"
		}
	],
	"reactions": [
		{
			"triggers": [
				{"kind": "start"},
				{"kind": "datapoint", "datapoint": "schedule.living.comfort"},
				{"kind": "datapoint", "datapoint": "living.window"},
				{"kind": "parameter", "parameter": "comfort"},
				{"kind": "parameter", "parameter": "setback"},
				{"kind": "parameter", "parameter": "frost"}
			],
			"actions": [
				{
					"kind": "write",
					"datapoint": "living.setpoint",
					"value": {"kind": "call", "function": "if", "args": [{"kind": "call", "function": "coalesce", "args": [{"kind": "datapoint", "name": "living.window"}, {"kind": "literal", "type": "boolean", "value": false}]}, {"kind": "parameter", "id": "frost"}, {"kind": "call", "function": "if", "args": [{"kind": "call", "function": "coalesce", "args": [{"kind": "datapoint", "name": "schedule.living.comfort"}, {"kind": "literal", "type": "boolean", "value": true}]}, {"kind": "parameter", "id": "comfort"}, {"kind": "parameter", "id": "setback"}]}]},
					"onlyOnChange": true
				}
			]
		}
	]
}
```

### 11.4 Staircase light

A state machine: a press in any state is a transition to `on` (from `on` itself a self-transition,
which re-runs the entry actions and so restarts the timer); the timer leads from `on` to `warning`
and from `warning` to `off`. Corpus: `spec/fixtures/rules/21-state-machine-staircase.json`.

```
rule stairs "Staircase light"
  parameter onTime: duration = 3min, 30s..30min, unit "min", label "On time"
  parameter warnLevel: real = 20.0, 5.0..50.0, unit "%", label "Warning brightness"
  parameter warnTime: duration = 30s, 10s..2min, unit "s", label "Warning time"
  timer t = $onTime
  state off
    entry: write stairs.light := 0.0
  state on
    entry: write stairs.light := 100.0; restartTimer t
  state warning
    entry: write stairs.light := $warnLevel; restartTimer t $warnTime
  initial off
  when datapoint stairs.button (update)
    if @stairs.button
    to on
  from on when timer t
    to warning
  from warning when timer t
    to off
```

```json
{
	"id": "stairs",
	"name": "Staircase light",
	"description": "A press switches on for $onTime (a press while on restarts it); then dims to $warnLevel for $warnTime as a warning, then off.",
	"parameters": [
		{
			"id": "onTime",
			"label": "On time",
			"type": "duration",
			"default": 180000,
			"min": 30000,
			"max": 1800000,
			"step": 30000,
			"unit": "min"
		},
		{
			"id": "warnLevel",
			"label": "Warning brightness",
			"type": "real",
			"default": 20.0,
			"min": 5.0,
			"max": 50.0,
			"step": 5,
			"unit": "%"
		},
		{
			"id": "warnTime",
			"label": "Warning time",
			"type": "duration",
			"default": 30000,
			"min": 10000,
			"max": 120000,
			"step": 10000,
			"unit": "s"
		}
	],
	"timers": [{"id": "t", "duration": {"kind": "parameter", "id": "onTime"}}],
	"states": [
		{
			"id": "off",
			"entry": [
				{
					"kind": "write",
					"datapoint": "stairs.light",
					"value": {"kind": "literal", "type": "real", "value": 0.0}
				}
			]
		},
		{
			"id": "on",
			"entry": [
				{
					"kind": "write",
					"datapoint": "stairs.light",
					"value": {"kind": "literal", "type": "real", "value": 100.0}
				},
				{"kind": "restartTimer", "timer": "t"}
			]
		},
		{
			"id": "warning",
			"entry": [
				{
					"kind": "write",
					"datapoint": "stairs.light",
					"value": {"kind": "parameter", "id": "warnLevel"}
				},
				{
					"kind": "restartTimer",
					"timer": "t",
					"duration": {"kind": "parameter", "id": "warnTime"}
				}
			]
		}
	],
	"initialState": "off",
	"reactions": [
		{
			"triggers": [{"kind": "datapoint", "datapoint": "stairs.button", "on": "update"}],
			"condition": {"kind": "datapoint", "name": "stairs.button"},
			"actions": [],
			"to": "on"
		},
		{
			"triggers": [{"kind": "timer", "timer": "t"}],
			"actions": [],
			"from": ["on"],
			"to": "warning"
		},
		{
			"triggers": [{"kind": "timer", "timer": "t"}],
			"actions": [],
			"from": ["warning"],
			"to": "off"
		}
	]
}
```

## 12. Decisions

The points the first draft left open, decided by the author on 2026-10-18. Each keeps its number;
the sections and corpus cases named state the decision as fact. A reversal changes the spec, the
corpus cases named and both interpreters.

1. **A condition without value runs neither branch.** Confirmed as specified (2026-10-18, #25).
   Treating it as `false` would run the `elseActions`: "if dark then on else off" would switch off
   whenever the light sensor is unknown. Case 13.
2. **Datapoint values across an image swap are kept.** Decided (2026-10-18, #26): a datapoint whose
   name and value type are unchanged keeps value, quality and update time; the datapoints declared
   read-on-start are read again after the swap, the new ones among them (7.6). Implemented by the
   image loader (`DatapointModel::load` keeping values, the KNX adapter reading after its
   reconfiguration).
3. **States survive a reboot; timers do not.** Decided (2026-10-18, #26): the current state is
   persisted under `<ruleId>/.state` with delayed, coalesced writes; at boot the rule takes it
   without running entry actions, and the `start` trigger with the reason `boot` fires as usual; a
   persisted state the image no longer declares, or a rule now without states, logs `stateReset`
   and the rule enters `initialState` (2.6, 7.10, 8.5, 9.2). Case 37.
4. **Disabling a rule cancels its timers.** Confirmed as specified (2026-10-18, #26). A light
   switched on by the rule stays on. Case 28.
5. **The `start` trigger fires for every rule at every image swap**, also for unchanged rules.
   Confirmed as specified (2026-10-18, #26). Case 6.
6. **No enumeration literals in the text; the round trip holds on normalized trees.** Decided
   (2026-10-18, #27; #34): the canonical text compares enumerations with numbers
   (`@hvac.mode == 1`). The loader and E-Home's parser normalize the AST (4.8): a minus over a
   number literal becomes a negative literal; an integer literal partnering an enumeration becomes
   an enumeration literal (its text stays the number), an enumeration literal elsewhere an integer
   literal; a controlled literal becomes the call `controlled(b, n)`; a time of day with a day of
   the week prints as `mon 07:30` (4.2, `spec/rules-grammar.ebnf`). Case 41 and the round trip of
   the corpus.
7. **`dateTime` arithmetic works on the instant.** Decided (2026-10-18, #27): `dateTime ± duration`
   is computed on the UTC instant through the device's time zone and converted back; comparisons
   compare instants; the summer-time flag selects the occurrence of an ambiguous local time (3.1,
   4.5, 4.6). Timers stay monotonic. Case 38.
8. **The loop guard counts derived firings per cascade, limit 32.** Confirmed as specified
   (2026-10-18, #25); the `loopGuard` entry names the culprit, the rule with the most derived
   firings in the cascade, and their number (9.2). Case 24.
9. **`between` is inclusive, `inWindow` half-open with `from == until` empty.** Confirmed as
   specified (2026-10-18, #25). Cases 11 and 17.
10. **Rule parameters become datapoints.** Adopted (2026-10-18, #28), implemented: each parameter,
    the enable switch included, is mirrored as a datapoint `rule/<ruleId>/<parameterId>` the device
    derives from the rules, owned by the rule engine (technology `rules`), so the UI, the API and
    MQTT treat it uniformly; a command to it is a parameter change of 8.2; the validation,
    persistence and swap rules of section 8 still apply (2.1, 8.6, 10.1). Cases 50 to 53.
11. **Labels may be language maps.** Decided (2026-10-18, #28): a rule's `name`, a parameter's
    `label` and a choice's `label` are labels (2.2): a string or an object from language tags to
    strings, resolved once at load from the setting `ui.lang` (exact tag, base language, `en`, the
    first entry); the API passes the label through. Case 40.
12. **A `read` action and a `silence` trigger.** Adopted (2026-10-18, #27), implemented: a `read`
    action asks the technology for a datapoint's value (KNX GroupValueRead, a Zigbee attribute
    read, a new fetch of an external source; `readRefused`, load refusal `notReadable`), and a
    `silence` trigger fires once when a datapoint has had no update for a given duration, counted
    from its last update (from the boot after a reboot) and re-armed by the next one (2.3, 2.7,
    7.11, 7.12). Cases 43 to 49.
13. **A `match` condition may not use time.** Confirmed as specified (2026-10-18, #25); schedules
    cover the time-based cases. Case 32.
14. **Timer durations are limited to 100 ms .. 30 d.** Confirmed as specified (2026-10-18, #27).
    Case 20.
15. **Setting a parameter to its default removes the override.** Decided (2026-10-18, #25): a
    value equal to the declared default is not stored and removes a stored override, so the
    parameter follows later default changes in E-Home again; a differing value is stored (8.2,
    8.4). Cases 28 and 36.
16. **Rules write every value of the application; `writable` is the user's.** Decided (2026-11-11,
    haumacher/ehome #138): a rule may write a datapoint of the image that is a value of the
    application (`virtual` or without a technology member, `spec/runtime-image.md` 3.3) also when
    it is not `writable`; `writable` says only whether the user may change it (the UI, the API,
    MQTT). A rule's own output, which the inhabitant must not change, is such a datapoint. A
    datapoint a technology owns that is not writable (a KNX status address) stays refused at load
    (`notWritable`, 10.1): that write is a mistake. Schema 1.6: an older firmware refuses the image
    (7.3). Cases 72 and 73.
