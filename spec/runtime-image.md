# The runtime image

*Specification of issue #8: the document E-Home deploys to a controller, describing everything the
controller runs: the datapoints and their KNX addresses, rooms and functions, the time master, the
schedules, the rules, the history budget and the settings the project fixes. Implemented by
`hc::image` (`core/include/hc/image/`, design and operation in `doc/image.md`); transported by
`PUT /api/image` (`spec/api.md`). Members follow the conventions of `spec/rules.md` (camelCase
names, the JSON value forms of its section 3.1). Fixtures: `spec/fixtures/images/`.*

## 1. Decisions

- **I1. JSON, UTF-8, on the wire and in flash.** The image is one JSON object, stored byte for byte
  as it arrived; `GET /api/image` returns those bytes. *Alternatives:* CBOR or a custom binary
  form (smaller by about a third, but E-Home and every test would need an encoder, and a stored
  image could not be read with `curl`); compiling the image to C structures in E-Home (no
  validation on the device, a firmware build per project). The device parses with the core's
  `json_parse` (`JsonSpan`, no tree is built) through a `JsonDocument` adapter for the rule loader,
  so JSON costs no second parser.
- **I2. The whole text in RAM once, at load.** The loader needs random access (rules refer to
  datapoints declared earlier or later). The text is held only while the image is validated and
  applied; then it is written to the blob store and freed. *Alternative:* a streaming parser over
  flash (no buffer, but every cross-reference becomes a second pass over flash, and the rule loader
  would need a streaming interface it does not have). The cost is the image's size for the
  duration of a load: 7 KB for the example of this document, 6 KB for the test-system template;
  the limit (I3) bounds it.
- **I3. At most 256 KB** (`image::kMaxImageBytes`). Larger bodies are refused before they are read
  (413). A realistic house (200 datapoints, 40 rules) is 60..120 KB. On an ESP32-C6 a body of
  more than about 100 KB may find no free heap block of its size; the device then answers 507
  and keeps its image.
- **I4. Versioned by `image.schemaVersion`, `<major>.<minor>`.** A newer major version is refused
  naming the version (`schemaVersion 2.0 is newer than this firmware reads (1.x, up to 1.0)`); a
  newer minor version of a known major loads. *Alternative:* a feature list (more precise, but
  E-Home would have to know which features a firmware has).
- **I5. Unknown members are ignored, except in `rules` and `schedules`.** A minor version may add
  members; an older firmware loads the image without them. The rule and schedule objects keep
  their own strict policy (`spec/rules.md` 10, `spec/schedules.md` 2: unknown members are refused,
  `description` and `comment` are allowed everywhere), because a misspelt trigger or time must not
  silently disappear. A top-level `comment` (or anywhere outside rules and schedules) is ignored
  too.
- **I6. A missing optional member means "none".** No `rules`: no rules; no `knx.timeMaster`: no time
  master; no `settings.tz`: the device keeps its own setting. Nothing defaults to "keep what the
  previous image had": an image describes the whole configuration.
- **I7. Validate everything, then apply atomically.** Every refusal is collected with the JSON path
  of the offending value and a reason (up to 12 reported, all counted), before anything changes.
  A refused image leaves the running one untouched. Section 6.
- **I8. Values survive a swap where they mean the same.** A datapoint whose `id` and `type` are
  unchanged keeps its value, quality and update time; rules keep their timers and states by id
  (`spec/rules.md` 7.6). Section 6.

## 2. The document

```json
{
	"image": {"schemaVersion": "1.0", "projectId": "example-house", "revision": 7,
	          "generatedAt": "2026-10-17T12:00:00Z", "generator": "E-Home 4.2", "name": "Example house"},
	"settings": {"tz": "CET-1CEST,M3.5.0,M10.5.0/3", "location": {"lat": 48.137, "lon": 11.576, "elev": 520},
	             "ntp": {"server": "pool.ntp.org"}},
	"datapoints": [ ... ],
	"locations": [ ... ],
	"functions": [ ... ],
	"knx": {"timeMaster": { ... }},
	"schedules": [ ... ],
	"rules": [ ... ],
	"history": {"budget": 4096},
	"ui": {}
}
```

