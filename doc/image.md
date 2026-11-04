# The runtime image on the controller

The format is `spec/runtime-image.md`, the endpoints `spec/api.md` "Runtime image". This note
describes how the controller loads, applies and keeps an image (`hc::image`,
`core/include/hc/image/`), and how to deploy one by hand until E-Home does.

## Parts

| Class | Role |
|---|---|
| `image::Loader` | Parses and validates the text into a `Plan` (datapoint specs, KNX mappings, time master, schedules, history specs, the rules' span) and a `Report` of refusals with paths. Builds nothing that runs. |
| `image::ImageService` | Owns the scheduler, the rule engine and its clock, the value store, the history, the KNX time master and the image store; applies a `Plan` (spec section 6), stores the text, loads the stored image at boot, unloads. It is the controller's `LedInput`. |
| `image::ImageStore` | The text in the blob store: 16 KB chunks in two alternating generations and a manifest with a CRC-32 (spec section 7). |
| `image::MemoryBlobStore` | The RAM stand-in of the platforms' blob store (below). |
| `image::DynamicSpecProvider` (`hc/image/dynamic_specs.h`) | A component with datapoints of its own next to the image's (the Zigbee adapter): states them, is told when the model holds them, notifies changes (below, "Dynamic datapoints"). |
| `JsonSpanDocument` (`hc/json_document.h`) | Lets the rule loader read the image's `rules` straight from the text. |
| `WebApi` | `PUT`/`GET`/`DELETE /api/image`, the `image` member of the status; a load or unload resyncs the live feed (every client gets a fresh snapshot, ids changed). |

Everything runs on the event loop. The loader allocates its tables from an arena freed after the
apply; the components allocate their tables at load and free them at the next load or unload. An
arena takes blocks of 1 KB, and a table larger than a quarter of that a block of exactly its size;
the load's summary line gives the bytes used against those allocated ("load tables <used> of <allocated>
bytes used", the same for the UI model). The refusals of a failed load (GET /api/image lists them)
are kept with the first inline and the room for the others (12, about 3 KB) allocated with the
second and freed by the next load; without the heap for it only the first is kept and the others
counted (issue #44).

## Mechanisms

**Body.** `WebApi::body_limit()` gives each route its limit: 1 KB for every API body, 256 KB for
`PUT /api/image`. The ESP server copies small bodies into its static job slot; a larger body and
every body the E-Home connection announces with `length` go to an `image::ImageBody`: for
`PUT /api/image` into the platform's `ImageStaging` (`WebApi::body_staging()`; on the ESP the flash
behind the updater's image, doc/ota.md "The staging area", read back through the flash cache as one
text), otherwise, or when the staging declines (off, too small, in use; the reason is logged), into
a heap buffer of exactly its length (507 `api.no_memory` when the heap has no block that large: the
image is not touched). Before #66 the image went to the heap: E-Home's 32 KB test-system image then
needed one 32 KB block and, held through the whole load, took the heap from 64 KB to 3.9 KB, and on
a fragmented heap (largest block 17.9 KB) the deploy failed outright. If the loop does not take the
job in time, the httpd task waits until the job has run before it releases the body. The POSIX
server lets cpp-httplib collect the body; the POSIX build has no staging area.
`GET /api/image` is streamed: the API answers with `WebResponse::stream` (length and a token of
the stored generation) and the server fetches 8 KB pieces through `WebApi::read_stream()`, one
loop job each, sending them chunked; a deployment in between changes the token and cuts the
response.

**Atomic apply.** The loader checks everything a component could refuse later: unique ids, DPTs
against types, addresses, the time master's addresses against the datapoints', schedule outputs,
history budget, and the rules through a scratch `DatapointModel` and `RuleSet::load` with the
image's schedules and the device's `ui.lang`. The compiled set stays in the plan and the swap hands
it to the engine (`Engine::adopt`): the image's datapoints have the same ids in the scratch and in
the device's model (they come first, the providers' after them), so the rules are compiled once per
load (#66; before, a second time at the swap, about 170 ms on the C6 and a second set beside the
old one). Only a valid plan reaches `apply()`. The datapoint model is the first thing
replaced; if it refuses (memory), the old image is untouched. Later failures cannot be undone in
place, so the service tears the new configuration down and loads the stored image again
(`restore`). The text is stored only after the apply succeeded; a store failure leaves the image
running and is reported (`stored: false`).

