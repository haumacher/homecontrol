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
a deploy is therefore the old image's steady state minus the swap's peak above it. The phases, as
`ImageService` and the `Loader` report them to a `LoadProbe` (`hc/image/load_probe.h`), each named
at its end:

| Phase | What it allocates (+) or frees (-) |
|---|---|
| `signature` | the signature check (nothing on the heap) |
| `json` | + the plan (the `Plan` object) |
| `datapoints` | + the datapoint tables, the KNX mapping, the index by name; - their scratch (the blocks' implied datapoints) |
| `schedules`, `history`, `ui` | + locations, functions, schedules, history specs; the UI is checked, not kept |
| `rules scratch`, `rules` | + a scratch `DatapointModel` of the image's datapoints, + the compiled rule set (kept), - the scratch and the compiler's work |
| `blocks scratch`, `blocks` | + a second scratch model, + the compiled `BlockSet` (kept), - the scratch |
| `compose`, `model` | + the composed spec table, + the new model; - the old model |
| `persistence` | the value store and the history bound and restored (their tables for the new image) |
| `engine` | the rule set adopted; - the old rules |
| `block runtime` | + the instances' runtime, the mirrors bound; - the old runtime and set (`BlockRuntime::adopt`) |
| `technologies` | KNX mapping, time master, schedules, external values, the providers |
| `start` | the rules and blocks started; - the old image's kept plan (with a provider) |
| `release` | - the new plan (kept instead with a provider) |
| `mqtt`, `view`, `store` | the MQTT member, the UI model, the store (off the loop since #68) |

Measured with the C6's sizes (`scripts/c6-model`, below) on E-Home's revision 11 of the KNX test
system (23 482 bytes, 58 datapoints, 10 rules, 6 schedules, one history with archives, a provider
registered as the Zigbee adapter is), with k blocks added as E-Home's revision 13 adds them (a room
controller on the room temperature switching an LED, a heating demand over its valve requesting on
another; k = 4 is two such pairs). Bytes above the start of the load, as the C6's heap charges them
(TLSF: each block at least 12 bytes, rounded up to 4, plus a 4-byte header); "k over j" deploys the
image with k blocks over the one with j:

| Phase peak | 0 over 0 | 1 over 0 | 1 over 1 | 2 over 1 | 2 over 2 | 4 over 3 | 4 over 4 |
|---|---|---|---|---|---|---|---|
| `json` | 884 | 884 | 884 | 884 | 884 | 884 | 884 |
| `datapoints` | 14 040 | 14 160 | 14 160 | 15 692 | 15 692 | 18 360 | 18 360 |
| `ui` | 8 560 | 9 724 | 9 724 | 9 304 | 9 304 | 10 524 | 10 524 |
| `rules scratch` | 14 628 | 17 132 | 17 132 | 17 360 | 17 360 | 20 588 | 20 588 |
| `rules` | **27 324** | **29 828** | **29 828** | **30 056** | **30 056** | **33 284** | **33 284** |
| `blocks scratch` | 21 704 | 24 208 | 24 208 | 24 436 | 24 436 | 27 664 | 27 664 |
| `blocks` | 21 704 | 24 940 | 24 940 | 25 568 | 25 568 | 29 920 | 29 920 |
| `model` | 23 724 | 26 924 | 26 924 | 27 536 | 27 536 | 31 840 | 31 840 |
| `persistence` | 17 656 | 20 856 | 19 516 | 20 128 | 19 480 | 22 428 | 21 776 |
| `engine` | 20 060 | 23 260 | 21 920 | 22 532 | 21 884 | 24 832 | 24 180 |
| `block runtime` | 10 624 | 14 936 | 13 596 | 14 928 | 14 280 | 19 060 | 18 408 |
| `technologies` | 11 672 | 15 984 | 13 020 | 14 352 | 12 680 | 15 840 | 14 164 |
| `start`, `release` | 10 580 | 14 892 | 11 928 | 13 260 | 11 588 | 14 748 | 13 072 |
| after the load | 0 | 4 312 | 0 | 1 332 | 0 | 2 380 | 0 |
| the old image held | 35 156 | 35 140 | 39 452 | 39 452 | 40 784 | 44 540 | 46 920 |
| old image + peak | 62 480 | 64 968 | 69 280 | 69 508 | 70 840 | 77 824 | 80 204 |

The peak is the rules' compilation (the plan, a scratch model and the compiler's work beside the
whole old image) and grows about 1.5 KB per block; the `model` step grows about 2 KB per block and
becomes the peak from about seven blocks. A block's steady cost here is 4.3 KB for the room
controller (16 mirror datapoints) and 1.3 to 2.4 KB for the heating demand: at the peak a block
costs its steady size in the old image plus at most its steady size in the new one, nothing more.
Measured on the C6, revision 12 (one block, over revision 11 without blocks) and revision 13 (two
blocks, over revision 12) differ by about 4.5 KB at the lowest point: the room controller is in
both images of the second swap (4.3 KB held by the old one) and the heating demand adds 0.2 KB to
the peak. (The board's 7.6 KB came from two firmware builds; its steady figures put the room
controller at 0.6 KB, so they do not resolve 4 KB either.)

`Swap memory: a block costs at the peak at most its steady size; the swap stays within its budget`
pins this on the host (64-bit, `example.json` with 0 to 4 blocks, over the same image and over one
block fewer): the peak above the old image is at most **16 KB** (the swap's budget for that image
without blocks; measured 13.2 KB on the host, 12.0 KB with the C6's sizes) plus k times a block's
steady cost, a block adds to the peak at most its steady cost, a redeploy holds what the first
deploy held, and nothing of the load is held at `release` beyond the new image. The probe and the
whole-heap counter are in the test (`core/test/swap_memory_test.cpp`; the counter's
`hc_test::heap_mark()`, `HeapExclusion` for the fakes of flash, NVS and the log ring); the table
above is its measurement case, run with `HC_SWAP_IMAGE=<image without blocks>`.

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
level, logs every load's phases:

```
swap heap: begin: free <F> B, lowest since boot <M> B
swap heap: json           free <f> B, lowest in the phase <l> B (<F - l> B below the start)
...                       (one line per phase of the table above)
swap heap: end (loaded): free <f> B, lowest of the load <L> B (<F - L> B below the start), lowest since boot <M'> B
```

The phase's lowest is `heap_caps_get_minimum_free_size()` over the IDF's local minimum, restarted at
each phase (`heap_caps_monitor_local_minimum_free_size_start()`); the probe stops it at the end of
the load, before the load's summary line, and the IDF then restores the minimum since boot,
including the load's lows. A heartbeat or System page minimum below the load's lowest means the
low came after the load (the store off the loop, the deployer's follow-up requests).

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