Complete example: `spec/fixtures/images/example.json` (placeholder addresses `1/x/y`).

| Member | Required | Section |
|---|---|---|
| `image` | yes | 3.1 |
| `settings` | no | 3.2 |
| `datapoints` | no (none) | 3.3 |
| `locations` | no (none) | 3.4 |
| `functions` | no (none) | 3.5 |
| `knx` | no (no time master) | 3.6 |
| `schedules` | no (none) | `spec/schedules.md` |
| `rules` | no (none) | `spec/rules.md` |
| `history` | no (budget = what the rings need) | 3.7 |
| `ui` | no | 3.8 |

Strings are 1..N bytes of valid UTF-8 without control characters; N is given per member. A
*label* is either a string (1..63 bytes) or an object of language tags (2..8 characters of
`A-Z a-z 0-9 -`) to such strings, e.g. `{"en": "Hall", "de": "Flur"}`.

## 3. Members

### 3.1 `image`

| Member | Type | Required | Meaning |
|---|---|---|---|
| `schemaVersion` | string `<major>.<minor>` | yes | This document is `1.0`. |
| `projectId` | string, 1..63 | yes | E-Home's project. Shown in the status. |
| `revision` | integer 0..4294967295 | yes | E-Home's deployment counter, shown in the status and returned by PUT. The device does not refuse an older revision: rolling back is a deployment like any other. |
| `generatedAt` | string, 1..31 | no | ISO 8601 time of the export. |
| `generator` | string, 1..63 | no | The exporting tool and version. |
| `name` | string, 1..63 | no | The project's display name. |

### 3.2 `settings`

The device settings the project fixes. Each is applied only if it differs from the stored value
(no flash write for an unchanged image), validated as on the console.

| Member | Type | Meaning |
|---|---|---|
| `tz` | POSIX TZ string, 1..127 | The setting `tz` (`doc/time.md`). |
| `location` | `{"lat": -90..90, "lon": -180..180, "elev": -500..9000}` (`elev` optional, metres, integer) | The coordinates for the sun times of astro schedules and `sunrise()`/`sunset()`. |
| `ntp` | `{"server": host name}` | The setting `ntp.server`; SNTP restarts with it. |

### 3.3 `datapoints`

An array of datapoint objects (at most 1024). The array order is the model order (the API's ids
0, 1, ...).

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | string, 1..63 printable bytes, unique | yes | The datapoint's name everywhere: rules, schedules, functions, the API and the persisted values. Ids `knx.time`, `knx.date`, `knx.datetime` are reserved when the image has a time master. |
| `type` | `boolean`, `unsigned`, `signed`, `real`, `string`, `timeOfDay`, `date`, `dateTime`, `enumeration`, `controlled`, `color`, `raw` | yes | The value type (`doc/datapoints.md`). |
| `label` | label | no | For the UI. |
| `unit` | string, 1..15 | no | For the UI (`°C`, `%`, `lx`). |
| `readable` | boolean | no, false | Others may read it (KNX: the device answers a GroupValue_Read on the sending address). |
| `writable` | boolean | no, false | Rules, the UI and the API may command it. |
| `transmit` | boolean | no, false | A commanded value is sent to the technology. |
| `readOnStart` | boolean | no, false | The technology reads the value after a start or reconnect. |
| `persist` | boolean | no, false | The last value is kept across restarts (`doc/persistence.md`). |
| `history` | `{"retention": s, "spacing": s}` | no | A short-term history ring: `spacing` 10..86400 s, `retention` from the spacing up to 31 days, at most 2048 samples; only `boolean`, `unsigned`, `signed`, `real`. |
| `knx` | object | no | The KNX binding, below. |
| `zigbee` | object | no | The Zigbee binding, below. Excludes `knx` and `virtual`. |
| `virtual` | object (members reserved) | no | A value of the application: no technology; rules and the UI command it. Excludes `knx` and `zigbee`. |

