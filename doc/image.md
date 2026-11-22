# The runtime image on the controller

The format is `spec/runtime-image.md` (the binary image: sections 9 to 13), the endpoints
`spec/api.md` "Runtime image". This note describes how the controller receives, checks, applies and
keeps an image (`hc::image`, `core/include/hc/image/`; issue #87). E-Home compiles the image from
its JSON source form; the device has no JSON reader for images.

## Parts

| Class | Role |
|---|---|
| `ImageSlot` (`hc/platform/image_slot.h`) | The platform's slot: one image, mapped for reading, written sector by sector. ESP: `EspImageSlot`, the partition `image` (data, subtype 0x40) mapped through the cache. POSIX: `FileImageSlot`, the file `<settings stem>.image.bin`, 512 KB, read into one heap block. Tests: `MemoryImageSlot`. |
| `image::bin::View` (`hc/image/binary.h`) | The records of spec 9 as C++ structs of exactly their layout, read in place: section lookup, strings, labels (resolved in a language, or written as the image gave them). |
| `image::check_*` (`hc/image/validator.h`) | Section 10: the header (10.1), the CRC (10.2), the structure (10.3 to 10.7) and the counted limits of 10.8. Every refusal with its path into the image and its reason. |
| `image::Loader` | The `Plan` of a structurally valid image: the components' specs built from the records (strings pointing into the slot) and the checks of 10.8 that need this firmware's catalogues (DPTs, the Zigbee function table, the presets, the block types, the history caps, the settings' validators, the persistence keys). |
| `image::ImageService` | Owns the scheduler, the rule engine and its clock, the block runtime, the value store, the history and the KNX time master; receives into the slot, checks, applies (spec 6), loads the slot's image at boot, unloads. It is the controller's `LedInput`. |
| `rules::RuleSet` | The rules of the image in place: the records, nodes and constants are the image's (`rules::Rule`, `Node`, `Val` have the binary layouts); RAM holds the derived tables (tracked datapoints, match and silence triggers) and the image's timelines as `Timeline` objects. |
| `image::ModelView` | GET /api/model written from the image: locations, functions, roles, labels, units, bindings. |
| `WebApi` | `PUT`/`GET`/`DELETE /api/image`, `GET /api/pages` (the image's `ui` section as it is), the `image` member of the status; a load or unload resyncs the live feed. |

## Receiving an image

`ImageService::receive_begin(head, available, length, source)` checks the header in the first 32
bytes against the announced length (10.1: magic, major version, flags, length, header size, the
section table's end; the slot's size: 413; an unsigned image while the device holds the
installation key: 403). A refusal here leaves the running image untouched. Otherwise the service
stops the running image (below, "The swap"), unmaps the slot and opens the receive:
`receive_write()` writes the body from its first byte, erasing each 4 KB sector just before its
first byte; `receive_end()` maps the slot and checks the CRC, the signature (on receipt only, I11)
and the structure, builds the plan and applies it. `receive_abort()` (the body broke off) and every
refusal from here on leave the device without an image (I9): it runs on, shows the setup state and
reports the refusal in the status and the E-Home connection's `auth`; E-Home sends the last valid
deployment again.

The receivers:

- **The ESP's HTTP server** (`hc_web`) reads the first 32 bytes, runs `WebApi::image_begin()` on the
  loop (it authenticates the request as `handle()` would and refuses a detached signature), then
  writes the rest from the httpd task through `receive_write()` (the loop does not touch the slot
  while a receive is open) and ends with the request on the loop (`WebRequest::image_streamed`). A
  connection that drops or stalls aborts the receive on the loop.
- **The E-Home connection** (`EhomeLink`, on the loop): `PUT /api/image` only with `length` and binary
  frames; it collects the header, calls `image_begin()`, writes each frame as it arrives (the
  sector erases run on the loop, about 45 ms per 4 KB), and dispatches the request after the last
  byte. A session that ends mid-body aborts the receive.
- **The POSIX server** gets the whole body from cpp-httplib and calls `ImageService::load()` on the
  loop (begin, write, end in one call), as `homecontrol --image <file>` and the tests do.
- **The ESP console** `image load <url>` streams the download through the same three calls.

`GET /api/image` and `GET /api/pages` (when the section is larger than the response buffer) are
streamed from the mapped slot in pieces through `WebApi::read_stream()`; a load or unload in between
changes the service's token and cuts the response.

## Checks, once

The validator walks every section, every record, every reference (string, label, index, run), every
code and flag and every expression (its nodes form a tree inside its range, no deeper than 12, at
most 64 nodes) before anything runs. After that nothing is checked at access: the components read
the image as it is. Bytes the C++ types cannot hold (a `bool` field that is neither 0 nor 1) are
checked as bytes. The probe `ImageProbe.BitFlipsAreRefusedOrSafe` flips single bits of real images
(with the CRC written anew) and requires each to be refused with a reason or to build and apply
without a fault, under ASan. At boot the same checks run without the signature; an erased slot
(`FF FF FF FF` at its start) is no image, not a refusal.

## What runs in place, what is built in RAM

| | In place (the slot) | In RAM |
|---|---|---|
| Datapoints | names, labels, units, owners | the model's datapoints (value, quality, time) and its copy of the names (the model's own table, shared with the providers' datapoints) |
| Rules | the records, the expression nodes, the constants, the strings, the labels | the runtime (timers, states, change records, diagnostics), the derived tables, the image's timelines (`Timeline` objects: the inhabitant's replace them) |
| Schedules | ids, labels | the scheduler's entry table (its members are adjustable at run time) |
| KNX | the address table | the adapter's mapping (built from the bindings) |
| Blocks | the records | the `BlockSet` specs and the runtime's state records |
| History, external values, MQTT | names, paths, units | the rings, the sources' tables, the MQTT configuration (a retiring one outlives the mapping, so it copies its strings) |
| Control pages | the `ui` section, served as it is | nothing |

## The swap (spec 12)

Before the slot is erased the service flushes the rules' queue, writes the persisted values, the
history rings and the blocks' records (all by name in the blob store), and takes in RAM what the
new image may keep: a `SwapTable` of the values (FNV-1a of the name, value, quality, update time;
two names with one hash drop both, logged) and the engine's `Carry` (`Engine::detach()`: per rule
the hash of its id, of its current state and of its timers and parameters, its runtime record, its
running timers, its parameter overrides, the inhabitant's timeline as canonical text, and the change
records of the tracked datapoints by name hash). The block runtime keeps its old `BlockSet` (RAM
copies of the ids): `BlockRuntime::adopt()` carries the state records by block id as before. Then
the components stop and the model is cleared: during the receive the device has no image.

After the new image validated, the model is loaded from its datapoints and every datapoint whose
name hash and value type are in the table takes value, quality and update time
(`DatapointModel::carry()`); the value store and the history load (restoring by name what the table
did not give); an application datapoint that still has no value takes its initial value, the
constant its record names (`ImageService::apply_initial_values()`, spec 3.3, format 1.2); `Engine::adopt(set, carry)` installs the rules with their kept runtime; then the
blocks, KNX, the time master, the schedules, the external values; the rules start with reason
`image`. The table is freed once an image applied; a failed image keeps it, so that E-Home's next
deployment of the last valid image finds the values.

## Dynamic datapoints

Some datapoints come and go at run time: a Zigbee device joins or is removed. A
`DynamicSpecProvider` registered with `ImageService::add_provider()` states the datapoints it needs
(`dynamic_specs()`), given the image's Zigbee bindings it serves (`ImageBindings`). The service
composes the model from the image's datapoints, at the image's positions, followed by every
provider's. A name that appears twice refuses the composition (`image.apply_failed` for an image, a
reload keeps the old model; `ImageService::dynamic_refusal()` holds the reason).

