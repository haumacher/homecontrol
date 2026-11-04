# Blocks: the runtime

The contract is `spec/blocks.md` (issue #60); this note is how the core implements it. The
WebAssembly spike that the interface is shaped for is `doc/blocks-wasm.md`.

| Piece | Where |
|---|---|
| The type interface, manifests, the registry | `core/include/hc/blocks/block_type.h`, `core/src/blocks/block_type.cpp` |
| The loader of the image's `blocks`, the implied datapoints | `block_set.h`, `core/src/blocks/block_set.cpp` |
| The runtime: calls, timers, faults, persistence, mirrors | `block_runtime.h`, `core/src/blocks/block_runtime.cpp` |
| The room controller 1.0 | `room_controller.h`, `core/src/blocks/room_controller.cpp` |
| The heating demand 1.0, the heating curve 1.0 (#61, #62) | `heating_demand.h`, `heating_curve.h`, `core/src/blocks/heating_{demand,curve}.cpp` |
| The API | `core/src/web_api_blocks.cpp` |
| The UI's "Blocks" section | `web/ui/dist/diagnostics.js` (System page) |
| The conformance type (tests only) | `core/test/conformance_block.{h,cpp}` |
| Tests | `core/test/room_controller_test.cpp`, `core/test/heating_test.cpp` (both on the rig `core/test/block_rig.h`), the corpus `spec/fixtures/blocks/` in `rules_corpus_test.cpp`, `web_api_test.cpp` ("Image.Blocks", "WebApi.Blocks") |

## The type interface and a later WebAssembly type

`hc::blocks::BlockType` is the contract's type: a `Manifest` (static tables for a built-in type) and
the five calls `start(cx, reason, restored)`, `input(cx, port)`, `timer(cx, timer)`, `period(cx)`,
`parameter(cx, index)`, plus `check(params, blamed)` for cross-parameter checks at load. A type keeps
nothing per instance: everything an instance needs between calls is its *state record*, an array
of doubles the runtime owns, persists by field id and shows in the diagnostics. The calls reach the
runtime through `BlockContext` (`now`, `wall`, `read`, `count`, `read_at`, `param`, `write`,
`set_timer`, `cancel_timer`, `timer_running`, `state`, `log`, `fault`); every value is a double
(spec B6). `count`/`read_at` give the datapoints of an input one by one (spec B20); the compiled
bindings keep their ids in one pool per instance (`BlockSpec::bound`, two bytes per bound
datapoint), so an input's `multiple` of 32 costs only what the image binds.

This is the spike's ABI made complete: a WebAssembly type will be a `BlockType` whose calls enter
the module's exports `hc_start(self, reason, restored)`, `hc_input(self, port)`, `hc_timer(self,
timer)`, `hc_period(self)`, `hc_parameter(self, index)`, whose imports `hc.now`, `hc.read`,
`hc.quality`, `hc.param`, `hc.write`, `hc.set_timer`, `hc.log`, `hc.fault` forward to the
`BlockContext`, and whose `state()` is the module's record of `self` in linear memory (one module
instance per type, the records selected by `self`, as the spike recommends). Its manifest comes from
the module's custom section `hc.manifest`, checked by `check_manifest()` as the tests check the
built-in ones; the registry (`BlockRegistry`) takes it like a built-in type. A trap or an exhausted
budget is a `fault()` of every instance of the type.

## How blocks run inside the rules' cascades

The rule engine owns the cascades (`spec/rules.md` 7). The block runtime is a `rules::BlockHost`
the engine calls (`Engine::set_block_host`):

- `datapoint_updated` treats an update of a datapoint bound to a block input as an event (the
  host's interest bitmap besides the rules');
- `process_event` runs, after the rules, `host.datapoint_event()` for datapoint events and
  `host.block_event()` for the runtime's own events (parameter changes, the enable switch, a
  restart, queued with `Engine::queue_block_event()`: derived inside a cascade, external otherwise);
- `process_due_timers` takes the host's earliest deadline (its timers and periods) into the order
  of the rule timers and runs it as a cascade of its own (`run_due`); `reschedule` includes it in
  the one loop timer;
- the start (`kStartAll`) runs one cascade per block after the rules' start cascades;
- the loop guard: a block call for a derived event asks `Engine::derived_call()`, which counts it
  with the rule firings and names the culprit across rules and blocks.

A call runs inside `process_event`, never in a datapoint listener, so a block may command the model:
its writes enqueue derived events like a rule's. The mirrors' values at a load are published inside
`Engine::quietly()` (spec 5.6).

## The record (frozen format)

One blob per block with persisted state, `b.` + the 8 lower-case hex digits of the 32-bit FNV-1a
hash of the block id (`persist::blob_name`), in the BlobStore beside the value records:

| Bytes | Content |
|---|---|
| 4 | `HCBS` |
| 1 | format, 1 |
| 1 + n | the type id's length and bytes |
| 2 | the type's major version, little-endian |
| 1 + n | the block id's length and bytes |
| 1 | the count of fields that follow |
| per field: 1 + n + 8 | the field id's length and bytes, its IEEE 754 double, little-endian |
| 4 | CRC-32 (IEEE, as `persist::crc32`) of everything before |

Fields are found by id, so a minor version that adds fields reads an older record (the new fields
start at their initial value); a field the manifest no longer has is ignored. A record of another
type or major version, a damaged one or one of another format logs `stateReset` and is deleted. The
loader refuses an image whose block ids' blob names collide (`limitExceeded`). Orphaned records are
deleted at every load (`persist::delete_orphans`).

The parameter overrides are entries of the rules' `ParameterStore` under the logical key
`@<blockId>/<parameterId>` (the enable switch `@<blockId>/enabled`): the rule engine keeps the `@`
entries when it prunes, the block runtime prunes its own. Rule ids cannot start with `@`.

## Persistence pacing

An instance's record is due after a call that changed a persisted field and is written by the
runtime's one loop timer: the first time after a load at once, later at most every
`max(persistEvery, n × 120 s)` (`n` the instances whose type persists): at most 30 records an hour
in all (`BlockLimits::kRecordWritesPerHour`), each an inline LittleFS file (under 100 bytes for the
room controller). `ImageService::flush()` (an orderly reboot) writes every due record. A faulted
instance is not written. The budget is in `doc/persistence.md` "The flash wear budget".

## Memory (ESP32-C6)

Measured on the host (64-bit) by `BlockRuntime: no allocation in steady state; the cost of a room
controller` and `Heating: no allocation in steady state; the cost of an instance is at most a room
controller's`; the C6's 32-bit pointers make the figures slightly smaller.

| What | Size |
|---|---|
| Static: the `BlockRuntime` in the image service, the device registry, the type singletons | about 0.25 KB (144 B + 136 B on the host) |
| Per room controller: the runtime (state record 20 fields, its stored copy, 26 parameters, the port tables) | about 1.2 KB |
| Per room controller: the compiled spec (bindings with the pool of bound ids, the image's parameter values, the label) | about 0.65 KB on the host (649 B) |
| Per room controller: 16 mirror datapoints (enabled, 4 setpoints, 10 outputs, an aggregated input) | about 1.3 KB in the model (64 B + the name each) |
| Per room controller in all | about 3.3 KB on the host (3255 B) |
| Per heating demand (8 valves, 4 demands): 11 fields, 11 parameters, 9 mirrors | about 2.0 KB on the host (2013 B) |
| Per heating curve (2 outside sensors): 9 fields, 29 parameters, 8 mirrors | about 2.2 KB on the host (2170 B) |
| Steady state | no allocation (calls, writes, timers, periods; the record writes allocate nothing in the core) |

Ten rooms cost about 30 KB of heap. On the C6, whose heap minimum at a deploy is about 33 to 37 KB,
that is the binding constraint: E-Home should warn above a handful of instances until the board
measurements of #60 say more.

**At a deploy** (#70, doc/image.md "Memory of a swap"): the old image holds its blocks until the
`block runtime` step releases them, and the new image's blocks are compiled and their datapoints
built beside them. With the C6's sizes (`scripts/c6-model`) the swap's peak above the old image
grows by about 1.5 KB per block (the rules' compilation is the peak) and by about 2 KB per block
from about seven blocks (the `model` step), less than a block's steady cost (4.3 KB for a room
controller with 16 mirrors in the test system, 1.3 to 2.4 KB for a heating demand). A block in both
images thus lowers a deploy's lowest free heap by its steady size plus at most that again, about
6 KB for a room controller: on the C6, which reached 26 KB at revision 13 of the test system (two
blocks), three more room controllers leave under 10 KB. `Swap memory: a block costs at the peak at
most its steady size; the swap stays within its budget` pins it: the peak above the old image is at
most the swap's budget (16 KB for `example.json` without blocks, 64-bit host) plus k times a
block's steady cost, and a block adds at most its steady cost to the peak. The diagnostic outputs (`p`, `i`, `d`, `error`, `filtered`,
`activeSetpoint`, `state`) are 7 of the 16 mirrors.

Flash: the blocks (runtime, loader, room controller, API, UI) took the C6 application from 0x411a0
to 0x33580 bytes free of the 0x2b0000 ota_0 partition (about 55 KB, at the project's `-Og`).
The heating demand and the heating curve (#61, #62) add 4.4 KB and 6.2 KB (`idf.py size-files`:
`heating_demand.cpp.obj` 4380 B, `heating_curve.cpp.obj` 6248 B, about half of each the manifest's
tables); with them 0x301c0 bytes stay free.