**Loading the stored image (#71).** The boot load and the restore read the stored image the way a
received one is read: into an `ImageBody` in the platform's staging, there parsed in place. The
service has a staging of its own for it (`ImageService::set_stored_staging()`), not the one of the
received images: a restore runs while the image that failed is still staged there. `ImageStore::read`
copies the chunks from the blob store through one heap piece of `ImageStore::kReadPiece` (2 KB),
appends them to the body and checks the CRC over the pieces, so no allocation of the load is larger
than the swap's own (before #71 the stored text, 24 KB for the test system, took one heap block, and
the restore failed on a heap whose largest block during a swap was 18–26 KB, leaving no image). On the
ESP the two stagings are the two slots of one area behind the updater (`EspImageStaging::received()`,
`stored()`, doc/ota.md "The staging area"). The stored format is unchanged. Without a stored
staging (the POSIX build), or when it declines (off, in use, no room beside the received image: the
reason is logged), the text goes to a heap buffer of its size as before; when the heap has no such
block either the load is refused as `no_memory` and logged (`stored image not loaded: no memory for
its N bytes`). The tests: `Swap memory: a failed apply on a fragmented heap returns to the stored
image, read in place (#71)` (every allocation above 3.5 KB refused; the old image ends loaded and
running, no allocation of the restore above 3 KB), `… the boot load of the stored image on a
fragmented heap …`, the probe that without a stored staging the same restore ends without an
image, and `… an image whose external member the device refuses returns to the stored image`.

*Provoking a restore on the board.* The loader checks only that `external` is an object; the
external values check its sources in the step `technologies`, after the model, the rules and the
blocks were replaced. The stored image with a source the device refuses added, e.g.
`"external": {"sources": [{"id": "restore-probe", "url": "ftp://192.0.2.1/x", "interval": 600,
"bindings": [{"datapoint": "<a datapoint of the image>", "path": "a"}]}]}`, deployed with
`PUT /api/image` (or the console's `image load <url>`; signed when the device holds the installation
key) fails at `external` and returns to the stored image: the log shows `image from api failed while
it was applied at external: …`, `loading the stored image again` and the stored image `loaded from
restore`, and no `read into the heap`.