When a provider's set changes the service reloads the model with `keep_values` from a timer, at most
once per second (`kReloadSpacingMs`), from the plan of the image in the slot (kept while the image is
loaded: it points into the slot), and binds every component again (value store, history, KNX, time
master, schedules, external values, the providers); the rules continue. Without an image the model
holds the providers' datapoints alone.

## LED and settings

`Controller::led_status()`: pairing, then connecting (network down), then `setup` when no image is
loaded (also during a receive and after a refused image), `degraded` when the image has KNX
datapoints (or a time master) and the KNX tunnel is down, `ready` otherwise. `tz`, `location` and
`ntp.server` of the image's `settings` are written only when they differ from the stored values.

## Memory

The device keeps no copy of the image in RAM (spec 5). A load allocates the plan's tables (the
datapoint specs, the KNX mapping, the schedule and history specs; freed with the image) and the
components' running state; a swap holds the swap table (56 bytes per datapoint with a value on the
C6's layout) and the engine's carry while the slot is written. The figures before and after issue
#87 are in the issue (its acceptance) and in the tests `Swap memory` and `Memory probe`.

## Console and command line

ESP serial console:

```
image status        what is loaded (format, CRC, signed), the slot, a receive in progress, kept values
image load <url>    download a binary image and deploy it as PUT /api/image does (its signature inside)
image clear         unload, erase the slot, remove the persisted state
```

POSIX: `homecontrol --image <file>` deploys the binary image in the file at start (exit status 1
with the refusals in the log when it is refused); the slot is the file `<settings stem>.image.bin`,
which the next start loads.

## Deploying by hand

E-Home compiles and signs the image (haumacher/ehome#176). A compiled image (`.bin`) is deployed with
a token (`web token create deploy` on the console):
`curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/octet-stream" -X PUT --data-binary @image.bin http://<device>/api/image`.
A refusal comes back with every problem's path into the binary image. The host tests compile their
JSON images with the test-only compiler (`core/test/image_compiler.h`).