The KNX binding:

| Member | Type | Required | Meaning |
|---|---|---|---|
| `dpt` | `"<main>.<sub>"` | yes | A supported DPT whose value type fits `type` (numeric types are interchangeable). |
| `send` | group address `"main/middle/sub"` | if `transmit` or `readable` | The address the device writes and answers on. |
| `listen` | array of up to 8 group addresses | no | Further addresses whose telegrams update the datapoint (status feedback, other senders). |

A KNX datapoint needs at least one address. A datapoint without `knx`, `zigbee` and `virtual` is a
plain application value too; a schedule may own it as its `output`.

Implied datapoints of the rules (`spec/rules.md` 8.6): the loader adds one datapoint per rule
parameter, the enable switch included, named `rule/<ruleId>/<parameterId>`, behind the image's own
datapoints and the time master's, in rule order (per rule `enabled` first, then the parameters in
their order). Its type follows the parameter's (`duration` as `unsigned` milliseconds); it is
writable exactly when the parameter is adjustable, not persisted by the datapoint model (the rule
engine's parameter store keeps the overrides) and without history; the rule engine owns it. Ids
starting with `rule/` are reserved: a declared one refuses the image. Functions, schedules and
the UI may name these datapoints like any other.

The Zigbee binding (issue #29; the bindings, the checks and their refusals in `spec/zigbee.md`
"Bindings"): the datapoint stands for one function of one endpoint of a joined (or not yet joined)
device, and a device with a binding gets only its bound datapoints.

| Member | Type | Required | Meaning |
|---|---|---|---|
| `ieee` | string, 16 hex digits (colons allowed) | yes | The device's IEEE address; not 0, not all ones. |
| `endpoint` | integer 1..240 | yes | |
| `cluster` | integer 0..65535 | yes | With `attribute`, one function of the table in `doc/zigbee.md`; its value type must fit `type` (numeric types and enumeration are interchangeable). |
| `attribute` | integer 0..65535 | unless `client` | |
| `client` | boolean | no, false | The device's client cluster commands (a button, cluster 6 or 8): an `enumeration` of actions. |
| `command` | integer 0..65534 | if `transmit` | The cluster the commands go to: the function's own, or 8 (Level Control) for On/Off. Absent: read-only. |
| `reporting` | object | no | The reporting the controller configures for the function instead of its defaults: `min`, `max` (seconds; `max` 0 no periodic reports, 65535 no reports) and `change` (in the datapoint's unit), each optional (`spec/zigbee.md` "Reporting overrides"). Not for a client binding, `multistate` and `ias_zone`. |

At most 512 Zigbee bindings; no function of an endpoint is bound twice. The controller adds the
generic datapoints of the joined devices without bindings behind the image's datapoints
(`zigbee.<ieee>.<endpoint>.<suffix>`, `doc/image.md` "Dynamic datapoints"); an `id` equal to one of
them refuses the image while that device is joined. Rules and functions name Zigbee datapoints
through bindings: the generic names are not the image's.

### 3.4 `locations`

A flat list; the tree is formed by `parent` (at most 256 entries).

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | 1..32 characters of `A-Z a-z 0-9 _ . -`, unique | yes | |
| `label` | label | yes | |
| `kind` | `building`, `floor`, `room`, `area`, `outside` | no | |
| `parent` | location id | no | Must exist; the parents must not form a cycle. |

### 3.5 `functions`

What the inhabitant operates: a light, a blind, a sensor. A function names the datapoints of its
*roles* (at most 512 functions).

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | 1..32 characters of `A-Z a-z 0-9 _ . -`, unique | yes | |
| `label` | label | yes | |
| `kind` | below | yes | |
| `room` | location id | no | |
| `unit` | string | no | For the UI. |
| `scene` | integer 1..64 | for `scene` | The scene number as people count it. The device sends the KNX scene code `scene − 1` on the `command` datapoint (DPT 17.001 and 18.001 count from 0). |
| `datapoints` | object role → datapoint id | yes | |

| Kind | Roles (**required**; *command roles need a writable datapoint*) |
|---|---|
| `light` | ***command*** (boolean), `state` (boolean) |
| `dimmer` | ***levelCommand*** (number), `level` (number), *command* (boolean), `state` (boolean), *step* (controlled: relative dimming, KNX DPT 3.007; the control bit is the direction, 1 brighter, the value the step code 1..7, 0 stops) |
| `blind` | ***move*** (boolean), *stop* (boolean), `position` (number), *positionCommand* (number), `slat` (number), *slatCommand* (number) |
| `switch` | ***command*** (boolean), `state` (boolean) |
| `sensor` | **value** (any type) |
| `climate` | **temperature** (number), `setpoint` (number), *setpointCommand* (number), `mode` (enumeration or number), *modeCommand* (enumeration or number), `heating` (boolean) |
| `scene` | ***command*** (number, enumeration or controlled) |
| `button` | **press** (boolean or controlled), `state` (boolean) |

"number" is `unsigned`, `signed` or `real`. Unknown roles are ignored (I5).

### 3.6 `knx`

`timeMaster`: the controller broadcasts the time (`doc/knx.md` "Time master").

| Member | Type | Required | Meaning |
|---|---|---|---|
| `time` | group address | at least one of the three | DPT 10.001 time of day. |
| `date` | group address | | DPT 11.001 date. |
| `dateTime` | group address | | DPT 19.001 date and time. |
| `interval` | integer 60..86400 s | no, 600 | Period of the broadcasts. |
| `atMidnight` | boolean | no, true | Also right after local midnight. |
| `onSynchronized` | boolean | no, true | Also when the wall clock becomes synchronized. |

The loader adds the datapoints `knx.time`, `knx.date`, `knx.datetime` for the configured
addresses (after the image's own). The addresses must not be the sending address of a datapoint.

### 3.7 `history`

`{"budget": bytes}`: the sample memory the image may use for its rings. Default: what the rings
need. Refused when the rings need more than the budget, or the budget exceeds the device's cap
(64 KB on the ESP32-C6, `HistoryLimits::cap_bytes`).

### 3.8 `external`

Optional. The HTTP JSON sources (weather and other services) that feed datapoints, in the shape of
`spec/external-values.md` (`{"sources": [...]}`): sources with a `url` or a `preset`, an interval and
bindings from JSON paths or preset variables to datapoints of this image. It is validated and applied
after the datapoint model it binds to; a refusal names the path under `external` and the image is not
applied.

### 3.9 `ui`

An object reserved for E-Home's presentation hints (dashboard order, icons). Version 1.0 defines
no members; the device checks only that it is an object.

### 3.10 `mqtt`

Optional. The MQTT broker the controller publishes to and the shape of what it publishes
(`spec/mqtt.md`): `broker` (`mqtt://host[:port]`, `mqtts://`), `username`, `baseTopic`, `retain`,
`discovery` (Home Assistant), `history` (the export of the rings). The password is never in the
image (the device setting `mqtt.password`); an image with `mqtt.password` is refused. The member is
validated with the image and applied after the datapoint model and the functions it names; a
refusal names the path under `mqtt`. Without the member the controller does not talk MQTT.

## 4. Refusals

Each refusal has a **path** (`datapoints[3].knx.send`, `functions[1].datapoints.command`,
`rules[0].reactions[1].actions[0].datapoint`; `""` for the document as a whole) and an English
**reason**. Kinds, as `spec/api.md` reports them:

| Kind | Code | HTTP | When |
|---|---|---|---|
| syntax | `image.bad_json` | 400 | Not JSON (the reason names the byte offset), or nested deeper than 48. |
| version | `image.version` | 422 | `schemaVersion` with a newer major version. Nothing else is checked. |
| invalid | `image.invalid` | 422 | Everything else the document says wrong. |
| too large | `image.too_large` | 413 | More than 256 KB. |
| no memory | `image.no_memory` | 507 | The device could not allocate the tables. Nothing changed. |
| apply | `image.apply_failed` | 500 | Valid, but a component refused while it was applied (section 6); the previous stored image was loaded again. |
| unsigned | `image.unsigned` | 403 | The device holds the installation key and the image comes without its signature (`spec/api.md` "Signed runtime images"). |
| bad signature | `image.bad_signature` | 403 | The signature does not verify under the installation key over the image's exact bytes (400: not 128 hex digits). |

The signature is checked after the size limit and before the document is parsed: a signed image
with invalid content reports the loader's kind, an unsigned one never reaches the loader. The
signature is detached (the header `X-Image-Signature`, the socket's `signature`, a `.sig` file); the
document itself does not change.

Rules are validated last and only when everything before is valid, because their references are
checked against the image's datapoints and schedules. Rule and schedule reasons start with the
reason codes of `spec/rules.md` 10.1 (`malformed:`, `outputConflict:`, `unknownReference:`, ...).

## 5. Limits and RAM

| | Limit |
|---|---|
| Image text | 256 KB |
| Nesting | 48 levels (rule expressions nest deeply; the API's own bodies stay at 16) |
| Datapoints / locations / functions | 1024 / 256 / 512 |
| Schedule entries | 128 (`spec/schedules.md`) |
| Listening addresses per datapoint | 8 |
| Reported refusals | 12 with path and reason; up to 100 counted |

RAM while loading: the text, the `Plan` (about 0.8 KB) and its load tables (one or more 4 KB
blocks), plus the scratch model the rule validation builds. RAM kept: the model's tables, the
rule set and its runtime, the schedules, the KNX tables and the history rings (the budget).
Measured on the host (64-bit, so pointers count double): the example keeps about 9 KB besides its
stored copy, of which 2.3 KB are history; the test-system template 3 KB plus 11.5 KB of history.

## 6. Applying an image

The event loop applies a valid image in this order:

1. The rules' queued events are processed with the old rules (`Engine::flush`).
2. The datapoint model is replaced. Datapoints whose `id` and `type` are unchanged keep value,
   quality and update time; the others start unknown. *A failure up to here leaves the old image
   running.*
3. The settings are written where they differ.
4. The value store and the history are loaded for the new model: persisted values are restored
   where the model has no kept value; the history saves its rings and restores them by datapoint
   name into the new rings (`doc/persistence.md`).
5. The rules are loaded; each rule's runtime (timers, states, parameter overrides, change
   tracking) follows its id (`spec/rules.md` 7.6), datapoints are followed by name.
6. The KNX adapter gets the new group address table; then the time master, whose broadcast
   addresses are merged into the table. Read-on-start datapoints are read again.
7. The schedules are loaded.
8. The rules start with reason `boot` (the first image after a restart) or `image` (a swap).

A failure from step 3 on (out of memory, a component refusing what the loader accepted) tears the
new configuration down and loads the stored image again (source `restore`); without a stored image
the device has none. The image text is stored only after step 8 succeeded.

## 7. Storage

The device keeps the last image that loaded and loads it at boot. Blobs are limited to 20 KB
(`BlobStore::kMaxBlobSize`), so the text is stored in 16 KB chunks `image.<g>.<n>` of generation
`g` (`a` or `b`, alternating) with a manifest `image.idx` (`HCIMG 1 <g> <bytes> <chunks>
<crc32>`), written last: the manifest's atomic replace switches generations, a power cut at any
moment leaves either the old or the new image, and the CRC-32 detects a damaged chunk. Chunks
without a manifest are removed at start.

`DELETE /api/image` (and the console's `image clear`) unloads the image and removes it, with the
persisted values and history rings: the device returns to setup.

## 8. Changes

- 1.0 (issue #8): first version.