**Storing off the loop (#68).** Writing the text to LittleFS was the largest step of a deploy on the
C6 (393 of about 790 ms for the test system's 23 KB image) and needs no parsed state: the verified
bytes stay in the staging area (or the heap buffer) anyway. So `ImageService::load()` takes the
received body over from the receiver (`WebRequest::body_holder`, `ImageBody::take`) and hands the
write to the platform's `Worker` (`hc/platform/worker.h`): on the ESP a task `hc_work` created for
the job (4 KB stack, priority 2, below the loop; it exists only while it stores, so the stack costs
RAM only then, and its unused stack is logged with the job's end), on POSIX a thread, in the tests a
`ManualWorker` the test steps. The image runs from the end of `load()`; the `steps` line says
`store 0 ms (started off the loop)`, and `image '…' revision N stored (B bytes in M ms, off the
loop)` follows when the worker is done. Only then is the body released (the staging area is free
again) and the deploy answered: `PUT /api/image` defers its answer (`WebResponse::deferred` with
`WebApi::kDeferredImageStore` and the store's ticket; the HTTP servers poll it every 100 ms, the
E-Home connection is told at once) and answers as before, `stored: true`, or `stored: false` with
`storeError` when the flash refused (the image runs, the previous stored image stays complete). The
answer carries the metadata of the deployed image as it was loaded; one fetched after a later store
began is `409 image.superseded`, never another deploy's outcome.

While the store runs: a deploy is refused `503 image.busy` with `Retry-After: 1`, before its body is
read (`WebApi::body_refusal()`, asked by the ESP and POSIX servers and the E-Home connection) or, if
it slipped in, by `load()` before anything is looked at (`Report::Kind::busy`); `DELETE /api/image`
is refused the same way; a reload for the providers that fails returns to the image being stored
(its text is at hand, the flash may hold either generation); `GET /api/image` serves the previous
stored image until the manifest switches (its token changes with it). A restart at any moment
keeps a whole image: `ImageStore` writes the new generation's chunks first and switches the
manifest last, each blob an atomic replace (LittleFS: a temporary file renamed over the blob;
littlefs commits a rename atomically), so the flash holds the previous image until the manifest is
written and the new one after (`Image store off the loop: a restart at any moment…`). The worker and
the loop use the BlobStore at the same time, on different blobs (doc/persistence.md "Threads"); a
chunk goes to `BlobStore::write()` in 4 KB pieces, so the loop waits at most for one piece's write
when it needs the file system's lock meanwhile (value and ring saves). The flash's own erase and
program operations still stop every task on the single-core C6 while they run (the cache is off),
but each is short and the loop runs between them.
Without a worker, without a holder (the POSIX `--image` file at start, a small image the E-Home
connection got inline in the request) or when the worker cannot start (no memory for the task,
logged), the store runs within `load()` as before. The console's `image load <url>` hands its
download buffer over too (`ImageBody::adopt`).

**The loop during a load.** The load runs on the loop in one handler; a line after the `loaded`
line gives the steps (`steps: signature … ms, parse …, apply …, MQTT …, UI model …, store …`).
On the C6 the test system's 23 KB image (revision 7) held the loop about 800 ms, and the 20 ms and
50 ms timers missed their periods meanwhile; since #68 the store (about 390 ms of it) runs off the
loop, above. A deploy is a rare operator action, so the load stays
one handler: running it off the loop would need a second, concurrent copy of the parsed state (the
heap #66 had to win back) and a model that the loop's readers do not see half built (#66).

**What survives a swap.** `DatapointModel::load(specs, count, keep_values)` copies value, quality
and update time of datapoints whose name and type are unchanged; `ValueStore::restore` then leaves
those alone. The rule engine gets the old id of each new datapoint (`previous_ids`) so the change
tracking and diagnostics follow datapoints whose position moved; rule timers, states and parameter
overrides follow the rule id (`spec/rules.md` 7.6). The KNX adapter re-reads read-on-start
datapoints when it is configured with the tunnel up.

**Implied datapoints.** The loader adds datapoints the image does not declare: the time master's
(`knx.time`, `knx.date`, `knx.datetime`) and, since the decision of 2026-10-18 (#28), one per rule
parameter, `rule/<ruleId>/<parameterId>` with `enabled` first in each rule (`spec/rules.md` 8.6;
`rules::parameter_datapoints()` is the one derivation). They follow the declared datapoints in the
model, before the providers' datapoints below; `rule/` is a reserved prefix for declared ids. The
rule engine owns the parameter datapoints through its technology `rules`: it publishes a
parameter's value when it changes, and a command on the datapoint is a `setParameter` with the same
validation. They are not persisted by the value store (the parameter store keeps the overrides) and
keep no history.

**Dynamic datapoints.** Some datapoints come and go at run time: a Zigbee device joins or is
removed. A `DynamicSpecProvider` registered with `ImageService::add_provider()` states the datapoints
it needs (`dynamic_specs()`: name, type, flags, unit, the `Technology` that owns them), given the
image's bindings it serves (`ImageBindings`: the `zigbee` members of the image's datapoints). The
service composes the model from the image's datapoints, at the image's positions, followed by every
provider's (the image's datapoints keep their ids whatever the providers do). A name that appears
twice refuses the composition: an image is refused with the path of its datapoint
(`datapoints[8].id: 'hall.light' is also a datapoint of the provider 'zigbee'`), a reload keeps the
old model and is logged (`ImageService::dynamic_refusal()` holds the reason). After the model load
the service binds each provider datapoint to its technology and calls `model_loaded()`; the provider
binds the image datapoints it serves by name.

When a provider's set changes it calls `notify_specs_changed()`; the service reloads the model with
`keep_values` from a timer, never within the call, and at most once per second
(`kReloadSpacingMs`): the first change after a quiet second reloads on the next loop turn, the
changes of the following second (a joining device writes the database several times) are coalesced
into one reload a second after the last. The reload binds every component again as a swap does,
from the applied plan the service keeps while a provider is registered: the value store and the
history load (rings saved and restored by name), the KNX adapter gets its mapping and the time
master its configuration again (the KNX adapter re-reads the read-on-start datapoints when the
tunnel is up), the schedules are loaded again (their state is kept by id; an output is reported
only when it differs), the external values are configured again (their sources fetch again after
the start delay). The rules are neither loaded again nor restarted: the image's datapoints are where
they were, the rules' ids stay valid, their timers and states run on; the engine ignores updates of
datapoints its tables do not cover (the providers' datapoints, which rules cannot name). An image
load composes the providers itself and drops a pending reload.

Without an image the model holds the providers' datapoints alone: at start when nothing is stored,
after `DELETE /api/image` (their values kept), after an apply failure without a stored image. The
first pairing on a fresh device works before E-Home deploys anything. A provider registered while
an image runs whose plan was not kept (no provider when it was applied) joins with the next load.

**LED.** `Controller::led_status()`: pairing, then connecting (network down), then `setup` when no
image is loaded, `degraded` when the image has KNX datapoints (or a time master) and the KNX tunnel
is down, `ready` otherwise (`doc/network.md` for the patterns).

**Settings.** `settings.tz`, `location` and `ntp.server` are written only when they differ from
the stored values, so a re-deployment does not wear the flash; a new NTP server restarts SNTP.

## Storage stand-in

The platforms' blob stores (LittleFS on the ESP, files on POSIX) are being written in parallel.
Until they are merged, both builds use `MemoryBlobStore`: the BlobStore contract (name rule, 20 KB
per blob, atomic replace, `no_space` when full) in heap memory, 96 KB on the ESP and 4 MB on POSIX.
Consequences, until the real store is wired in `main.cpp` (one line each):

- nothing survives a restart: the device boots without an image (LED: setup), the persisted values
  and the history rings are gone;
- on the ESP the stored copy of the image, the persisted values and the saved rings cost RAM (the
  image's size plus up to the history budget); a full store refuses writes (`no_space`, logged)
  without touching what runs.

The image store, the value store and the history are tested against the contract
(`core/test/fake_blob_store.h`); the stand-in has its own contract test.

## Memory

With a provider registered (the ESP32-C6 with Zigbee) the service keeps the applied plan, the
"load tables" the log names for each load ("kept for the providers' reloads"), until the next load
or unload; a reload allocates the composed spec table and the providers' tables for its duration.

On the ESP32-C6 the image service and its components are static (part of the 8.6 KB the `.bss`
grew); the rules engine, scheduler, value store and history are linked for the first time (flash
+171 KB). Per image (host measurements, 64-bit): the example keeps about 9 KB besides its stored
copy, 2.3 KB of it history; the test-system template about 3 KB plus 11.5 KB of history (one
day at one-minute spacing). A load needs the text and its load tables on top (below), and the
event-loop task's stack (12 KB on the ESP; `image status` prints the reserve left).

### Memory of a swap (#70)

A load builds the new image beside the old one, which stays whole until the step that replaces its
part: the old model goes at `model`, the old rules at `engine`, the old blocks at `block runtime`,
the kept plan of the old image at `start`, the new image's plan at `release`. The lowest free heap of
a deploy is therefore the old image's steady state minus the swap's peak above it, minus what other
tasks take meanwhile (below). The phases, as `ImageService` and the `Loader` report them to a
`LoadProbe` (`hc/image/load_probe.h`), each named at its end:

| Phase | What it allocates (+) or frees (-) |
|---|---|
| `signature` | the signature check (Monocypher: the stack only, nothing on the heap) |
| `json` | + the plan (the `Plan` object) |
| `datapoints` | + the datapoint tables, the KNX mapping, the index by name; - their scratch (the KNX, Zigbee and history drafts) |
| `schedules`, `history`, `ui` | + locations, functions, schedules, history specs; the UI is checked, not kept |
| `rules` | + the compiled rule set (kept), - the compiler's work |
| `blocks` | + the compiled `BlockSet` (kept) |
| `compose`, `model` | + the composed spec table (only when a provider adds datapoints), + the new model; - the old model |
| `persistence` | the value store and the history bound and restored (their tables for the new image) |
| `engine` | + the rules' runtime; - the old rules |
| `block runtime` | + the instances' runtime, the mirrors bound; - the old runtime and set (`BlockRuntime::adopt`) |
| `technologies` | KNX mapping, time master, schedules, external values, the providers |
| `start` | the rules and blocks started; - the old image's kept plan (with a provider) |
| `release` | - the new plan (kept instead with a provider) |
| `mqtt`, `view`, `store` | the MQTT member, the UI model, the store (off the loop since #68) |

**What the board showed** (4217c3c, revision 15 of the test system over revision 14, two blocks,
right after the E-Home link had served a GET of the stored image): free 54 896 B at the start; the
deploy failed with 507 in `rules`: "no memory for the rules (7104 bytes)" with 9 780 B free. Two
causes, both fixed generally:

1. *One large block.* The compiled rule set was one allocation of all its tables (7 104 B), and a
   fragmented heap had no such block. Since #70 no table of the swap takes one block of everything:
   the rule set takes one allocation per table, the rules' runtime too and its per-rule and
   per-parameter tables in chunks of 16, the model its datapoints in chunks of 32 (2 KB on the C6),
   the block runtime one allocation per instance, the loader its per-datapoint drafts in chunks of
   16 and the implied datapoints of the rules and blocks in batches from the stack. The largest
   single allocation of the test system's swap went from 7 104 to 2 248 bytes. The tables still
   in one piece grow with the image by a few bytes per entry: the plan's spec table (8 B per
   datapoint), the KNX mapping, the model's names and index, the rule set's nodes (16 B each),
   strings and constants, the history's and the value store's tables.
2. *The swap's own peak.* The rules and the blocks were checked against a scratch `DatapointModel`
   of the plan (8 KB on the board, beside the old image); they now look datapoints up in the plan's
   own tables (`DatapointDirectory`, `PlanDirectory` in the loader; the checks the model would make
   are made without building one). The rule compiler's table of 128 rule ids (4.2 KB) is sized by
   the image's rules. The loader's KNX drafts and rule-parameter list left the plan (they are
   scratch), which also shrinks the plan the service keeps with a provider (2.6 KB less in the
   steady state). The composed spec table is not copied when no provider adds datapoints; the swap
   table of the ids takes its own allocation, not a 1 KB arena block.

Measured with the C6's sizes (`scripts/c6-model`, below) on E-Home's revision 11 of the test system
(23 482 bytes, 58 datapoints, 10 rules, 6 schedules, one history with archives, a provider registered
as the Zigbee adapter is), with k blocks added as revision 13 adds them (a room controller and a
heating demand per pair). Peak of each phase above the start of the load, as the C6's heap charges it
(TLSF: each block at least 12 bytes, rounded up to 4, plus a 4-byte header), and the largest single
allocation of the phase; "k over j" deploys the image with k blocks over the one with j:

| Phase: peak / largest block | 0 over 0 | 1 over 1 | 2 over 1 | 2 over 2 | 4 over 4 |
|---|---|---|---|---|---|
| `json` | 948 / 944 | 948 / 944 | 948 / 944 | 948 / 944 | 948 / 944 |
| `datapoints` | 14 004 / 1 312 | 14 124 / 1 312 | 15 224 / 1 312 | 15 224 / 1 312 | 15 400 / 1 312 |
| `schedules` .. `ui` | 6 516 / 1 040 | 7 204 / 1 040 | 6 692 / 472 | 6 692 / 472 | 8 480 / 1 040 |
| `rules` | 15 256 / 2 061 | 15 944 / 2 061 | 15 432 / 2 061 | 15 432 / 2 061 | 17 220 / 2 061 |
| `blocks` | 13 636 / 0 | 15 056 / 216 | 14 944 / 312 | 14 944 / 312 | 17 856 / 624 |
| **`model`** | **19 888** / 2 048 | **22 460** / 2 048 | **22 920** / 2 048 | **22 920** / 2 048 | **27 572** / 2 248 |
| `persistence` | 13 796 / 776 | 15 028 / 776 | 15 488 / 776 | 14 832 / 776 | 17 476 / 776 |
| `engine` | 16 300 / 1 200 | 17 532 / 1 200 | 17 996 / 1 200 | 17 340 / 1 200 | 19 984 / 1 200 |
| `block runtime` | 6 720 / 40 | 9 076 / 1 112 | 10 264 / 1 112 | 9 604 / 1 112 | 14 096 / 1 112 |
| `technologies` | 7 768 / 864 | 8 488 / 864 | 9 676 / 864 | 7 988 / 864 | 9 820 / 864 |
| the old image held | 33 280 | 37 112 | 37 112 | 38 368 | 45 088 |
| old image + peak | 53 168 | 59 572 | 60 032 | 61 288 | 72 660 |
| before #70 (old + peak) | 62 480 | 69 280 | 69 508 | 70 840 | 80 204 |

Before #70 the peak was in `rules` (30 056 B above the old image for two blocks, a scratch model
and the compiler's 5.6 KB beside it); it is now the `model` step (22 920 B: the plan, the new rule
and block sets and the new model beside the old image), and each block in both images lowers the
deploy's lowest free heap by about its steady size plus 2 KB (doc/blocks.md "Memory").

**Model and board.** The 32-bit model reproduces the board's figures of the core: in the phases
where nothing else happened the board's growth equals the model's within 30 bytes (`schedules`
+1 044 against +1 072, the rules' scratch model +8 068 against +8 056 before #70). What the model
leaves out is the heap of the other tasks during the load: on the board about 20 KB were taken in
the first 170 ms of the load (`signature` 10 KB, `json` 3.9 KB, `datapoints` 6 KB) and still held
at its end, and the heap fell by another 28 KB after it (the lowest since boot 7 304 B). The load
holds the event loop (priority 5) for several hundred milliseconds; the tasks below it (the KNX
socket's reader, mDNS, the E-Home sender and receiver, the console writer, a running HTTP fetch)
cannot take what the network delivers to them meanwhile, and every packet waiting for them holds a
WiFi receive buffer of the heap (up to `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` = 16 of about 1.6 KB).
Since #70 the loop sleeps one tick at the end of each phase (`CONFIG_HC_IMAGE_LOAD_YIELD`, default
on; about 20 ticks of at most 10 ms), so they drain what arrived. Which tasks held the 20 KB is for
the board to show (below, `CONFIG_HC_IMAGE_HEAP_BY_TASK`).

**The budget.** `Swap memory: a block costs at the peak at most its steady size; the swap stays
within its budget and takes no large block` pins the core on the host (64-bit, `example.json` with 0
to 4 blocks, over the same image and over one block fewer): the peak above the old image is at most
**9 KB** (the swap's budget for that image without blocks; measured 6.9 KB on the host, 5.9 KB with
the C6's sizes; 13.2 KB before #70) plus k times a block's steady cost, a block adds to the peak at
most its steady cost, no single allocation of the swap exceeds **3 KB**, a redeploy holds what the
first deploy held, and nothing of the load is held at `release` beyond the new image. `Swap memory:
a large image swaps while no block above 3.5 KB can be had (6 KB on the 64-bit host)` deploys `example.json` with 100 more datapoints, 50 more
rules and 4 blocks (215 datapoints) while every allocation above the limit fails
(`hc_test::BlockLimit`); on the C6's sizes its largest allocation is 3 116 B. The board's allowance
for the other tasks is not part of the host budget; the deploy's margin on the C6 is

    lowest free = free before the deploy - (the swap's peak, C6 model) - (the other tasks' heap during the load)

with the other tasks' heap 20 KB as measured on 4217c3c (without the phase yield), a margin of at
least 8 KB required, and the largest free block during the load at least the swap's largest
allocation (2.3 KB for the test system). The probe's lines give all three terms.

The probe and the whole-heap counter are in the test (`core/test/swap_memory_test.cpp`; the counter's
`hc_test::heap_mark()`, `heap_largest()`, `HeapExclusion` for the fakes of flash, NVS and the log
ring, `BlockLimit`); the table above is its measurement case, run with `HC_SWAP_IMAGE=<image without
blocks>`.

**The 32-bit model** (`scripts/c6-model`): the core and that test compiled by the ESP-IDF's
RISC-V compiler, with the C6's sizes (4-byte pointers; a `Datapoint` is 64 bytes, a block
instance's runtime 312), run under `qemu-riscv32-static` (newlib's `sim` system calls, stubs in
`sim_stubs.c`). Not part of the gate:

```
source ~/tools/esp/esp-idf-v5.5/export.sh
cmake -S scripts/c6-model -B build-c6model -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/scripts/c6-model/toolchain.cmake \
    -DHC_SWAP_IMAGE=<image without blocks>
cmake --build build-c6model && qemu-riscv32-static build-c6model/swap_c6
```

**On the board** (`hc::esp::EspLoadProbe`, component `hc_image`): `log level hc.image debug` on the
console, or `CONFIG_HC_IMAGE_HEAP_PHASES=y` (menu "HomeControl runtime image", default off) for info
level, logs every load's phases, and 3 s and 10 s after the load what the store, the answer and the
tasks the load held back took:

```
swap heap: begin: free <F> B, largest block <B> B, lowest since boot <M> B
swap heap: json          free <f> B, largest <b> B, lowest in the phase <l> B (<F - l> below the start)
...                      (one line per phase of the table above)
swap heap: end (loaded): free <f> B, largest block <b> B, lowest of the load <L> B (<F - L> below the start), lowest since boot <M'> B
swap heap: 3 s after the load: free <f> B, largest block <b> B, lowest since boot <M''> B
swap heap: 10 s after the load: ...
```

With `CONFIG_HC_IMAGE_HEAP_BY_TASK=y` (it turns on the IDF's heap task tracking: a few bytes per
allocation, for measurements) the `end` and the after lines add every task whose heap changed by
256 B or more since the load began (`swap heap: end: task <name> holds <n> B (<+d> B since the load
began)`).

The phase's lowest is `heap_caps_get_minimum_free_size()` over the IDF's local minimum, restarted at
each phase (`heap_caps_monitor_local_minimum_free_size_start()`); the probe stops it at the end of
the load, before the load's summary line, and the IDF then restores the minimum since boot,
including the load's lows. The largest block is `heap_caps_get_largest_free_block()` at the phase's
end.

## Console and command line

ESP serial console:

```
image status        what is loaded, the stored copy, the last refusal, the loop's stack reserve
image load <url> [<signature>]
                    download an image over http(s) and load it as PUT /api/image does; the
                    signature (128 hex digits) is needed once the device holds the installation key
image clear         unload and remove the image with its persisted state
```

POSIX: `homecontrol --image <file>` loads the file at start (exit status 1 with the refusals in the
log when it is invalid), with the signature from `<file>.sig` when that file exists; `PUT /api/image`
replaces it while running. `--demo` and `--image` exclude each other.

**Signatures** (issue #35, spec/api.md "Signed runtime images"): while the device holds the
installation key (`ehome.key`, from the E-Home enrollment) every load except the boot load of the
stored image needs the image's Ed25519 signature: the header `X-Image-Signature`, the socket's
`signature`, the console argument, the `.sig` file. `scripts/sign.py image <secret-file> <image>`
writes `<image>.sig` (for tests; E-Home signs its deployments).

## Deploying by hand

Until E-Home deploys (its side of issue #8), an image is written by hand. Start from
`spec/fixtures/images/example.json` or, for the KNX test system, from
`spec/fixtures/images/test-system.template.json`:

1. Copy the template outside the repository and replace every `"GA:<name>"` by the project's group
   address (`"GA:key3"` → the address the Tastsensor's key 3 sends on, `"GA:chC"` → channel C's
   switching object, `"GA:chC.state"` → its status object, `"GA:temperature"` → the room
   temperature, `"GA:time"` → the time address, or remove `knx.timeMaster`). Real addresses are
   never committed.
2. Create a token on the console (`web token create deploy`) and push:
   `curl -s -H "Authorization: Bearer $TOKEN" -X PUT --data-binary @image.json http://<device>/api/image`.
   A refusal comes back as 422 with every problem's path; fix and push again.
3. The log shows `image 'knx-test-system' revision 1 ... loaded from api`, the read requests of the
   state datapoints, and the rules starting. The LED goes from the short pulse to `ready` (or
   `degraded` while the tunnel is down). The UI's Overview shows the datapoints and the project.
4. Deploy a new revision the same way: values of unchanged datapoints and the rules' timers stay.
