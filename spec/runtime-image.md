# The runtime image

*Specification of issue #8 (the content) and issue #87 (the binary form; E-Home's side
haumacher/ehome#176): what E-Home deploys to a controller, describing everything the controller
runs: the datapoints and their KNX addresses, rooms and functions, the time master, the schedules, the
rules, the blocks, the history budget and the settings the project fixes.*

*The image has two forms. The **source form** (sections 2 to 8) is a JSON document: E-Home writes
it, simulates and tests it, people read it, and the corpus `spec/fixtures/images/*.json` is written
in it. Its members follow the conventions of `spec/rules.md` (camelCase names, the JSON value forms
of its section 3.1). The **binary image** (sections 9 to 13) is what the controller receives,
keeps in its flash partition `image` and works from: E-Home's compiler makes it from the source form
as the last step of a deployment. The binary image is the device's contract; the device has no
JSON reader for images. Implemented by `hc::image` (`core/include/hc/image/`, design and operation
in `doc/image.md`); transported by `PUT /api/image` (`spec/api.md`). Fixtures:
`spec/fixtures/images/` (the sources `*.json` and beside them the compiled `*.bin`, section 13).*

## 1. Decisions

- **I1. JSON is the source form, a compiled binary image the device's form.** E-Home validates the
  source form completely (every check of sections 3 and 4 and of `spec/rules.md` 10,
  `spec/schedules.md` 6, `spec/blocks.md` 11) and compiles it into the binary image of section 9:
  every reference resolved to a table index, every string once, the rules as flat expression trees,
  the control pages as one finished JSON text. The device checks only the structure (I7) and what
  only it knows (its DPT codecs, its Zigbee function table, its block types, its memory caps). The
  owner (2026-11-16): "Abbild 'parsen' - kann das nicht ehome machen und ein direkt verwendbares
  Binärabbild erzeugen?" *Alternatives:* JSON on the wire and in flash, parsed on the device
  (until firmware 1.2: the whole text had to be contiguous in RAM while loading, which forced a
  staging area behind the updater, chunked storage in two generations and a heap fallback, and
  limited a deployable image to about 60 KB on the C6); CBOR (smaller, but still parsed into RAM
  tables); C structures compiled into the firmware (a firmware build per project).
- **I2. Used in place.** The device maps its image partition into the address space and works from
  flash: the tables, the strings, the rule trees, the schedules, the control pages. RAM holds only
  the running state: the values, the timers, the history rings, the rule and block runtime. The
  binary layout is therefore fixed-size little-endian records with indices into tables, aligned for
  the ESP32-C6's cache (9.1), without compression and without variable-length integers.
  *Alternatives:* a compressed image unpacked into RAM (the RAM problem again); variable-length
  records (no random access by index).
- **I3. One partition, one slot, the old image is gone at the first written byte.** The partition
  `image` holds exactly one image: 128 KB on a 4 MB flash (taken from the application partition),
  512 KB on 8 and 16 MB (section 7). A received image is written straight into it, so its size is
  bounded only by the slot. The owner: "wenn das Hochladen scheitert kann man doch ganz einfach die
  letzte gültige Version wieder hochladen, dafür muss man doch auf dem Mini-Device nicht auch noch
  doppelten Platz reservieren". *Alternatives:* two slots (A/B) with a fallback on the device (twice
  the flash for a case E-Home handles by sending the last valid deployment again, I9); a staging
  area beside the stored image (the scheme until firmware 1.2).
- **I4. Two versions.** The source form is versioned by `image.schemaVersion` (`<major>.<minor>`,
  this document 1.8, section 8): it is E-Home's and the corpus's; E-Home's compiler reads every
  schema 1.0 to 1.8 and compiles each with the meaning its version gives it (an image of 1.6 whose
  rule with states declares a parameter `state` keeps that parameter's datapoint and gets no state
  datapoint, section 8). The binary image is versioned by its **format version** `<major>.<minor>`
  in the header (section 11; this document is format 1.1). The device reports the format it reads
  and the size of its slot (`GET /api/status` `device.format` and `image.slot`, the E-Home
  connection's `auth`, `spec/api.md`); E-Home compiles for that format and refuses, before sending,
  an image larger than the slot ("Das Abbild hat N Bytes, der Controller fasst M") or one that needs
  a newer format than the controller reads. The image's `schemaVersion` travels as text only (shown
  in the status). *Alternative:* one version for both forms (a change of the compiled layout that
  the source does not see, or the reverse, would bump a version that says nothing).
- **I5. Unknown parts.** In the source form unknown members are ignored, except in `rules`,
  `schedules`, `blocks` and a button's `presses`, which are strict (`spec/rules.md` 10,
  `spec/schedules.md` 2, `spec/blocks.md` 4; `description` and `comment` allowed everywhere): a
  misspelt trigger, time or press must not silently disappear. E-Home applies these rules when it
  validates the source. In the binary image an unknown section is skipped unless it is marked
  *required*; an unknown code or flag bit in a known field refuses the image, naming the format
  versions (section 11).
- **I6. A missing optional member means "none".** No `rules`: no rules; no `knx.timeMaster`: no time
  master; no `settings.tz`: the device keeps its own setting. Nothing defaults to "keep what the
  previous image had": an image describes the whole configuration. In the binary image an absent
  section is an empty table.
- **I7. Validated once, structurally, then trusted.** On receipt the device checks the signature (the
  installation key, as before); on receipt and at every boot the header, the CRC-32, and then every offset,
  length, index, type tag, count and limit against its section (section 10), before anything runs.
  Semantic checks (types of expressions, roles of functions, ids and their forms, conflicts between
  owners, bounds of adjustable values) are E-Home's and are not repeated. After the validation the
  device reads the image without further checks: no access can leave its table, no expression walk
  can loop, no value the runtime divides by or loops over can be zero or out of range.
  *Alternative:* checking at every access (slower, and a check forgotten at one access is a crash).
- **I8. Values survive a swap where they mean the same.** A datapoint whose `id` and `type` are
  unchanged keeps its value, quality and update time; rules keep their timers and states by id
  (`spec/rules.md` 7.6), blocks their state records (`spec/blocks.md` 8.1). Because the new image
  overwrites the old one in the slot, the device copies what it keeps into a small RAM table before
  the slot is erased, keyed by a hash of the ids (section 12).
- **I9. A failed image leaves no image.** An image that is refused after the slot was erased
  (damaged on the way, a bad signature, a structural refusal, a component refusing it while it is
  applied, a power cut while it is written) leaves the device without an image: it runs on (network,
  web UI, E-Home connection), shows the setup state and reports the refusal (`GET /api/status`, the
  E-Home connection's `status` and `auth`). E-Home then sends the last valid deployment again
  (haumacher/ehome#176). Before the erase the device checks the header (section 10.1): an image in
  another major format, a larger one than the slot, a truncated body or an unsigned image to a
  device that holds the installation key are refused while the running image is untouched.
  *Alternative:* a restore from a second copy on the device (I3).
- **I10. The control pages are one JSON text.** E-Home writes the control pages (`ui`, 3.9) as the
  finished body of `GET /api/pages`, with the datapoints as their ids; the device serves the bytes
  unchanged and never reads them (9.16). *Alternative:* tables of pages, sections and elements in
  the binary image (more format for something only the browser reads).
- **I11. The signature is part of the image, checked on receipt only.** The binary image carries its
  Ed25519 signature at its end (9.17): one form for HTTP, the E-Home connection and a file, and
  `GET /api/image` returns bytes that can be deployed again as they are. The device verifies it when
  it receives an image; at boot it checks the CRC and the structure, not the signature (the owner,
  2026-11-16): only the verified receive path writes the slot, so what the slot holds was verified,
  and a key rotation must not cost the installation its image at the next restart. *Alternatives:*
  the detached signature of `spec/api.md` until firmware 1.2 (a header or socket member, a `.sig`
  file); a check at every boot (an image signed with a rotated-out key would be dropped at a restart
  although nothing about it changed).

- **I12. The image says what may be used without sign-in** (homecontrol #91, E-Home's side
  haumacher/ehome#184). The device has three roles, `anonymous` < `operator` < `administrator`
  (`spec/api.md` "Authentication"); what `anonymous` may is configuration and belongs to E-Home: a
  control page has a minimum role (3.9), and the compiler writes the pages of minimum role
  `anonymous` as a second finished body of `GET /api/pages` (`ui.anonymous`) together with the
  tables of the datapoints and functions those pages show (9.16). The device serves that body
  unchanged (I10 holds: it never reads page JSON) and checks every anonymous request against the
  tables by binary search. The sections are optional: an older firmware skips them and keeps
  everything behind the sign-in, as does an image without them. *Alternatives:* a flag "operable
  without sign-in" per page (it is the lowest role of a ladder that the operator's sign-in, #77,
  builds on); a flag per function (the page already is the selection); a switch on the device
  (configuration belongs to E-Home); the device filtering the page JSON itself (breaks I10).

## 2. The source document

The source form (I1): what E-Home writes, validates and compiles. The device never reads it.

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
	"blocks": [ ... ],
	"history": {"budget": 4096},
	"ui": {"pages": [ ... ]}
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
| `blocks` | no (none) | `spec/blocks.md` (1.3) |
| `history` | no (budget = what the rings need) | 3.7 |
| `ui` | no (no control pages: the generated overview) | 3.9 |

Strings are 1..N bytes of valid UTF-8 without control characters; N is given per member. A
*label* is either a string (1..63 bytes) or an object of language tags (2..8 characters of
`A-Z a-z 0-9 -`) to such strings, e.g. `{"en": "Hall", "de": "Flur"}`.

## 3. Members

The members of the source form. Where this section says that "the loader" adds, derives or
refuses something, E-Home's validation and compiler do it now and write the result into the binary
image (section 9: the implied datapoints are records of `datapoints`, 9.7); the device checks only
what section 10 lists. Where it says what "the device" does at run time, that holds unchanged.

### 3.1 `image`

| Member | Type | Required | Meaning |
|---|---|---|---|
| `schemaVersion` | string `<major>.<minor>` | yes | This document is `1.7` (section 8). The binary image carries it as text (9.6); its own version is the format version (I4, section 11). |
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
| `location` | `{"lat": -90..90, "lon": -180..180, "elev": -500..9000}` (`elev` optional, metres, integer) | The coordinates for the sun times of astro schedules and `sunrise()`/`sunset()`, and for the sun's position of blocks (`spec/blocks.md` B21). |
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
| `writable` | boolean | no, false | The user may command it (the UI, the API, MQTT), and so may rules. A value of the application (`virtual`, or a datapoint without a technology member) is the rules' to write also when it is not writable (1.6, `spec/rules.md` 7.3): `writable` is the user's permission. |
| `transmit` | boolean | no, false | A commanded value is sent to the technology. |
| `readOnStart` | boolean | no, false | The technology reads the value after a start or reconnect. |
| `persist` | boolean | no, false | The last value is kept across restarts (`doc/persistence.md`). |
| `history` | `{"retention": s, "spacing": s, "archives": [...]}` | no | A short-term history ring: `spacing` 10..86400 s, `retention` from the spacing up to 31 days, at most 2048 samples; only `boolean`, `unsigned`, `signed`, `real`. `archives` (1.2, optional): the consolidated archives of the datapoint, below. |
| `knx` | object | no | The KNX binding, below. |
| `zigbee` | object | no | The Zigbee binding, below. Excludes `knx` and `virtual`. |
| `virtual` | object (members reserved) | no | A value of the application: no technology; rules write it, the user commands it where it is `writable`. Excludes `knx` and `zigbee`. |

The archives of a history (issue #56): up to 4 objects `{"resolution": s, "retention": s}`, each a
store of consolidated slots of `resolution` seconds kept for `retention` seconds, besides the raw
ring. `resolution` is 60 s .. 31 days and larger than the ring's `spacing`; no two archives of a
datapoint have the same resolution; `retention` is at least the resolution and at most 10 years
(3650 days), and `ceil(retention / resolution) + 1` (the slots of the archive) at most 2048. A
resolution of whole days is aligned to the local days of `settings.tz` (midnight to midnight, a DST
day has 23 or 25 hours), any other to multiples of the resolution in UTC. A slot consolidates the
samples of its interval: `last`, `min`, `max`, the number of samples, and the time-weighted mean
(each value counts for the time it was held, up to the ring's spacing plus 10 s after its sample; a
keep-alive sample counts like a new one); a boolean's slot also has its seconds on. Nothing is fed
while the value is not good or the wall clock is not synchronized: a slot without samples is empty,
not zero. Refusals at `datapoints[i].history.archives`: not an array, more than 4 (before an item is
read); at `...archives[k]`: not an object, a missing member, a resolution or retention out of its
range, a resolution not above the spacing, two archives of the same resolution, more than 2048
slots. Where they live and what they cost: 3.7 and `doc/persistence.md` "Archives".

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
engine's parameter store keeps the overrides) and without history; the rule engine owns it. Behind
a rule's parameter datapoints, a rule with states adds `rule/<ruleId>/state` (1.7, `spec/rules.md`
8.8): type `enumeration`, the 0-based position of the current state in the rule's `states`, not
writable (a command is refused), not readable, not persisted by the datapoint model, without
history; the rule engine owns it. So the order is per rule `enabled`, the parameters, `state`. Ids
starting with `rule/` are reserved: a declared one refuses the image. Functions, schedules and
the UI may name these datapoints like any other.

Implied datapoints of the schedules (`spec/schedules.md` 2.2, issue #58): behind the rules'
datapoints, the loader adds `schedule/<scheduleId>/time` for every entry of kind `weekly`,
`yearly` or `dates` whose `adjustable` names `time`, in schedule order: type `timeOfDay`, writable
(within the entry's bounds), transmitting, not persisted by the datapoint model and without
history; the scheduler owns it. Ids starting with `schedule/` are reserved: a declared one refuses
the image.

Implied datapoints of the blocks (`spec/blocks.md` 6, issue #60): behind the schedules' datapoints,
the loader adds for every block in block order `block/<id>/enabled`, `block/<id>/<parameter>` for
each adjustable parameter of its type, `block/<id>/<output>` for each output, and
`block/<id>/<input>` for each input bound to more than one datapoint (the aggregated value). The
enable switch and the parameters are writable (the switch only while `enabledAdjustable`), the
outputs and aggregated inputs are not; none is persisted by the datapoint model or has a history;
the block runtime owns them (technology `blocks`). Ids starting with `block/` are reserved: a
declared one refuses the image.

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
| `presses` | object kind of press → array of elements | for a `button` with an enumeration press (1.4) | Which elements of the press datapoint are which press, below. |
| `datapoints` | object role → datapoint id | yes | |

| Kind | Roles (**required**; *command roles need a writable datapoint*) |
|---|---|
| `light` | ***command*** (boolean), `state` (boolean) |
| `dimmer` | *command* (boolean, the main role), `state` (boolean), *levelCommand* (number), `level` (number), *step* (controlled: relative dimming, KNX DPT 3.007; the control bit is the direction, 1 brighter, the value the step code 1..7, 0 stops); **at least one of command and levelCommand** (1.6) |
| `blind` | ***move*** (boolean), *stop* (boolean), `position` (number), *positionCommand* (number), `slat` (number), *slatCommand* (number), `atTop` (boolean, 1.6), `atBottom` (boolean, 1.6) |
| `switch` | ***command*** (boolean), `state` (boolean) |
| `sensor` | **value** (any type) |
| `climate` | **temperature** (number), `setpoint` (number), *setpointCommand* (number), `mode` (enumeration or number), *modeCommand* (enumeration or number), `heating` (boolean or number, 1.6), `heatingLevel` (number, 1.6) |
| `scene` | ***command*** (number, enumeration or controlled) |
| `button` | **press** (boolean, controlled, or enumeration: a Zigbee remote's `action`, 1.4), `state` (boolean) |

"number" is `unsigned`, `signed` or `real`. Unknown roles are ignored (I5).

**The roles of 1.6** (haumacher/ehome #134, #136, #138):

- A **dimmer**'s main role is its switching `command` (the function is named after it); `levelCommand`
  and `step` are additional. A dimmer needs `command` or `levelCommand`, at least one (refused at
  `functions[i].datapoints`: `a dimmer needs the role command or levelCommand`). An image of 1.5 (a
  dimmer with `levelCommand`, with or without `command`) loads unchanged.
- A **blind** binds whatever its actuator reports: a percentage `position` (KNX DPT 5.001), the end
  positions `atTop` and `atBottom` (1 bit each, true while the hanging is at that end: an actuator's
  "end position top/bottom" reports, e.g. a shutter or awning without positioning), or both.
- A **climate**'s `heating` is a boolean or a number: a number is the actuating value in percent
  (DPT 5.001) and means heating while it is above 0. `heatingLevel` (a number) is the actuating
  value shown as such. One datapoint may take both roles.

**What the controller shows** (the device UI, `web/ui/dist/function_state.js`; MQTT in
`spec/mqtt.md` 4):

| Kind | Shown |
|---|---|
| `blind` with a known `position` | The position in percent. The end positions only cross-check it: an end reported while the position is more than 5 % away from it (or both ends at once) marks the state as contradictory ("end positions disagree"). |
| `blind` without a known position | From the end positions: open (`atTop` true), closed (`atBottom` true), in between (both false: stopped on the way); with only one of them bound its false value says "not fully open" or "not fully closed". Never a percentage. Both true: unknown, contradictory. |
| `climate` | The temperature; with `heatingLevel` its value in percent and as flames in three steps (0 %: none; up to a third: one; up to two thirds: two; above: three); without it "heating" while `heating` is true or above 0. The setpoint ("set to", with colder and warmer where `setpointCommand` is writable) only where the function has `setpoint` or `setpointCommand`. |
| `dimmer` | On or off by `state`, else by its level above 0, else by the last value of `command`; the level in percent where it has one. The icon switches `command` where it is writable, else `levelCommand` (0 or 100). |

**A Zigbee remote is a button** (1.4, haumacher/ehome #70). A remote reports its presses as the
enumeration datapoint of its `action` function (a client binding, 3.3 and `spec/zigbee.md`): every
command it sends is one update of the datapoint, also when the element is the same as before (two
presses of "on" are two updates), with the elements 0 off, 1 on, 2 toggle, 3 dim up, 4 dim down, 5
dim stop (`doc/zigbee.md`, the function table). Such a datapoint is the `press` of a `button`, and
the function says in `presses` which element is which kind of press:

| Kind | Meaning | The remote's elements E-Home writes |
|---|---|---|
| `press` | a press of a single or toggle button | 2 (toggle) |
| `on`, `off` | a press of the on or the off button | 1, 0 |
| `up`, `down` | a press of the up or the down (dim) button, short (a step) or held (a move); the remote does not tell the two apart | 3, 4 |
| `release` | the release of an up or down button after holding it | 5 |

```json
{"id": "hall-remote", "label": "Hall remote", "kind": "button", "room": "hall",
 "presses": {"press": [2], "on": [1], "off": [0], "up": [3], "down": [4], "release": [5]},
 "datapoints": {"press": "hall.remote"}}
```

Each kind is optional and names 1..6 elements; an element belongs to at most one kind; an element
no kind names is no press (the device UI shows its number). One remote is one function, not one
function per button: the device UI shows the last press with its kind ("On, 3 min ago", the press
datapoint's last update), which needs all of a remote's elements in one place, and a rule names the
datapoint anyway. `presses` is strict inside (I5): unknown members are refused, `description` and
`comment` allowed. Refusals at `functions[i].presses` (the paths and the beginning of the reasons):

| Path | Reason |
|---|---|
| `functions[i].datapoints.press` | `an enumeration press needs a Zigbee action datapoint (a client binding), whose elements are known; 'x' is not one` (a KNX or virtual enumeration: its elements are not known to the loader) |
| `functions[i].presses` | `a button with an enumeration press needs presses (which elements are which press)` |
| | `presses is only for an enumeration press; 'x' is boolean` (also controlled, or a number the role refused) |
| | `malformed: presses must be an object of press kinds (press, on, off, up, down, release) to arrays of elements` |
| | `malformed: unknown member 'longPress'` (with the hint of I4 when the image's minor version is newer) |
| | `presses names no press` (an empty object) |
| `functions[i].presses.on` | `malformed: on must be a non-empty array of elements` |
| `functions[i].presses.on[k]` | `malformed: an element must be an integer` |
| | `element 7 is no element of 'x' (a Zigbee action: 0..5)` |
| | `element 1 is a press of off already` |

A boolean or controlled press is unchanged: no `presses` (it is refused there), the UI's press
button commands it (`true`, `{"control": true, "value": 1}`). An enumeration press is never
commanded: a client binding is read-only (3.3).

**Rules on a remote** name the press datapoint, not the function: a trigger `{"kind": "datapoint",
"datapoint": "hall.remote", "on": "update"}` sees every press (`on: change`, the default, misses a
second press of the same button), and the condition compares the element (`@hall.remote == 1`;
spec/rules.md 3.1 compares enumerations with integers). A **long press** of a dimming remote is a
held up or down button: the remote sends a move (`up`, 3, or `down`, 4) when the button is held and
a stop (`release`, 5) when it is let go. A rule dims with two reactions: on an update to 3 or 4 it
starts the dimming (a relative `step` of a dimmer, `{"control": true, "value": 1}` brighter), on an
update to 5 it stops it (`{"control": false, "value": 0}`, step code 0). A rule that tells a long
press from a short one starts a timer (say 0.5 s) on the update to 3 or 4: the timer expiring before
the update to 5 is a long press (a transition to a state `held`), the update to 5 before it a short
one (spec/rules.md 7.8, `restartTimer`, a `timer` trigger, `from`/`to`).

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

`{"budget": bytes, "archiveBudget": bytes}`, both optional.

- `budget`: the RAM the history may use: the rings and the archives' open slots. Default: what they
  need, `Σ rings 8 × ring samples + Σ archives (ceil(3600 / resolution) + 2) × 32`, where ring
  samples is `ceil(retention / spacing) + 1` and an archive keeps its open slot and up to an hour of
  closed slots not yet written (32 bytes each; 96 bytes for a resolution of an hour or more). Cap: 64
  KB on the ESP32-C6 (`HistoryLimits::cap_bytes`, 65 536).
- `archiveBudget` (1.2): the flash the archives may use: `Σ archives ceil(slots / 127) × 4096`, with
  slots `ceil(retention / resolution) + 1`: an archive is stored in segments of 127 slots of 32
  bytes and a header, one 4 KB flash block each. Default: what the archives need. Cap: 64 KB on the
  ESP32-C6 (`HistoryLimits::archive_cap_bytes`, 65 536: 16 blocks of the 40 free with a typical
  image, `doc/persistence.md` "Archives").

Refused at `history` when the rings and archives need more than `budget`, or the archives more than
`archiveBudget`, or, where a budget is not given (with or without the member `history`), more than the
device's cap; at `history.budget` or `history.archiveBudget` when it exceeds the device's cap
(`refuse-history-ram-cap.json`).
Example: a temperature with a ring of 2 days at 300 s (577 samples), an hourly archive of 31 days
(745 slots, 6 segments) and a daily one of 2 years (731 slots, 6 segments), and a boolean with a
ring of a day at 900 s (97 samples) and a daily archive of a year (366 slots, 3 segments): `budget`
577 × 8 + 96 + 96 + 97 × 8 + 96 = 5680, `archiveBudget` 15 × 4096 = 61 440
(`spec/fixtures/images/cases/accept-history-archives.json`).

### 3.8 `external`

Optional. The HTTP JSON sources (weather and other services) that feed datapoints, in the shape of
`spec/external-values.md` (`{"sources": [...]}`): sources with a `url` or a `preset`, an interval and
bindings from JSON paths or preset variables to datapoints of this image. It is validated and applied
after the datapoint model it binds to; a refusal names the path under `external` and the image is not
applied.

### 3.9 `ui`

E-Home's presentation: the **control pages** (issue #54, E-Home's side haumacher/ehome#72), the
only member so far. Without `ui` or `pages`, or with an empty list, the device UI shows its
generated overview of rooms and functions. Unknown members are ignored (I5). A 1.0 firmware checks
only that `ui` is an object and keeps its overview, so an image with pages loads there too (I4). Compiled, the pages are the section
`ui` (9.16): the finished body of `GET /api/pages`, which the device serves unchanged.

```json
"ui": {"pages": [
  {"id": "evening", "label": {"de": "Abend", "en": "Evening"}, "icon": "moon",
   "sections": [
     {"label": {"de": "Wohnzimmer"}, "elements": [
       {"function": "living-light", "label": {"de": "Decke"}},
       {"function": "evening"},
       {"datapoint": "1/3/1", "readOnly": true},
       {"datapoint": "rule/heat/setpoint"},
       {"datapoint": "rule/presence/enabled", "label": {"de": "Anwesenheit"}}]},
     {"elements": [
       {"chart": {"label": {"de": "Temperaturen"}, "width": "full", "height": 4,
         "range": "24h", "presets": ["24h", "7d", "30d", "1y"],
         "axes": {"left": {"min": null, "max": null}, "right": {"min": 0, "max": 100}},
         "series": [{"datapoint": "1/3/1", "label": {"de": "Wohnzimmer"}, "axis": "left", "consolidation": "mean"},
                    {"datapoint": "1/3/5", "axis": "right", "consolidation": "mean"},
                    {"datapoint": "1/1/7", "label": {"de": "Heizung"}}],
         "references": [{"value": 21, "axis": "left", "label": {"de": "Soll"}},
                        {"datapoint": "rule/heat/setpoint", "axis": "left"}]}}]}]}]}
```

Complete example: `spec/fixtures/images/pages.json`. One global set of pages, in navigation order;
the first is the start page. There is no free positioning: a section is a grid of tiles in element
order. E-Home expands a room into its function elements before export; an element may appear on
several pages.

A **page**:

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | 1..32 characters of `A-Z a-z 0-9 _ . -`, unique among the pages | yes | The page's address in the UI (`/page/<id>`). |
| `label` | label | no | The navigation entry and the title (default: the id). |
| `icon` | 1..31 characters of `A-Z a-z 0-9 _ . -` | no | A name of the UI's icon set (`moon`, `sun`, `home`, `bulb`, `chart`, `energy`, `thermometer`, `blind`, `power`, `play`, `calendar`, ...); an unknown name shows the default icon. |
| `minimumRole` | `"anonymous"` or `"operator"` | no, `"operator"` | The lowest role that sees and operates the page (schema 1.8, I12): `"anonymous"` without sign-in, `"operator"` signed in (an administrator sees every page). |
| `sections` | array of sections | no (none) | |

**Anonymous pages** (schema 1.8, I12). A page with `"minimumRole": "anonymous"` is shown and operated
without sign-in. Everything such a page shows may be used, of whatever kind: the compiler derives
from the anonymous pages, in their order,

- the body of `GET /api/pages` for `anonymous`: the five lists of 9.16 built from the anonymous
  pages alone (their sections, elements, series and reference lines, the indices counted within
  this body), byte for byte what the same compiler writes as `ui` for a source whose `ui.pages` are
  just those pages;
- the **datapoints** they show, each once, with whether it is **operated**: a datapoint element's
  datapoint (operated unless `readOnly` is true), every datapoint of a function element's function
  (all its roles, operated: the tile is the function's control), a chart series' and a reference
  line's datapoint (read only). A datapoint shown both ways is operated. A rule parameter is shown
  by its datapoint (`rule/<ruleId>/<parameterId>`, `rule/<ruleId>/enabled`), so the same entry says
  whether it may be set;
- the **functions** of their function elements, each once.

Without a page of minimum role `anonymous` none of this is written, and the device lets nobody in
without sign-in (`spec/api.md` "Authentication"). The member is refused when it is not one of the
two values (`ui.pages[0].minimumRole`).

A **section**: `label` (label, optional: the section's title; none, no title) and `elements` (array,
optional). An **element** has exactly one of `function`, `datapoint` or `chart`, and an optional
`label` that replaces the target's own:

| Element | Members | Meaning |
|---|---|---|
| function | `function`: a function id (3.5) | The function's tile by its kind, with its detail sheet. |
| datapoint | `datapoint`: a datapoint id (3.3, including the implied `rule/<ruleId>/<parameterId>`, `rule/<ruleId>/enabled` and `rule/<ruleId>/state`); `readOnly`: boolean, default false | A value tile; the control is chosen by the value type (doc/web.md "Control pages"), and shown only when the datapoint is writable and `readOnly` is not true. `readOnly: false` cannot make a datapoint writable: it stays read-only. `readOnly` on another element is refused. |
| chart | `chart`: object, below | A history chart over the tiles' grid. |

The 1.0 draft's `chart: true` flag of a datapoint element was replaced by the chart element: an
element with `datapoint` and `chart` names two kinds and is refused.

A **chart**:

| Member | Type | Required | Meaning |
|---|---|---|---|
| `label` | label | no | The title; the element's `label`, when it has one, replaces it. |
| `width` | 1, 2, 3, 4 or `"full"` | no, `"full"` | Columns of the section's grid, capped at the columns a narrow screen has. |
| `height` | 2 or 4 | no, 2 | Tile rows of 64 px: 2 hold a title and one or two series, 4 suit many series with the legend. |
| `range` | `"1h"`, `"24h"`, `"7d"`, `"30d"`, `"1y"` (a year is 365 days) | no, `"24h"` | The range shown first. |
| `presets` | array of ranges, each once, including `range` | no, `[range]` | The ranges the viewer switches between; the viewer also steps back and forth in time by one range. |
| `axes` | `{"left": {"min", "max"}, "right": {"min", "max"}}`, each bound a number (−10⁹..10⁹) or `null` | no, automatic | A fixed bound per axis; `null` or absent is automatic. `min` < `max` when both are fixed. |
| `series` | array of series, may be empty (an empty chart) | yes | In this order, coloured by a fixed palette of 12 in series order (no per-series colour). |
| `references` | array of reference lines | no | Horizontal lines that cost no history. |

A **series**: `datapoint` (required: a datapoint of the image **with a history ring**, 3.3),
`label` (label; default the datapoint's), `axis` (`"left"`, `"right"`; default `"left"`; ignored for
a boolean) and `consolidation`:

- `mean`: the line through each bucket's mean; the default for a `real` and a `boolean`;
- `last`: the line through each bucket's last value; the default for `unsigned` and `signed`;
- `band`: the mean line with the bucket's min/max band (a band only on a chart of a single series;
  with several series it is drawn as its mean line);
- `consumption`: bars of the delta between consecutive buckets' last values, for meter readings
  (refused for a boolean); a negative delta is a counter reset and is drawn as a gap.

A **boolean** series is drawn as an on/off strip beneath the plot, without an axis; a consolidated
bucket shows its on-share (with `mean`) or its last state (with `last`). A legend is shown with more
than one series; tapping an entry hides or shows the series.

A **reference line**: exactly one of `value` (a number: a constant such as 21 °C or a limit) or
`datapoint` (a numeric datapoint, `unsigned`, `signed` or `real`: its current value, drawn across the
whole chart and following the live value), plus `label` (label) and `axis` (default `"left"`).

**Which history answers.** A chart queries the history of its series (spec/api.md "history") over its
range in buckets of `range / 48` (bars: their width). Of the datapoint's stores (the raw ring and
its archives, 3.3) whose resolution is at most the bucket width (the ring's is its spacing), the
finest that reaches back to the range's start answers; if none does, the one that reaches furthest;
if no archive is fine enough, the raw ring. A range longer than what the answering store keeps shows
what it has, with the hint "only the last ... stored", never an error; the response's `store` names
the store (`"raw"` or the archive's resolution in seconds). E-Home derives the archives from the
charts' ranges (ehome#73).

Refusals, each with its path (`ui.pages[0].sections[1].elements[0].chart.series[2].consolidation`):
`ui` or `pages` of the wrong type; more than the limits of section 5 (at `ui.pages`, before anything
else is checked); a page without `id`, a malformed or duplicate id, a malformed icon; a page, section,
element, chart, series or line that is not an object; an element with none or several of `function`,
`datapoint`, `chart`; `readOnly` outside a datapoint element; an unknown function or datapoint; a
malformed label; a width, height, range, preset, consolidation or axis out of its set; presets that
are empty, list a range twice or do not offer `range`; an axis with `min` >= `max`; a chart without
series; a series datapoint without history; `consumption` on a boolean; a reference line with none
or both of `value` and `datapoint`, or a datapoint that is not numeric.

### 3.10 `mqtt`

Optional. The MQTT broker the controller publishes to and the shape of what it publishes
(`spec/mqtt.md`): `broker` (`mqtt://host[:port]`, `mqtts://`), `username`, `baseTopic`, `retain`,
`discovery` (Home Assistant), `history` (the export of the rings). The password is never in the
image (the device setting `mqtt.password`); an image with `mqtt.password` is refused. The member is
validated with the image and applied after the datapoint model and the functions it names; a
refusal names the path under `mqtt`. Without the member the controller does not talk MQTT.

## 4. Refusals

**Of the source form (E-Home).** E-Home validates the source form before it compiles it and refuses
it with the same paths and reasons the device used while it read JSON: each refusal has a **path**
(`datapoints[3].knx.send`, `functions[1].datapoints.command`,
`rules[0].reactions[1].actions[0].datapoint`; `""` for the document as a whole) and an English
**reason**. Rules are validated after everything else and only when everything before is valid,
because their references are checked against the image's datapoints and schedules; the blocks come
last (`spec/blocks.md` 11, `blocks[2].inputs.temperature`). Rule, schedule and block reasons start
with the reason codes of `spec/rules.md` 10.1 (`malformed:`, `outputConflict:`,
`unknownReference:`, ...; blocks add `unknownType:` and `missingInput:`). The corpus cases
`spec/fixtures/images/cases/refuse-*.json` are E-Home's: its compiler refuses each with the case's
`expect`. An image that does not validate is never compiled nor sent.

**Of the binary image (the device).** Each refusal has a path into the binary image
(`header.crc`, `sections[4].offset`, `datapoints[3].owner`, `nodes[17].argc`; section 10) and an
English reason; E-Home maps a record path back to the source path where it can (it knows which source
member it compiled into which record) and shows both. Kinds, as `spec/api.md` reports them:

| Kind | Code | HTTP | When | The running image |
|---|---|---|---|---|
| too large | `image.too_large` | 413 | The announced body is larger than the slot. | untouched |
| not an image | `image.format` | 400 | The header is not that of a binary image: shorter than 32 bytes, another magic (a JSON text is named as such), a header size or length that does not fit the body (10.1). | untouched |
| version | `image.version` | 422 | Another major format version. Nothing else is checked. | untouched |
| unsigned | `image.unsigned` | 403 | The device holds the installation key and the header says the image is unsigned (`spec/api.md` "Signed runtime images"). | untouched |
| damaged | `image.damaged` | 422 | The CRC-32 does not match: damaged on the way or in the slot. | gone |
| bad signature | `image.bad_signature` | 403 | The signature does not verify under the installation key. | gone |
| invalid | `image.invalid` | 422 | A check of sections 10.3 to 10.8. | gone |
| no memory | `image.no_memory` | 507 | The device could not allocate the running state (values, rule and block runtime, history rings). | gone |
| apply | `image.apply_failed` | 500 | Valid, but a component refused while it was applied (section 6). | gone |

"Untouched": refused from the header alone, before the slot is erased (10.1). "Gone": the device has
no image afterwards (I9). Up to 12 refusals are reported with path and reason, up to 100 counted;
the checks of 10.1 to 10.4 stop at their first refusal (the later ones would read through what is
wrong), those of 10.5 to 10.8 continue with the next record.

## 5. Limits and RAM

| | Limit |
|---|---|
| Binary image | the slot: 128 KB on 4 MB flash, 512 KB on 8 and 16 MB (section 7); the POSIX build 512 KB |
| Source text | none on the device; E-Home's own (the source never reaches the device) |
| Nesting of the source | 48 levels (rule expressions nest deeply), checked by E-Home |
| Datapoints / locations / functions | 1024 declared (the source's `datapoints`; the implied ones of 3.3 and 3.6 come in addition, bounded by the limits of the rules, schedules and blocks) / 256 / 512 |
| Roles of a function | 16 |
| Control pages (`ui.pages`, over all pages) | 16 pages, 128 sections, 512 elements, 256 chart series, 128 reference lines; checked by E-Home (the device does not read the pages, I10) |
| Schedule entries | 128, with at most 128 dates each (`spec/schedules.md`) |
| Rules | the limits of `spec/rules.md` 10.2 |
| Blocks | 32 (`spec/blocks.md` 11) |
| Listening addresses per datapoint | 8 |
| Zigbee bindings | 512 |
| External sources / bindings | 8 / 32 (`spec/external-values.md`) |
| History | per datapoint a ring of up to 2048 samples and 4 archives of up to 2048 slots; RAM `history.budget` and flash `history.archiveBudget` at most 64 KB each (3.7) |
| A string of the binary image | 511 bytes (the longest member, an external source's URL, has 383) |
| Languages of a label | 16 |
| Reported refusals | 12 with path and reason; up to 100 counted |

The device checks these limits on the binary image (section 10.8), because its running state is
sized by them; E-Home checks them on the source first.

RAM: the device keeps no copy of the image. While a new image arrives it holds the table of the
values it carries across the swap (section 12) and a buffer of the transport (a few KB); while it
validates, no more than a few hundred bytes of counters. What stays is the running state: per
datapoint its value, quality and update time, the rule runtime (timers, states, match memories,
change tracking), the block runtime (state records), the history rings and the archives' open slots
(the budget), the KNX adapter's queue. A table the runtime needs that the image does not carry (the
rules' tracked datapoints and their match and silence triggers, `spec/rules.md` 7.4, 7.7, 7.11) is
derived once at load. The figures before and after issue #87 are recorded in the issue (its
acceptance).

## 6. Applying an image

On `PUT /api/image` (and the console's `image load`, the POSIX `--image`) the event loop goes
through these steps. At boot it checks the header of the image in the slot (step 1) and goes on
with step 4; there is nothing to keep (no table of section 12) and nothing to write.

1. **The header** (the first 32 bytes) is checked (10.1). A refusal here leaves the running image
   untouched.
2. **What the swap keeps is taken out of the slot's reach:** the rules' queued events are processed
   with the old rules (`Engine::flush`); the history saves its rings and the value store writes the
   persisted values (both by datapoint name, `doc/persistence.md`); the device takes the table of
   section 12 (values, rule and block runtime, by id hash). Then the components stop and the old
   image is unmapped: from here the device runs without an image.
3. **The slot is written:** each 4 KB sector the image needs is erased just before the bytes for it
   are written, as they arrive; nothing is held in RAM beyond the transport's buffer. A body that
   ends early or a write that fails leaves no image (`image.format`, the reason naming the bytes
   received, or the flash error).
4. **The slot is mapped** into the address space.
5. **Checked:** the CRC-32, the signature, then the structure (section 10). A refusal leaves no
   image.
6. **The datapoint model** is built from the image's datapoints: a datapoint whose id hash and type
   are in the table of section 12 takes value, quality and update time from it; the others start
   unknown.
7. The settings are written where they differ (no flash write for an unchanged image).
8. The value store and the history are loaded for the new model: persisted values are restored where
   the model has no kept value; the history restores its rings by datapoint name into the new rings
   (`doc/persistence.md`).
9. The rules are loaded; each rule's runtime (timers, states, change tracking, diagnostics) follows
   its id through the table of section 12, its parameter overrides and timeline override through the
   parameter store (`spec/rules.md` 7.6, 8.4, 8.7). Then the blocks: state records follow the block
   id through the table, overrides through the store (`spec/blocks.md` 8.1); the mirrors are bound.
10. The KNX adapter gets the image's group address table (the time master's addresses are in it);
    then the time master. Read-on-start datapoints are read again.
11. The schedules are loaded.
12. The rules start with reason `boot` (the image at a restart) or `image` (a swap), then the blocks
    (`spec/blocks.md` 5.2).

A failure in steps 6 to 12 (out of memory, a component refusing what the validation accepted) tears
the new configuration down; the device has no image (I9) and reports `image.no_memory` or
`image.apply_failed` with the component's reason. The table of section 12 is freed when an image
has been applied (step 12), otherwise kept until the next image is applied, so that the deployment
E-Home sends again after a failure finds the values (section 12).

## 7. The slot

**The partition.** The image lives in its own flash partition `image` (type `data`, subtype `0x40`,
found by its name), one image at a time, mapped through the cache (`esp_partition_mmap`). No other
data is in it, and nothing of the image is in the storage partition any more: no LittleFS chunks
`image.<g>.<n>`, no manifest `image.idx`, no staging area behind the updater (`EspImageStaging` and
the reservation behind the updater's binary go). The partition tables
(`platform/esp/partitions-<size>.csv`, generated by `platform/esp/tools/gen_partitions.py`):

| Flash | `app` | `image` | `storage` | Reason |
|---|---|---|---|---|
| 4 MB | 2 688 KB at 0xF0000 (was 2 752 KB at 0x100000; the updater gives 64 KB, 960 KB -> 896 KB, since its staging area is gone) | 128 KB at 0x390000 (0x020000), between `app` and `otadata` | 204 KB, unchanged | The firmware is 2 475 392 bytes: 10 % of `app` stay free. The storage keeps the history rings and archives (up to 64 + 64 KB), the language pack, the values and the overrides. |
| 8 MB | 4 160 KB at 0xF0000 (the updater's 64 KB) | 512 KB at 0x500000 (0x080000), directly behind `app`; `otadata` moves to 0x580000 | 2 444 KB at 0x582000 (was 2 956 KB) | The storage is large on these boards; the application partition keeps its room and takes the updater's 64 KB. |
| 16 MB | 6 208 KB at 0xF0000 (the updater's 64 KB) | 512 KB at 0x700000 (0x080000), directly behind `app`; `otadata` moves to 0x780000 | 8 588 KB at 0x782000 (was 9 100 KB) | As on 8 MB. |

Every partition start is a multiple of 64 KB (the MMU's page), so the slot maps from its first byte.
A new partition table is flashed once over USB on every controller; nothing is migrated (the owner:
"alle Controller können neu geflasht und neu programmiert werden"). The images of the old JSON form
in the storage partition are removed at the first start of the new firmware.

**The POSIX build** keeps the slot in the file `image.bin` of its data directory, 512 KB at most,
read into one heap block at start and after a write (the platform interface stands for the mapping).

**The slot size** is the partition's size (the POSIX build: 524 288). It is the limit of
`PUT /api/image` (413 beyond it, before a byte is read) and is reported as `image.slot` in
`GET /api/status` and as `slot` in the E-Home connection's `auth` (`spec/api.md`). E-Home refuses a
larger image before sending it.

**Writing and power.** The image is written as it arrives (section 6 step 3). A power cut while it is
written leaves a slot whose CRC-32 or length does not match: at the next start the device has no
image and reports `image.damaged` or `image.format`; E-Home sends the last valid deployment again.

**At boot** the device validates the image in the slot as on receipt, except the signature and the
check for an unsigned image (section 10; I11: only the verified receive path writes the slot, so an
image signed with a key since rotated, or unsigned before a key was stored, keeps loading). A slot
that fails the CRC or the structure leaves no image. An erased slot (the magic missing, all bytes
`FF`) is "no image", not a refusal.

`DELETE /api/image` (and the console's `image clear`, and the factory reset) unloads the image and
erases the sectors it uses, with the persisted values and history rings: the device returns to setup.
`GET /api/image` returns the bytes of the loaded image (`[0, length)`, the signature included); while
no image is loaded it answers 404 `image.none`, also when the slot holds bytes that did not validate.

## 8. Changes

- 1.0 (issue #8): first version.
- 1.1 (issue #54): `ui.pages`, the control pages (3.9), with their limits (5). An image of 1.0 loads
  unchanged; a 1.0 firmware loads an image of 1.1 and ignores its pages (I4, I5).
- 1.2 (issues #58 and #56): the schedule members `adjustable` and `bounds` (`spec/schedules.md` 2.1)
  and the implied datapoints `schedule/<id>/time` (3.3), the prefix `schedule/` reserved; and
  `datapoints[].history.archives` with `history.archiveBudget`, the consolidated archives of
  homecontrol#56 (3.3, 3.7; corpus cases `accept-history-archives`, `refuse-history-archives`,
  `refuse-history-archive-budget`); `history.budget` now also counts the archives' open slots in
  RAM. E-Home writes 1.2. An image
  of 1.0 or 1.1 loads unchanged. A 1.1 firmware ignores `history.archives` (I5), but refuses
  `adjustable` and `bounds` (schedule members are strict, I5): an image that uses them needs a 1.2
  firmware, one without them loads on 1.1 too. From 1.2 on such a strict refusal of a newer minor
  names the versions (I4). The exact form of the schedule members, which E-Home exports
  (haumacher/ehome#74):
  `"adjustable": ["time", "days"]`, an array of member names of the entry's kind, each once, and
  `"bounds": {"<member>": {"min": v, "max": v, "step": n}}`, every part optional, where `v` has the
  member's own form (a time `{"hour", "minute", "second"}`, an `offset` or `every` in milliseconds,
  an `at` as a date and time) and `step` is in seconds for the times (1..86400), in milliseconds
  for `offset` (whole seconds) and `every` (whole minutes), and absent for `at`; `days` has no
  bounds.
- 1.3 (issue #60): the member `blocks`, the virtual devices of `spec/blocks.md` (the first type the
  room controller `org.homecontrol.room-controller` 1.0), strict inside (I5); the implied datapoints
  `block/<id>/<x>` (3.3), the prefix `block/` reserved; the limit of 32 blocks (5). E-Home writes 1.3.
  An image of 1.0 to 1.2 loads unchanged. A firmware that reads only up to 1.2 would ignore `blocks`
  (I5) and run none: E-Home deploys an image with blocks only to a controller that reports 1.3 or
  newer (I4, `spec/blocks.md` B1). The corpus is `spec/fixtures/blocks/`.
- 1.4 (haumacher/ehome #70): the Zigbee remotes as buttons: the role `press` of a `button` also
  takes an enumeration, a Zigbee action (a client binding), with the function's member `presses`
  (3.5), strict inside (I5); the API's model passes `presses` on (`spec/api.md`), the device UI
  shows the last press by its kind, MQTT announces such a button without `state` as a `sensor`
  (`spec/mqtt.md`). Corpus cases `accept-button-presses`, `refuse-button-presses`. E-Home writes
  1.4. An image of 1.0 to 1.3 loads unchanged. A firmware that reads only up to 1.3 refuses an
  enumeration press (`datapoint 'x' is enumeration, but the role press of a button takes boolean or
  controlled`), so the whole image: E-Home deploys an image with an enumeration press only to a
  controller that reports 1.4 or newer (I4), as it does with blocks for 1.3; an image without one
  loads on 1.3 as before (`presses` elsewhere is an unknown member, ignored).
- 1.5 (issue #81, haumacher/ehome #110): a rule parameter's id has the characters of a rule id,
  1..24 of `A-Z a-z 0-9 _ -`, not a keyword, not `enabled` (`spec/rules.md` 2.1); the text writes
  one that is not an identifier `$"max-helligkeit"` (4.3, 4.8), the JSON AST is unchanged
  (`{"kind": "parameter", "id": "max-helligkeit"}`), its datapoint is
  `rule/<ruleId>/max-helligkeit` and its persistence key `<ruleId>/max-helligkeit` (8.4, 8.6).
  Corpus cases `spec/fixtures/rules/` 65 to 71, image cases `accept-parameter-ids`,
  `refuse-parameter-id`. E-Home writes 1.5. An image of 1.0 to 1.4 loads unchanged. A firmware
  that reads only up to 1.4 refuses an image with such a parameter id (`rule 'r': malformed:
  parameter id 'max-helligkeit' is not an identifier`, without the hint of I4, which that firmware
  adds only to an unknown member), so the whole image: E-Home deploys an image with a parameter id
  that is not an identifier only to a controller that reports 1.5 or newer (I4), as with the
  enumeration press for 1.4; an image whose parameter ids are all identifiers loads on 1.4 as
  before.
- 1.6 (haumacher/ehome #134, #136, #138): a dimmer's main role is its switching `command`; it needs
  `command` or `levelCommand`, at least one (before: `levelCommand` required). The blind's roles
  `atTop` and `atBottom` (1 bit each, the end positions), the climate's `heatingLevel` (a number,
  the actuating value) and its `heating` of a number (heating while above 0), with what the
  controller shows of them (3.5). A rule writes a value of the application (`virtual`, or without
  a technology member) also when it is not `writable`, which is the user's permission (3.3,
  `spec/rules.md` 7.3 and decision 16); the API's datapoint flags have the bit 0x40 for such a
  value (`spec/api.md` "Datapoints"). Corpus cases `accept-function-roles-1-6`,
  `refuse-function-roles-1-6`, `refuse-rules` (now a KNX status datapoint), rule cases 72 and 73.
  E-Home writes 1.6. An image of 1.0 to 1.5 loads unchanged. A firmware that reads only up to 1.5
  ignores `atTop`, `atBottom` and `heatingLevel` (unknown roles, I5: the blind shows no end
  positions, the climate no heating level) and loads the image; it refuses a dimmer without
  `levelCommand` (`a dimmer needs the role levelCommand`), a `heating` of a number (`... the role
  heating of a climate takes boolean`) and a rule writing an application datapoint that is not
  writable (`notWritable`), so the whole image: E-Home deploys an image with one of these three only
  to a controller that reports 1.6 or newer (I4), as with the parameter ids for 1.5; an image
  without them (a dimmer with `levelCommand`, a boolean `heating`, rules writing only writable
  datapoints) loads on 1.5 as before.
- 1.7 (issue #84, haumacher/ehome #140): time programs. A rule with states may carry a `timeline`
  (`spec/rules.md` 2.8: day plans of switching points, a clock time or a sun event with `offset`,
  `notBefore`, `notAfter`, and a state; assignments of plans to days, the default and `days`;
  `adjustable`), and a state may carry a `label`; a rule with a timeline needs no reaction. The
  semantics (`spec/rules.md` 7.13): a switching point is a transition, every start, synchronization
  or step of the clock and change of the timeline enters the state due now with its entry actions,
  a reaction with `to` overrides until the next point. The inhabitant replaces an adjustable
  timeline on the device (`spec/rules.md` 8.7; `PUT`/`DELETE /api/rules/<ruleId>/timeline`,
  `spec/api.md`), kept by a new image while every state it names exists. Every rule with states
  has the implied datapoint `rule/<ruleId>/state` (3.3, `spec/rules.md` 8.8), an enumeration of the
  state's position, read-only, with the states' labels as its choices in the API; a parameter
  `state` of a rule with states is refused. The limits of `spec/rules.md` 10.2 (8 plans, 16 points
  per plan, 64 per timeline, 256 points and 64 plans per image). Corpus cases
  `spec/fixtures/rules/` 74 to 100, the image `heating-timeline.json` (E-Home's export of the
  heating program equals it) and the image case `refuse-rule-timeline`. E-Home writes 1.7. An image
  of 1.0 to 1.6 loads unchanged; its rules with states get the state datapoint, which no image
  could declare (the prefix `rule/` is reserved), so nothing it says changes meaning. A parameter
  `state` of a rule with states is refused in an image of 1.7 or newer; in an older one, which may
  declare it, that parameter keeps its datapoint and the rule gets no state datapoint (logged at
  load), so a stored image keeps loading after the firmware update. A firmware that reads only up to 1.6 refuses a rule with `timeline` or a
  state with `label` (`rule 'heating': malformed: unknown member 'timeline' (the image's schema 1.7
  is newer than this firmware reads, up to 1.6: update the firmware)`), so the whole image: E-Home
  deploys an image with either only to a controller that reports 1.7 or newer (I4), as for 1.6; an
  image without them loads on 1.6 as before (without the state datapoints, which E-Home's pages
  and rules may then not name).
- **Format 1.0** (issue #87, haumacher/ehome#176): the binary image (sections 9 to 13), the only form
  the device reads; the source form stays at schema 1.7 and is E-Home's (I1, I4). The device reports
  `format` and the slot size instead of `schema` and `image.storable` (`spec/api.md`); the version
  gates of 1.3 to 1.7 above concerned firmware that read JSON and no longer apply: every controller
  is flashed with a firmware that reads format 1.0, which carries everything of schema 1.7. The
  history of the JSON on the device (the paragraphs above that speak of a firmware refusing or
  ignoring a member) describes firmware up to 1.2.
- 1.8 and **format 1.1** (homecontrol #91, haumacher/ehome#184): the page member `minimumRole`
  (3.9, I12) and the three optional sections `ui.anonymous`, `ui.anonymous.datapoints` and
  `ui.anonymous.functions` (9.16) that the compiler derives from it. E-Home writes the header's
  minor 1 for an image with these sections and minor 0 for one without them (an image without an
  anonymous page is the format 1.0 image it was). A firmware that reads format 1.0 skips the three
  sections (they are not `required`, section 11) and lets nobody in without sign-in; so does a
  firmware of 1.1 with an image without them. E-Home writes them only for a controller that reports
  format 1.1 or newer (I4), and says otherwise that the anonymous pages need a firmware update.
  Corpus: the image `anonymous-pages.json` (13.1).

## 9. The binary image

The device's contract (issue #87): what E-Home's compiler writes and the device reads in place. The
section says what each byte means; section 10 what the device checks; section 13 shows a complete
image byte by byte.

### 9.1 Conventions

- **Byte order** little-endian (the ESP32-C6 and the hosts). `u8`, `u16`, `u32`, `u64` are unsigned,
  `i32`, `i64` two's complement, `f64` IEEE 754 binary64.
- **Alignment.** The ESP32-C6 reads the slot through its cache with ordinary loads, and the firmware
  reads a record as a C++ struct of exactly the layout given here. So every section starts at an
  offset that is a multiple of 8; a record with a 64-bit field has a size that is a multiple of 8 and
  the field at a multiple of 8 within it; a record with 32-bit fields a size that is a multiple of 4
  and those fields at multiples of 4; 16-bit fields lie at even offsets. The layouts below have no
  implicit padding: every byte is named, the unnamed ones `reserved`.
- **Reserved** fields and the padding between sections are written 0 and not read by the device (a
  later minor version may give them a meaning whose zero is today's, section 11).
- **Booleans** are `u8` 0 or 1; flags are bits of a `u8` or `u16` named per field.
- **Indices.** A reference to a record is its 0-based index in its table, `u16` (`u8` where noted);
  `kNone` = `0xFFFF` (`u8`: `0xFF`) means "none". Indices of a rule's own parameters, timers and
  states are local to the rule (0 is the rule's first); every other index is global to its table.
- **A run** is a `first` index with a `count`: the records `first .. first + count − 1` of a table.
  A count of 0 is an empty run whatever its `first`.
- **A string** is referenced by a `u32` byte offset into the section `strings` (9.5); offset 0 is the
  empty string, which stands for "none" where a member is optional.
- **A label** (a string, or a map of language tags to strings; `spec/rules.md` 2.2) is referenced by a
  `u32` *label reference* into the section `labels` (9.5); 0 is "no label".
- **Group addresses** are their 16-bit value (`main << 11 | middle << 8 | sub`, the free form
  `main/sub` alike); the records reference them by their index in the address table (9.10).
- **Value types and expression types** are the codes of 9.14.

### 9.2 Header

The image starts with a header of 32 bytes; the section table follows it.

| Offset | Type | Field | Value |
|---|---|---|---|
| 0 | `u8[4]` | `magic` | `48 43 49 4D` ("HCIM") |
| 4 | `u16` | `major` | The format's major version: 1. |
| 6 | `u16` | `minor` | The format's minor version the image is written in: 0 (format 1.0), 1 for an image with a section of format 1.1 (section 8). |
| 8 | `u32` | `length` | The image's size in bytes, the signature included; a multiple of 8. |
| 12 | `u32` | `crc` | CRC-32 (the IEEE polynomial as zlib's `crc32()`: reflected `0xEDB88320`, initial and final XOR `0xFFFFFFFF`) over the bytes `[0, signedEnd)` with this field taken as 0. |
| 16 | `u32` | `flags` | Bit 0 `signed`: the image ends with a 64-byte Ed25519 signature (9.17). Other bits: none in 1.0. |
| 20 | `u16` | `headerSize` | The bytes of the header, 32 in 1.0; the section table starts here (a later minor may lengthen the header). A multiple of 8. |
| 22 | `u16` | `sectionCount` | The entries of the section table. |
| 24 | `u32` | reserved | |
| 28 | `u32` | reserved | |

`signedEnd` is `length − 64` for a signed image, `length` otherwise: the bytes the CRC and the
signature cover. The CRC detects damage on the way and in the flash also where no installation key
is held; the signature proves the origin (9.17).

### 9.3 The section table

`sectionCount` entries of 16 bytes at `headerSize`, one per section, in ascending order of their ids
and of their offsets:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u16` | `id` | The section (9.4). |
| 2 | `u16` | `flags` | Bit 0 `required`: a firmware that does not know `id` refuses the image (section 11). Other bits: none in 1.0. |
| 4 | `u32` | `offset` | From the image's first byte; a multiple of 8, not before the end of the section table. |
| 8 | `u32` | `length` | Bytes; `offset + length` not beyond `signedEnd` and not beyond the next section's `offset`. |
| 12 | `u16` | `entrySize` | The size of a record of a table section, at least the size this document gives for it and a multiple of its alignment (a later minor may lengthen records, section 11); 0 for a byte section (`strings`, `ui`). |
| 14 | `u16` | reserved | |

A table section holds `count = length / entrySize` records (`length` a multiple of `entrySize`);
record `i` starts at `offset + i × entrySize`, and the device reads the fields this document gives
for it from that start. A section that is absent is an empty table (I6); E-Home writes no empty
table section.

### 9.4 The sections

| Id | Name (paths, 10.2) | Record | Size | Source (section) |
|---|---|---|---|---|
| `0x01` | `image` | exactly one | 32 | `image`, `history` budgets (3.1, 3.7) |
| `0x02` | `strings` | bytes | — | every string (9.5) |
| `0x03` | `labels` | label entry | 8 | every label (9.5) |
| `0x04` | `settings` | exactly one | 32 | `settings` (3.2) |
| `0x10` | `datapoints` | datapoint | 24 | `datapoints` and the implied ones (3.3, 3.6) |
| `0x11` | `locations` | location | 12 | `locations` (3.4) |
| `0x12` | `functions` | function | 28 | `functions` (3.5) |
| `0x13` | `roles` | role | 8 | `functions[].datapoints` |
| `0x14` | `history` | ring | 16 | `datapoints[].history` (3.3) |
| `0x15` | `archives` | archive | 8 | `datapoints[].history.archives` |
| `0x20` | `knx.addresses` | group address | 8 | every address of `knx` bindings and the time master |
| `0x21` | `knx.bindings` | KNX binding | 12 | `datapoints[].knx`, the time master's datapoints |
| `0x22` | `knx.listen` | address index | 2 | `datapoints[].knx.listen` |
| `0x23` | `knx.listeners` | binding index | 2 | the bindings per address |
| `0x24` | `knx.timeMaster` | exactly one | 12 | `knx.timeMaster` (3.6) |
| `0x28` | `zigbee.bindings` | Zigbee binding | 32 | `datapoints[].zigbee` |
| `0x2C` | `external.sources` | source | 20 | `external.sources` (3.8) |
| `0x2D` | `external.bindings` | binding | 32 | `external.sources[].bindings` |
| `0x2E` | `mqtt` | exactly one | 20 | `mqtt` (3.10) |
| `0x30` | `schedules` | entry | 64 | `schedules` (`spec/schedules.md`) |
| `0x31` | `schedules.dates` | date | 4 | `schedules[].dates` |
| `0x32` | `schedules.bounds` | bound | 32 | `schedules[].bounds` |
| `0x40` | `rules` | rule | 28 | `rules` (`spec/rules.md` 2) |
| `0x41` | `rules.parameters` | parameter | 28 | `rules[].parameters`, `enabled` first |
| `0x42` | `rules.choices` | choice | 8 | `rules[].parameters[].choices` |
| `0x43` | `rules.timers` | timer | 8 | `rules[].timers` |
| `0x44` | `rules.states` | state | 16 | `rules[].states` |
| `0x45` | `rules.reactions` | reaction | 16 | `rules[].reactions` |
| `0x46` | `rules.triggers` | trigger | 8 | `rules[].reactions[].triggers` |
| `0x47` | `rules.actions` | action | 8 | every action list |
| `0x48` | `rules.logParts` | log part | 8 | the `message` of `log` actions |
| `0x49` | `rules.timelines` | timeline | 8 | `rules[].timeline` |
| `0x4A` | `rules.plans` | plan | 12 | `rules[].timeline.plans` |
| `0x4B` | `rules.points` | point | 16 | `rules[].timeline.plans[].points` |
| `0x4C` | `rules.assignments` | assignment | 2 | `rules[].timeline.assignments` |
| `0x4D` | `nodes` | expression node | 16 | every expression (`spec/rules.md` 5) |
| `0x4E` | `constants` | value | 40 | parameter values, long literals |
| `0x50` | `blocks` | instance | 28 | `blocks` (`spec/blocks.md` 4) |
| `0x51` | `blocks.ports` | port binding | 12 | `blocks[].inputs`, `outputs`, `aggregation` |
| `0x52` | `blocks.bound` | datapoint index | 2 | the datapoints of the ports |
| `0x53` | `blocks.parameters` | parameter value | 16 | `blocks[].parameters` |
| `0x60` | `ui` | bytes | — | `ui` (3.9), as the body of `GET /api/pages` |
| `0x62` | `ui.anonymous` | bytes | — | the pages of minimum role `anonymous` (3.9), as the body of `GET /api/pages` for `anonymous` (format 1.1) |
| `0x63` | `ui.anonymous.datapoints` | anonymous datapoint | 4 | the datapoints the anonymous pages show (format 1.1) |
| `0x64` | `ui.anonymous.functions` | function index | 2 | the functions the anonymous pages show (format 1.1) |

`image` and `strings` are required in every image; every other section may be absent. The id `0x61`
is left free: the mutation corpus (13) uses it as the section a firmware does not know.

### 9.5 Strings and labels

**`strings`** is a byte section: NUL-terminated UTF-8 strings one after the other, the first the
empty string (the byte 0 at offset 0), the last byte of the section a NUL. A string reference is the
offset of a string's first byte. Every string is at most 511 bytes. E-Home writes every distinct
string once, in ascending bytewise order.

**`labels`** holds label entries of 8 bytes:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `tag` | String: a language tag (`de`, `en-GB`); the empty string for a plain label. |
| 4 | `u32` | `text` | String: the label's text in that language. |

A **label reference** is a `u32`: bits 0..23 the index of its first entry, bits 24..31 the number of
its entries, 1..16; 0 is no label. A plain label (a JSON string) is one entry whose tag is the empty
string; a language map is its entries in the source's order, every tag non-empty. The API writes a
label back as the source gave it (`"Hall"` or `{"en": "Hall", "de": "Flur"}`); the device resolves
it in its language (`spec/rules.md` 2.2) by reading the entries. E-Home writes the entries of every
label reference anew, in the order the references occur in the image (sections by id, records in
order, fields in order); no two references share entries.

### 9.6 `image` and `settings`

**`image`** (one record, 32 bytes):

| Offset | Type | Field | Source |
|---|---|---|---|
| 0 | `u32` | `projectId` | String, `image.projectId`. |
| 4 | `u32` | `revision` | `image.revision`. |
| 8 | `u32` | `schemaVersion` | String: the source's `image.schemaVersion` (`"1.7"`), shown in the status. |
| 12 | `u32` | `name` | String, `image.name` (0: none). |
| 16 | `u32` | `generator` | String, `image.generator` (0: none). |
| 20 | `u32` | `generatedAt` | String, `image.generatedAt` (0: none). |
| 24 | `u32` | `historyBudget` | `history.budget` in bytes; without it what the rings and archives need (3.7). |
| 28 | `u32` | `archiveBudget` | `history.archiveBudget` in bytes; without it what the archives need. |

**`settings`** (one record, 32 bytes; absent: the image fixes no setting):

| Offset | Type | Field | Source |
|---|---|---|---|
| 0 | `u32` | `tz` | String, `settings.tz` (0: the device keeps its own). |
| 4 | `u32` | `ntp` | String, `settings.ntp.server` (0: keeps its own). |
| 8 | `u8` | `flags` | Bit 0: `location` given; bit 1: its `elev` given. |
| 9 | `u8[3]` | reserved | |
| 12 | `i32` | `elev` | Metres. |
| 16 | `f64` | `lat` | Degrees. |
| 24 | `f64` | `lon` | Degrees. |

### 9.7 `datapoints`

The datapoint model, in model order: the API's datapoint ids are the record indices, and so are the
datapoint references everywhere in the image and in the `ui` text. The order is that of 3.3 and 3.6:
the source's datapoints in their order, then the time master's (`knx.time`, `knx.date`,
`knx.datetime`, those configured), then per rule `rule/<id>/enabled`, its parameters', its
`rule/<id>/state`, then the schedules' `schedule/<id>/time`, then per block its `enabled`, adjustable
parameters, outputs and aggregated inputs. Record of 24 bytes:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `name` | String: the id (`living.light`, `rule/heat/setpoint`). |
| 4 | `u32` | `label` | Label reference (0: none; an implied datapoint's label is its owner's, 0 here). |
| 8 | `u32` | `unit` | String (0: none). |
| 12 | `u8` | `type` | Value type code (9.14). |
| 13 | `u8` | `flags` | Bits 0..6 the datapoint flags: `0x01` readable, `0x02` writable, `0x04` transmit, `0x08` readOnStart, `0x10` persist, `0x20` history, `0x40` application (`spec/api.md` "Datapoints"); bit 7 (`0x80`): the source has a `virtual` member (the API's `binding` shows `{"virtual": {}}`). |
| 14 | `u8` | `owner` | Who owns it, below. |
| 15 | `u8` | reserved | |
| 16 | `u16` | `ownerIndex` | The owner's record, below. |
| 18 | `u16` | `ownerMember` | Which of the owner's datapoints, below. |
| 20 | `u16` | `history` | Index in `history` (9.9); `kNone` without. |
| 22 | `u16` | reserved | |

| `owner` | Owner | `ownerIndex` | `ownerMember` |
|---|---|---|---|
| 0 | none: a value of the application | `kNone` | 0 |
| 1 | KNX | its `knx.bindings` record | 0 |
| 2 | scheduler | its `schedules` record | 0: the entry's `output`; 1: its time datapoint `schedule/<id>/time` |
| 3 | time master | its `knx.bindings` record | 0 `knx.time`, 1 `knx.date`, 2 `knx.datetime` |
| 4 | Zigbee | its `zigbee.bindings` record | 0 |
| 5 | rule engine | its `rules` record | the parameter's local index (0 `enabled`), or `0x8000` for the state datapoint |
| 6 | block runtime | its `blocks` record | `kind × 256 + index`: kind 0 the enable switch (index 0), 1 a parameter, 2 an output, 3 an aggregated input; `index` the parameter's or port's position in the block's records (9.15) |
| 7 | external values | its `external.bindings` record | 0 |

Every owner record names its datapoints back (`knx.bindings[].datapoint`, `schedules[].output` and
`timeDatapoint`, `rules.parameters[].datapoint`, `rules[].stateDatapoint`, ...); the two directions
agree (10.7). The Zigbee adapter's generic datapoints of joined devices (`doc/image.md` "Dynamic
datapoints") are not in the image; they follow it in the model.

### 9.8 `locations`, `functions`, `roles`

**`locations`** (12 bytes):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `id` | String. |
| 4 | `u32` | `label` | Label reference. |
| 8 | `u16` | `parent` | Location index, `kNone` for none. |
| 10 | `u8` | `kind` | 0 none, 1 `building`, 2 `floor`, 3 `room`, 4 `area`, 5 `outside`. |
| 11 | `u8` | reserved | |

**`functions`** (28 bytes):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `id` | String. |
| 4 | `u32` | `label` | Label reference. |
| 8 | `u32` | `unit` | String (0: none). |
| 12 | `u16` | `room` | Location index, `kNone` for none. |
| 14 | `u16` | `firstRole` | Run in `roles`. |
| 16 | `u8` | `kind` | 0 `light`, 1 `dimmer`, 2 `blind`, 3 `switch`, 4 `sensor`, 5 `climate`, 6 `scene`, 7 `button`. |
| 17 | `u8` | `scene` | 1..64 for a scene, else 0. |
| 18 | `u8` | `roleCount` | |
| 19 | `u8` | reserved | |
| 20 | `u8[6]` | `presses` | A button's `presses` (3.5), one byte per kind of press in the order `press`, `on`, `off`, `up`, `down`, `release`: bit `e` set when element `e` (0..5) is a press of that kind; all 0 without `presses`. The API lists the elements in ascending order. |
| 26 | `u16` | reserved | |

**`roles`** (8 bytes), the function's roles in the source's order:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `name` | String: the role (`command`, `levelCommand`, ...; a role E-Home's validation does not know is not compiled). |
| 4 | `u16` | `datapoint` | Datapoint index. |
| 6 | `u16` | reserved | |

### 9.9 `history`, `archives`

**`history`** (16 bytes), one ring per datapoint with a history, in datapoint order:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u16` | `datapoint` | Datapoint index. |
| 2 | `u8` | `archiveCount` | 0..4. |
| 3 | `u8` | reserved | |
| 4 | `u32` | `retention` | Seconds. |
| 8 | `u32` | `spacing` | Seconds. |
| 12 | `u16` | `firstArchive` | Run in `archives`. |
| 14 | `u16` | reserved | |

**`archives`** (8 bytes): `u32 resolution` at 0, `u32 retention` at 4, seconds.

### 9.10 KNX

**`knx.addresses`** (8 bytes): every group address the image uses (a datapoint's `send` and
`listen`, the time master's), each once, in ascending order of the value:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u16` | `address` | The group address. |
| 2 | `u16` | `firstListener` | Run in `knx.listeners`: the bindings a telegram on this address updates (those that send or listen on it). |
| 4 | `u16` | `listenerCount` | |
| 6 | `u16` | reserved | |

**`knx.bindings`** (12 bytes), one per KNX datapoint and per datapoint of the time master, in
ascending order of the datapoint:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u16` | `datapoint` | Datapoint index. |
| 2 | `u16` | `dptMain` | The DPT's main number (`9` of `9.001`). |
| 4 | `u16` | `dptSub` | Its sub number (`1`). The API writes the DPT as `main.sub` with three digits of sub (`9.001`). |
| 6 | `u16` | `send` | Address index of `send`; `kNone` without. |
| 8 | `u16` | `firstListen` | Run in `knx.listen`: the `listen` addresses, in the source's order, without `send`. |
| 10 | `u8` | `listenCount` | 0..8. |
| 11 | `u8` | reserved | |

A binding whose datapoint's owner is the time master (9.7) transmits for it: the adapter sends what
the time master commands and owns no value of it. The time master's bindings have the DPT 10.001,
11.001 and 19.001 and their address as `send`.

**`knx.listen`** (2 bytes): `u16` address indices, the bindings' runs. **`knx.listeners`** (2 bytes):
`u16` binding indices, the addresses' runs, each run in ascending order.

**`knx.timeMaster`** (one record, 12 bytes; absent: no time master):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u16` | `time` | Datapoint index of `knx.time`, `kNone` without `time`. |
| 2 | `u16` | `date` | Of `knx.date`. |
| 4 | `u16` | `dateTime` | Of `knx.datetime`. |
| 6 | `u8` | `flags` | Bit 0 `atMidnight`, bit 1 `onSynchronized` (the source's defaults written out). |
| 7 | `u8` | reserved | |
| 8 | `u32` | `interval` | Seconds (default 600 written out). |

The broadcast addresses are the `send` of these datapoints' bindings.

### 9.11 Zigbee, external values, MQTT

**`zigbee.bindings`** (32 bytes), one per datapoint with a `zigbee` member:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u64` | `ieee` | The device's IEEE address. |
| 8 | `u16` | `datapoint` | Datapoint index. |
| 10 | `u16` | `cluster` | |
| 12 | `u16` | `attribute` | 0 for a client binding. |
| 14 | `u16` | `command` | The cluster commands go to; `0xFFFF` without (read-only). |
| 16 | `u16` | `reportMin` | `reporting.min`, seconds (flag bit 1). |
| 18 | `u16` | `reportMax` | `reporting.max`, seconds (flag bit 2). |
| 20 | `u8` | `endpoint` | 1..240. |
| 21 | `u8` | `flags` | Bit 0 `client`; bits 1, 2, 3: `reporting.min`, `max`, `change` given. |
| 22 | `u16` | reserved | |
| 24 | `f64` | `reportChange` | `reporting.change`, in the datapoint's unit (flag bit 3). |

**`external.sources`** (20 bytes):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `id` | String. |
| 4 | `u32` | `url` | String (0 for a preset source). |
| 8 | `u32` | `preset` | String: the preset's name (`open-meteo`; 0 for a URL source). |
| 12 | `u32` | `interval` | Seconds (default 900 written out). |
| 16 | `u16` | `firstBinding` | Run in `external.bindings`. |
| 18 | `u8` | `bindingCount` | |
| 19 | `u8` | reserved | |

**`external.bindings`** (32 bytes):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u16` | `datapoint` | Datapoint index. |
| 2 | `u16` | reserved | |
| 4 | `u32` | `path` | String (0 for a preset's binding). |
| 8 | `u32` | `variable` | String: the preset's variable (0 for a URL's binding). |
| 12 | `u32` | `unit` | String (0: none). |
| 16 | `f64` | `scale` | Default 1 written out. |
| 24 | `f64` | `offset` | Default 0. |

**`mqtt`** (one record, 20 bytes; absent: no MQTT):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `broker` | String. |
| 4 | `u32` | `username` | String (0: the setting `mqtt.user`). |
| 8 | `u32` | `baseTopic` | String (0: `homecontrol/<device>`). |
| 12 | `u32` | `discoveryPrefix` | String (0: `homeassistant`). |
| 16 | `u16` | `historyInterval` | Minutes, 1..1440 (default 15 written out). |
| 18 | `u8` | `flags` | Bit 0 `enabled`, bit 1 `retain`, bit 2 `discovery.enabled`, bit 3 `history.enabled` (the source's defaults written out). |
| 19 | `u8` | reserved | |

### 9.12 Schedules

**`schedules`** (64 bytes), in image order; only the members of the entry's kind are meaningful,
the others 0:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `id` | String. |
| 4 | `u32` | `label` | String (a schedule's label is a plain string). |
| 8 | `u8` | `kind` | 0 `weekly`, 1 `yearly`, 2 `dates`, 3 `astro`, 4 `interval`, 5 `once`. |
| 9 | `u8` | `flags` | Bit 0 `enabled`, bit 1 `enabledAdjustable`, bit 2 `missed` is `catchUp`, bit 3 `notBefore` set, bit 4 `notAfter` set. |
| 10 | `u8` | `days` | Weekday bits, bit 0 Monday .. bit 6 Sunday (default all written out). |
| 11 | `u8` | `sun` | 0 `dawn`, 1 `sunrise`, 2 `sunset`, 3 `dusk`. |
| 12 | `u16` | `output` | Datapoint index, `kNone` without. |
| 14 | `u16` | `timeDatapoint` | Datapoint index of `schedule/<id>/time`, `kNone` without. |
| 16 | `u16` | `adjustable` | Member bits: `0x001` time, `0x002` days, `0x004` offset, `0x008` notBefore, `0x010` notAfter, `0x020` every, `0x040` from, `0x080` until, `0x100` at. |
| 18 | `u16` | `every` | Minutes, 1..720. |
| 20 | `u16` | `firstDate` | Run in `schedules.dates`. |
| 22 | `u16` | `dateCount` | 1..128 for `dates`. |
| 24 | `u16` | `firstBound` | Run in `schedules.bounds`. |
| 26 | `u8` | `boundCount` | 0..9. |
| 27 | `u8` | `month` | `yearly`: 1..12. |
| 28 | `u8` | `day` | `yearly`: 1..31. |
| 29 | `u8[3]` | reserved | |
| 32 | `i32` | `offset` | `astro`: seconds. |
| 36 | `u32` | `time` | Seconds of the day (`weekly`, `yearly`, `dates`). |
| 40 | `u32` | `notBefore` | Seconds of the day. |
| 44 | `u32` | `notAfter` | Seconds of the day. |
| 48 | `u32` | `from` | Seconds of the day. |
| 52 | `u32` | `until` | Seconds of the day. |
| 56 | `u16` | `atYear` | `once`. |
| 58 | `u8` | `atMonth` | |
| 59 | `u8` | `atDay` | |
| 60 | `u32` | `atTime` | Seconds of the day. |

**`schedules.dates`** (4 bytes): `u16 year`, `u8 month`, `u8 day`.

**`schedules.bounds`** (32 bytes): `u16 member` (one member bit) at 0, reserved to 8, `i64 min` at
8, `i64 max` at 16, `i64 step` at 24 (0: none), in the member's scalar of `ScheduleBound`
(`core/include/hc/scheduler.h`): seconds of the day for a time, seconds for `offset` and `every`,
local seconds since 1970-01-01 for `at`; the source's absent `min` and `max` written out.

### 9.13 Rules

The compiled rule set of `spec/rules.md`, in rule order. Each rule owns runs in the tables below.

**`rules`** (28 bytes):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `id` | String. |
| 4 | `u32` | `name` | Label reference. |
| 8 | `u16` | `firstParameter` | Run in `rules.parameters`; local parameter 0 is `enabled`. |
| 10 | `u16` | `firstTimer` | Run in `rules.timers`. |
| 12 | `u16` | `firstState` | Run in `rules.states`. |
| 14 | `u16` | `firstReaction` | Run in `rules.reactions`. |
| 16 | `u8` | `parameterCount` | 1..8, `enabled` included. |
| 17 | `u8` | `timerCount` | 0..8. |
| 18 | `u8` | `stateCount` | 0..16. |
| 19 | `u8` | `reactionCount` | 0..16. |
| 20 | `u8` | `initialState` | Local state index; `0xFF` without states. |
| 21 | `u8` | reserved | |
| 22 | `u16` | `timeline` | Index in `rules.timelines`, `kNone` without. |
| 24 | `u16` | `stateDatapoint` | Datapoint index of `rule/<id>/state`; `kNone` without states (and in a rule of a source below 1.7 that declares a parameter `state`, section 8). |
| 26 | `u16` | reserved | |

**`rules.parameters`** (28 bytes); per rule first the enable switch (`id` `enabled`, type boolean,
default the rule's `enabled`, adjustable as `enabledAdjustable`, no label: the API shows the rule's
name), then the declared parameters in order:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `id` | String. |
| 4 | `u32` | `label` | Label reference. |
| 8 | `u32` | `unit` | String (0: none). |
| 12 | `u8` | `type` | Expression type code (9.14): boolean, integer, real, string, duration, timeOfDay, enumeration or color. |
| 13 | `u8` | `adjustable` | Boolean. |
| 14 | `u8` | `choiceCount` | 0..16. |
| 15 | `u8` | reserved | |
| 16 | `u16` | `firstChoice` | Run in `rules.choices`. |
| 18 | `u16` | `default` | Constant index. |
| 20 | `u16` | `min` | Constant index, `kNone` without. |
| 22 | `u16` | `max` | Constant index, `kNone` without. |
| 24 | `u16` | `step` | Constant index (a real, integer or duration), `kNone` without. |
| 26 | `u16` | `datapoint` | Datapoint index of `rule/<ruleId>/<id>`. |

**`rules.choices`** (8 bytes): `u16 value` (constant index) at 0, reserved `u16` at 2, `u32 label`
(label reference) at 4.

**`rules.timers`** (8 bytes): `u32 id` (string) at 0, `u16 duration` (expression root) at 4,
reserved `u16` at 6.

**`rules.states`** (16 bytes):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `id` | String. |
| 4 | `u32` | `label` | Label reference (0: none). |
| 8 | `u16` | `firstEntry` | Run in `rules.actions`: the entry actions. |
| 10 | `u8` | `entryCount` | 0..16. |
| 11 | `u8` | reserved | |
| 12 | `u16` | `firstExit` | Run in `rules.actions`: the exit actions. |
| 14 | `u8` | `exitCount` | 0..16. |
| 15 | `u8` | reserved | |

**`rules.reactions`** (16 bytes):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u16` | `firstTrigger` | Run in `rules.triggers`. |
| 2 | `u8` | `triggerCount` | 1..8. |
| 3 | `u8` | `to` | Local state index; `0xFF` without. |
| 4 | `u16` | `condition` | Expression root; `kNone`: always satisfied. |
| 6 | `u16` | `from` | Bit per local state index; `0xFFFF` without `from`. |
| 8 | `u16` | `firstAction` | Run in `rules.actions`: `actions`. |
| 10 | `u8` | `actionCount` | 0..16. |
| 11 | `u8` | reserved | |
| 12 | `u16` | `firstElse` | Run in `rules.actions`: `elseActions`. |
| 14 | `u8` | `elseCount` | 0..16 (0 also without the member). |
| 15 | `u8` | reserved | |

**`rules.triggers`** (8 bytes): `u8 kind` at 0, `u8 flags` at 1, `u16 ref` at 2, `u16 ref2` at 4,
reserved `u16` at 6:

| `kind` | Trigger | `flags` | `ref` | `ref2` |
|---|---|---|---|---|
| 0 | `datapoint` | bit 0 `on: update`; bit 1 `origin: report`; bit 2 `origin: command` | datapoint index | `kNone` |
| 1 | `match` | 0 | expression root of the condition | `kNone` |
| 2 | `schedule` | 0 | `schedules` index | `kNone` |
| 3 | `timer` | 0 | local timer index | `kNone` |
| 4 | `start` | bit 0 `boot`, bit 1 `image`, bit 2 `enable` (the default all three written out) | `kNone` | `kNone` |
| 5 | `parameter` | 0 | local parameter index | `kNone` |
| 6 | `silence` | 0 | datapoint index | expression root of `after` |

**`rules.actions`** (8 bytes): `u8 kind` at 0, `u8 flags` at 1, `u16 target` at 2, `u16 expr` at 4,
`u16 count` at 6. Each action list (a reaction's `actions` and `elseActions`, a state's `entry` and
`exit`) is a run.

| `kind` | Action | `flags` | `target` | `expr` | `count` |
|---|---|---|---|---|---|
| 0 | `write` | bit 0 `onlyOnChange` | datapoint index | root of `value` | 0 |
| 1 | `startTimer` | 0 | local timer index | root of `duration`, `kNone` without | 0 |
| 2 | `restartTimer` | 0 | local timer index | root of `duration`, `kNone` without | 0 |
| 3 | `cancelTimer` | 0 | local timer index | `kNone` | 0 |
| 4 | `setParameter` | 0 | local parameter index | root of `value` | 0 |
| 5 | `log` | `level`: 0 info, 1 warning, 2 error | first in `rules.logParts` | `kNone` | parts, 1..8 |
| 6 | `read` | 0 | datapoint index | `kNone` | 0 |

**`rules.logParts`** (8 bytes): `u32 text` (string; 0 for an expression part) at 0, `u16 expr`
(expression root; `kNone` for a text part) at 4, reserved `u16` at 6.

**`rules.timelines`** (8 bytes): `u16 firstPlan` at 0, `u16 firstAssignment` at 2, `u8 planCount`
(1..8) at 4, `u8 assignmentCount` (1..8) at 5, `u8 flags` (bit 0 `adjustable`) at 6, reserved `u8`
at 7.

**`rules.plans`** (12 bytes): `u32 id` (string) at 0, `u32 label` (label reference) at 4,
`u16 firstPoint` (run in `rules.points`) at 8, `u8 pointCount` (0..16) at 10, reserved `u8` at 11.

**`rules.points`** (16 bytes), the plan's points in the source's order (the plan order of
`spec/rules.md` 7.13):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u8` | `state` | Local state index. |
| 1 | `u8` | `sun` | 0 a clock point; 1 + the sun event (1 `dawn`, 2 `sunrise`, 3 `sunset`, 4 `dusk`). |
| 2 | `u8` | `flags` | Bit 0 `notBefore` given, bit 1 `notAfter` given. |
| 3 | `u8` | reserved | |
| 4 | `i32` | `seconds` | A clock point: its time, seconds of the day; a sun point: its `offset` in seconds. |
| 8 | `i32` | `notBefore` | Seconds of the day. |
| 12 | `i32` | `notAfter` | Seconds of the day. |

**`rules.assignments`** (2 bytes): `u8 plan` (the plan's index within the timeline) at 0, `u8 days`
(weekday bits as `schedules.days`; 0 for the default) at 1.

### 9.14 Expressions: `nodes` and `constants`

**Codes.** Value types of datapoints (`datapoints.type`, `doc/datapoints.md`): 1 boolean, 2
unsigned, 3 signed, 4 real, 5 string, 6 timeOfDay, 7 date, 8 dateTime, 9 enumeration, 10 raw, 11
controlled, 12 color. Expression types (`spec/rules.md` 3.1; `nodes.type`, `constants.type`,
`rules.parameters.type`): 0 boolean, 1 integer, 2 real, 3 string, 4 duration, 5 timeOfDay, 6 date,
7 dateTime, 8 enumeration, 9 controlled, 10 color, 11 raw.

**An expression** is a tree of nodes in `nodes`, referenced by its root's index. Its nodes are the
range `[root, root + root.size)`, in the order E-Home's compiler allocates them (and the device's
rule loader did): the root first; the children of a node are consecutive (`a .. a + argc − 1`) and
lie after the node; a node's children are allocated as one block when the node is written, before
the subtrees of the children, which follow in child order. `remaining(off) > 1min30s` is

```
n0 binary  op 8 (>)  argc 2 a 1 size 4      the root
n1 call    op 40 (remaining) argc 1 a 3 size 2
n2 literal type 4 (duration) lit 90000 size 1
n3 timer   a 0 (local timer 0) size 1
```

**`nodes`** (16 bytes):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u8` | `kind` | 0 literal, 1 datapoint, 2 parameter, 3 timer, 4 unary, 5 binary, 6 call (`spec/rules.md` 5). |
| 1 | `u8` | `op` | unary and binary: the operator; call: the function; else 0. |
| 2 | `u8` | `type` | The node's static result type (expression type code). |
| 3 | `u8` | `argc` | Children: literal, datapoint, parameter, timer 0; unary 1; binary 2; call its arguments. |
| 4 | `u16` | `a` | literal: the constant index of a `string`, `raw` or `dateTime` literal, else `kNone`; datapoint: the datapoint index; parameter: the local parameter index; timer: the local timer index; unary, binary, call: the first child (`kNone` for a call without arguments). |
| 6 | `u16` | `size` | The nodes of the subtree rooted here, this node included. |
| 8 | 8 bytes | `lit` | literal: the value of the short types (below); call of `state`: `u32` the index of the rule whose state it reads (the expression's own rule for `state()`); else 0. |

The literal payload at offset 8 (the rest of the 8 bytes 0): boolean `u8`; integer and duration
`i64` (milliseconds); real `f64`; enumeration `u32`; timeOfDay `u8 dayOfWeek, u8 hour, u8 minute,
u8 second`; date `u16 year, u8 month, u8 day`; controlled `u8 control, u8 value`; color `u8 red,
green, blue, white, valid` (valid: bit 3 red, bit 2 green, bit 1 blue, bit 0 white).

The trees are normalized (`spec/rules.md` 4.8): no unary minus over a literal; an enumeration
literal is an integer literal of type enumeration only where 4.8 says so; a controlled literal is
the call `controlled(b, n)` of a boolean and an integer literal.

Operators (`op` of unary and binary): 0 `-` (negation, unary), 1 `not` (unary), 2 `or`, 3 `and`, 4
`==`, 5 `!=`, 6 `<`, 7 `<=`, 8 `>`, 9 `>=`, 10 `+`, 11 `-`, 12 `*`, 13 `/`, 14 `%` (binary).

Functions (`op` of a call) with their argument counts:

| Code | Function | Arguments | Code | Function | Arguments |
|---|---|---|---|---|---|
| 0 | `if` | 3 | 28 | `sunrise` | 0 |
| 1 | `known` | 1 | 29 | `sunset` | 0 |
| 2 | `coalesce` | 2 | 30 | `dawn` | 0 |
| 3 | `between` | 3 | 31 | `dusk` | 0 |
| 4 | `hysteresis` | 4 | 32 | `sunAzimuth` | 0 |
| 5 | `abs` | 1 | 33 | `sunElevation` | 0 |
| 6 | `min` | 2..8 | 34 | `sinceUpdate` | 1, a datapoint node |
| 7 | `max` | 2..8 | 35 | `sinceChange` | 1, a datapoint node |
| 8 | `clamp` | 3 | 36 | `changedWithin` | 2, the first a datapoint node |
| 9 | `round` | 1 | 37 | `lastKnown` | 1, a datapoint node |
| 10 | `floor` | 1 | 38 | `quality` | 1, a datapoint node |
| 11 | `ceil` | 1 | 39 | `running` | 1, a timer node |
| 12 | `trunc` | 1 | 40 | `remaining` | 1, a timer node |
| 13 | `roundTo` | 2 | 41 | `state` | 0, or 1 string literal |
| 14 | `toInt` | 1 | 42 | `length` | 1 |
| 15 | `now` | 0 | 43 | `contains` | 2 |
| 16 | `today` | 0 | 44 | `startsWith` | 2 |
| 17 | `time` | 0 | 45 | `endsWith` | 2 |
| 18 | `weekday` | 0..1 | 46 | `toString` | 1 |
| 19 | `hour` | 1 | 47 | `rgb` | 3 |
| 20 | `minute` | 1 | 48 | `rgbw` | 4 |
| 21 | `second` | 1 | 49 | `red` | 1 |
| 22 | `year` | 1 | 50 | `green` | 1 |
| 23 | `month` | 1 | 51 | `blue` | 1 |
| 24 | `day` | 1 | 52 | `white` | 1 |
| 25 | `dateOf` | 1 | 53 | `controlled` | 2 |
| 26 | `timeOf` | 1 | 54 | `controlBit` | 1 |
| 27 | `inWindow` | 3 | 55 | `controlValue` | 1 |

**`constants`** (40 bytes): the values of the parameters (defaults, bounds, steps, choices) and the
literals of the long types. The layout is the firmware's expression value (`hc::rules::Val`), so the
engine reads a constant in place:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u8` | `type` | Expression type code. |
| 1 | `u8` | `present` | 1. |
| 2 | `u8` | `length` | string: its bytes without the NUL, 0..28; raw: its bytes, 0..14; else 0. |
| 3 | `u8[5]` | reserved | |
| 8 | 32 bytes | `value` | The short types as the node payload above; dateTime `u16 year, u8 month, u8 day, u8 dayOfWeek, u8 hour, u8 minute, u8 second, u16 flags` (`spec/rules.md` 3.1; a literal's flags 0); string the bytes and a NUL; raw the bytes; the rest 0. |

### 9.15 Blocks

**`blocks`** (28 bytes), in block order:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `id` | String. |
| 4 | `u32` | `type` | String: the type id (`org.homecontrol.room-controller`). |
| 8 | `u32` | `label` | Label reference. |
| 12 | `u16` | `major` | The image's `version`. |
| 14 | `u16` | `minor` | |
| 16 | `u16` | `firstPort` | Run in `blocks.ports`. |
| 18 | `u16` | `firstParameter` | Run in `blocks.parameters`. |
| 20 | `u8` | `portCount` | The ports of the type in its manifest's order, at the image's version: every port, bound or not. |
| 21 | `u8` | `parameterCount` | The parameters of the type in manifest order, at the image's version. |
| 22 | `u8` | `flags` | Bit 0 `enabled`, bit 1 `enabledAdjustable`. |
| 23 | `u8` | reserved | |
| 24 | `u16` | `enabledDatapoint` | Datapoint index of `block/<id>/enabled`. |
| 26 | `u16` | reserved | |

**`blocks.ports`** (12 bytes):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `id` | String: the port's id, as the manifest names it at this position. |
| 4 | `u16` | `firstBound` | Run in `blocks.bound`. |
| 6 | `u8` | `boundCount` | 0 (unbound) .. 32; an output at most 1. |
| 7 | `u8` | `aggregation` | 0 none (bound to at most one), 1 `mean`, 2 `min`, 3 `max`, 4 `any`, 5 `all` (the default written out). |
| 8 | `u16` | `mirror` | Datapoint index: an output's `block/<id>/<output>`, an input bound to more than one datapoint its `block/<id>/<input>`; `kNone` for the other inputs. |
| 10 | `u16` | reserved | |

**`blocks.bound`** (2 bytes): `u16` datapoint indices, the ports' runs, in the source's order.

**`blocks.parameters`** (16 bytes):

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `id` | String: the parameter's id, as the manifest names it at this position. |
| 4 | `u16` | `datapoint` | Datapoint index of `block/<id>/<parameter>` for an adjustable parameter, `kNone` otherwise. |
| 6 | `u8` | `flags` | Bit 0: the image gives the value (else the manifest's default applies, `value` 0). |
| 7 | `u8` | reserved | |
| 8 | `f64` | `value` | The image's value: a choice by its value, a duration in ms, a time of day in seconds since midnight, a boolean 0 or 1. |

A firmware whose type has a newer minor version than the image's finds fewer records than its
manifest has: the ports beyond are unbound and have no mirror, the parameters beyond take the
manifest's default and are not adjustable on the device (no datapoint).

### 9.16 `ui`

A byte section: the control pages (3.9) as the UTF-8 JSON text of the body of `GET /api/pages`
(`spec/api.md`): one object with the lists `pages`, `sections`, `elements`, `series` and
`references`, defaults resolved, datapoints by their index (the API's id), functions by their id.
The device serves it unchanged and does not read it; E-Home validated it (3.9, the limits of
section 5). Absent: no control pages (the device UI shows its generated overview).

**The anonymous pages** (format 1.1, I12; written only when a page has the minimum role
`anonymous`, then `ui.anonymous` always, the two tables when they are not empty):

- **`ui.anonymous`** (`0x62`), a byte section: the body of `GET /api/pages` for `anonymous`, in the
  form of `ui` and built from the anonymous pages alone (3.9). Served unchanged, never read, like
  `ui`.
- **`ui.anonymous.datapoints`** (`0x63`), records of 4 bytes in strictly ascending order of
  `datapoint`, so the device finds an entry by binary search:

  | Offset | Type | Field | Meaning |
  |---|---|---|---|
  | 0 | `u16` | `datapoint` | Datapoint index (9.7). |
  | 2 | `u8` | `flags` | Bit 0 `operate`: `anonymous` may command it (a rule parameter's datapoint: set the parameter). Without it the datapoint is only read. |
  | 3 | `u8` | reserved | |

- **`ui.anonymous.functions`** (`0x64`), records of a `u16` function index (9.8), strictly
  ascending.

`anonymous` reads and operates nothing that is not listed (`spec/api.md` "Authentication"). Absent
`ui.anonymous`: nobody gets in without sign-in, whatever else the image holds.

### 9.17 The signature

A signed image (header flag `signed`) ends with 64 bytes: the Ed25519 signature (RFC 8032 section
5.1.6, pure Ed25519) with the installation key over the 52-byte message

```
M = "homecontrol-image-v2" || SHA-256(image[0, signedEnd))
```

(the 20 ASCII bytes without a terminator, then the digest of everything before the signature: the
header with its CRC, the table, the sections, the padding). The context `v2` keeps a signature of a
JSON image (`v1`, firmware up to 1.2) from ever verifying a binary one. E-Home writes the header
with `signed` and the final `length`, computes the CRC, signs, and appends the signature. The device
checks it while it holds the installation key, when it receives an image; not at boot (I11,
`spec/api.md` "Signed runtime images"). Without a key the signature is not checked; an unsigned image
to a device with a key is refused from its header (10.1).

### 9.18 The canonical layout

E-Home's compiler writes the same bytes for the same source (haumacher/ehome#176: the corpus `.bin`
files are compared byte for byte). Where the format leaves a choice, it writes:

- the header of 32 bytes, the section table behind it, then the sections in ascending order of their
  ids, each at the next multiple of 8, no empty table section, `entrySize` the size of this
  document, reserved bytes and padding 0, `length` the end of the last section rounded up to 8 (plus
  64 for the signature);
- `strings` sorted and without duplicates (9.5); `labels` as 9.5 says;
- the tables in the order of their source: datapoints in model order (9.7); locations, functions,
  schedules, rules, blocks, external sources in source order; `zigbee.bindings`, `knx.bindings`
  and `history` by datapoint; `knx.addresses` by value;
- the runs of a record in the order of the records that own them (the roles of function 0, then of
  function 1, ...), so each table is the concatenation of its runs;
- per rule, in rule order, the walk: its parameters (`enabled` first), appending the constants of
  each (`default`, `min`, `max`, `step`, then its choices' values); its timers, each followed by its
  duration's expression; its states, each with its entry actions, then its exit actions; its
  reactions, each with its triggers (and their expressions), its condition, its actions and its
  else actions; its timeline (plans with their points, then the assignments). An action list's
  actions are appended together, then the log parts and expressions of each action in order. An
  expression's nodes are appended when the walk reaches it, in the allocation order of 9.14, and
  the constant of a long literal when its node is written.

The device relies on none of this: it accepts every layout section 10 accepts.

## 10. Validation on the device

What the device checks, in this order, on receipt (`PUT /api/image`, the console, the POSIX
`--image`) and at every boot, except the two checks of the installation key (the unsigned image of
10.1 and the signature of 10.2), which run on receipt only (I11). Everything here is structure or a fact of this firmware (its codecs, its
function table, its block types, its memory caps); the meaning is E-Home's (I7). Section 4 gives the
kinds and codes; the paths name the header (`header.<field>`), an entry of the section table
(`sections[k].<field>`, `k` its position), a record (`<section name>[i].<field>`, the names of 9.4,
`i` the record's index) or a whole section (`strings`).

**The hint.** A refusal of an unknown code, flag bit or required section adds, when the image's
minor version is newer than the firmware's: ` (the image's format 1.2 is newer than this firmware
reads, up to 1.1: update the firmware)`.

### 10.1 The header, before the slot is erased

| Check | Path | Code | Reason |
|---|---|---|---|
| The body announced or received has at least 32 bytes. | `header` | `image.format` | `not a runtime image: 20 bytes, the header alone has 32` |
| `magic` is `48 43 49 4D`. | `header.magic` | `image.format` | `not a runtime image: the magic is 7B 0A 09 22, not 48 43 49 4D ("HCIM")`; when the first byte is `{` it adds `: a JSON text (E-Home compiles the image; send the binary form)` |
| `major` is 1. | `header.major` | `image.version` | `format 2.0: this firmware reads format 1 (up to 1.1)` |
| `flags` has no bits but `signed`. | `header.flags` | `image.version` | `unknown flags 0x2` with the hint |
| `length` equals the bytes of the body (at boot: is at most the slot's size). | `header.length` | `image.format` | `the header says 112 bytes, the body has 100` (at boot: `the header says 600000 bytes, the slot holds 524288`) |
| `length` is a multiple of 8 and at least `headerSize`, plus 64 when `signed`. | `header.length` | `image.format` | `length 113 is not a multiple of 8`; `length 64 leaves no room for the signature` |
| `headerSize` is at least 32 and a multiple of 8. | `header.headerSize` | `image.format` | `header size 24, at least 32 and a multiple of 8` |
| The section table ends within `signedEnd`. | `header.sectionCount` | `image.format` | `the section table of 300 entries ends at 4832, beyond the 4096 signed bytes` |
| On receipt, while the device holds the installation key: `signed` is set. | `header.flags` | `image.unsigned` | `the image is not signed; the device accepts only images signed with the installation key ce7d 4a41 a476 9ee3` |

On receipt these checks run on the first 32 bytes (the body's length is the announced one); the
running image stays untouched when one fails. At boot a slot whose first four bytes are `FF FF FF
FF` (erased) holds no image: no refusal is reported.

### 10.2 The bytes: CRC and signature

| Check | Path | Code | Reason |
|---|---|---|---|
| The CRC-32 over `[0, signedEnd)` (the field taken as 0) equals `crc`. | `header.crc` | `image.damaged` | `the CRC-32 is 59772c42, the bytes give 0d5e8a10: the image was damaged on the way or in the slot` |
| On receipt, while the device holds the installation key: the signature verifies (9.17). | `signature` | `image.bad_signature` | `the signature does not verify under the installation key ce7d 4a41 a476 9ee3` (an unreadable stored key: `the installation key stored on the device is damaged`) |

### 10.3 The section table

| Check | Path | Reason |
|---|---|---|
| The ids ascend strictly. | `sections[k].id` | `malformed: section 0x10 after 0x12: the table is not in ascending order` (equal: `section 0x10 twice`) |
| `flags` has no bits but `required`. | `sections[k].flags` | `malformed: unknown flags 0x4` with the hint |
| An unknown id is not `required` (an unknown one that is not is skipped, section 11). | `sections[k].id` | `malformed: section 0x61 is required, but this firmware does not know it` with the hint |
| `offset` is a multiple of 8, not before the table's end, not before the previous section's end. | `sections[k].offset` | `malformed: offset 0x41 is not a multiple of 8`; `offset 0x30 lies inside the section table (it ends at 0x40)`; `offset 0x50 lies inside section 0x01 (0x40..0x5f)` |
| `offset + length` is at most `signedEnd`. | `sections[k].length` | `malformed: the section ends at 0x80, beyond the signed bytes (0x70)` |
| A byte section (`strings`, `ui`, `ui.anonymous`): `entrySize` 0. A known table section: `entrySize` at least its record's size of 9.4 and a multiple of the record's alignment (8 with a 64-bit field, 4 with a 32-bit field, else 2); `length` a multiple of `entrySize`. | `sections[k].entrySize` | `malformed: entry size 20 for datapoints, which need at least 24`; `entry size 26 is not a multiple of 4`; `a byte section has entry size 0`; `length 50 is not a multiple of the entry size 24` |
| `image`, `settings`, `knx.timeMaster`, `mqtt` hold exactly one record. | `sections[k].length` | `malformed: the section image holds 2 records, it has one` |
| A table referenced by `u16` indices has at most 65 535 records, `labels` at most 16 777 215. | `sections[k].length` | `limitExceeded: 70000 records, at most 65535` |
| `image` and `strings` are present. | `sections` | `malformed: the section image (0x01) is missing` |

### 10.4 Strings and labels

| Check | Path | Reason |
|---|---|---|
| The first and the last byte of `strings` are NUL. | `strings` | `malformed: offset 0 is not the empty string`; `malformed: the last byte is not a NUL` |
| `strings` is valid UTF-8 (no overlong form, no surrogate, nothing above U+10FFFF, no sequence across a NUL). | `strings` | `malformed: invalid UTF-8 at offset 0x1a` |
| Every string has at most 511 bytes. | `strings` | `limitExceeded: the string at offset 0x40 has 600 bytes, at most 511` |
| `ui` is valid UTF-8 without a NUL. | `ui` | `malformed: invalid UTF-8 at offset 0x1a`; `malformed: a NUL at offset 0x200` |
| `ui.anonymous` likewise (format 1.1). | `ui.anonymous` | the same reasons |

Every reference field of a record is then checked by its kind; each kind has one reason (the field's
path names where):

| Kind | Check | Reason |
|---|---|---|
| String | Below the length of `strings`, and 0 or the byte before it a NUL. | `malformed: 6 is no string (not the start of one of the 13 bytes of strings)` |
| Id string (the fields called `id`, `name` of `datapoints` and `roles`, `projectId`, `schemaVersion`, `type` of `blocks`, `broker`) | A string, not the empty one. | `malformed: empty, it is required` |
| Label reference | 0, or its entries exist (`first + count` at most the entries of `labels`), `count` at most 16, each entry's `tag` and `text` strings; one entry with an empty tag, or every tag non-empty. | `malformed: label entries 40..41 do not exist (31 entries)`; `limitExceeded: a label of 17 languages, at most 16`; `malformed: a language map with an empty tag` |
| Index | Below the count of its table, or `kNone` where the field allows none. | `malformed: datapoint 1203 does not exist (19 datapoints)` |
| Run | `first + count` at most the count of its table (any `first` with count 0). | `malformed: rules.actions 40..42 do not exist (41 actions)` |
| Code | One of the field's values. | `malformed: kind 9 is unknown (0..7)` with the hint |
| Flags | No bit but the field's (below: `flags 0..4` allows bits 0 to 4, `flags 0` bit 0 alone, `no flags` none). | `malformed: unknown bits 0x40 in flags` with the hint |
| Boolean | 0 or 1. | `malformed: 2 is not a boolean (0 or 1)` |
| Range | Within the field's range, finite for an `f64`. | `malformed: spacing 0 is outside 10..86400` |

### 10.5 The records

The fields not listed here are reserved (not checked) or plain numbers the runtime reads as they are.

| Section | Field | Check |
|---|---|---|
| `image` | `projectId`, `schemaVersion`; `name`, `generator`, `generatedAt` | id string; string |
| `settings` | `tz`, `ntp`; `flags`; `lat`, `lon`, `elev` (with flag bit 0) | string; flags 0..1; range −90..90, −180..180, −500..9000 |
| `datapoints` | `name`; `label`; `unit`; `type`; `owner`; `history` | id string; label; string; code 1..12; code 0..7; index into `history` or `kNone`, and flag `0x20` set exactly when it is not `kNone` (`malformed: the history flag and the history index disagree`) |
| `locations` | `id`; `label`; `parent`; `kind` | id string; label; index or `kNone`; code 0..5. The parents form no cycle (`malformed: the parents of location 4 form a cycle`). |
| `functions` | `id`; `label`; `unit`; `room`; roles; `kind`; `scene`; `presses` | id string; label; string; location index or `kNone`; run in `roles`; code 0..7; range 0..64; each byte flags of bits 0..5 |
| `roles` | `name`; `datapoint` | id string; datapoint index |
| `history` | `datapoint`; `spacing`; `retention`; archives | datapoint index of type boolean, unsigned, signed or real (`malformed: a history of a string datapoint: only boolean, unsigned, signed and real`); range 10..86400; range `spacing`..2 678 400 (31 days) and `ceil(retention / spacing) + 1` at most 2048 (`limitExceeded: 4321 samples, at most 2048`); run in `archives`, `archiveCount` at most 4 |
| `archives` | `resolution`; `retention` | range 60..2 678 400 and above the ring's spacing (`malformed: resolution 60 is not above the ring's spacing 60`); range `resolution`..315 360 000 (3650 days) and `ceil(retention / resolution) + 1` at most 2048 |
| `knx.addresses` | `address`; listeners | strictly ascending (`malformed: address 1/1/2 follows 1/1/3`; equal: `address 1/1/2 twice`); run in `knx.listeners` whose entries ascend strictly |
| `knx.bindings` | `datapoint`; `send`; listen | datapoint index, strictly ascending; address index or `kNone`; run in `knx.listen`, `listenCount` at most 8, its addresses distinct and none of them `send` (`malformed: address 1/1/2 is listed twice`) |
| `knx.listen`, `knx.listeners` | each entry | index into `knx.addresses`, `knx.bindings` |
| `knx.timeMaster` | `time`, `date`, `dateTime`; `flags`; `interval` | datapoint index or `kNone`, at least one not `kNone` (`malformed: a time master without an address`); flags 0..1; range 60..86400 |
| `zigbee.bindings` | `datapoint`; `endpoint`; `flags`; `reportChange` | datapoint index; range 1..240; flags 0..3; finite where flag bit 3 is set |
| `external.sources` | `id`; `url`, `preset`; `interval`; bindings | id string; strings, exactly one of them not empty (`malformed: a source has a url or a preset, not both`); range 300..86400; run in `external.bindings`, at least one |
| `external.bindings` | `datapoint`; `path`, `variable`, `unit`; `scale`, `offset` | datapoint index; strings; finite, `scale` not 0 |
| `mqtt` | `broker`; `username`, `baseTopic`, `discoveryPrefix`; `historyInterval`; `flags` | id string; strings; range 1..1440; flags 0..3 |
| `schedules` | `id`; `label`; `kind`; `flags`; `days`; `sun`; `output`, `timeDatapoint`; `adjustable`; `every`; dates; bounds; `month`, `day`; `offset`; `time`, `notBefore`, `notAfter`, `from`, `until`, `atTime`; `atYear`, `atMonth`, `atDay` | id string; string; code 0..5; flags 0..4; flags 0..6, not empty for `weekly`, `astro`, `interval`; code 0..3; datapoint index or `kNone`; flags 0..8; range 1..720 for `interval`; run in `schedules.dates`, 1..128 for `dates`; run in `schedules.bounds`, at most 9; range 1..12 and 1..31 for `yearly`; range −43200..43200; range 0..86399; for `once` a valid date (below) |
| `schedules.dates` | `year`, `month`, `day` | a valid date: year 1970..2155, month 1..12, day 1..the days of that month |
| `schedules.bounds` | `member`; `min`, `max`, `step` | exactly one bit of `adjustable`'s; `min` at most `max`, `step` not negative |
| `rules` | `id`; `name`; parameters, timers, states, reactions; `initialState`; `timeline`; `stateDatapoint` | id string; label; runs in their tables, at least the enable switch; a local state index, `0xFF` exactly when the rule has no states; index into `rules.timelines` or `kNone`, `kNone` without states; datapoint index or `kNone` |
| `rules.parameters` | `id`; `label`; `unit`; `type`; `adjustable`; choices; `default`, `min`, `max`, `step`; `datapoint` | id string; label; string; code 0..5, 8 or 10, and boolean for the rule's local parameter 0 (`malformed: parameter 0 of a rule is its enable switch, a boolean`); boolean; run in `rules.choices`; constant indices (`min`, `max`, `step` or `kNone`) of the parameter's type (`step` a real, integer or duration; the choices' values too; `malformed: constant 7 is a real, the parameter an integer`); datapoint index |
| `rules.choices` | `value`; `label` | constant index; label |
| `rules.timers` | `id`; `duration` | id string; an expression (10.6) |
| `rules.states` | `id`; `label`; entry, exit | id string; label or 0; runs in `rules.actions` |
| `rules.reactions` | triggers; `to`; `condition`; `from`; actions, else | run in `rules.triggers`, at least one; a local state index or `0xFF`; an expression or `kNone`; `0xFFFF` or only bits of the rule's states (`malformed: from names state 5, the rule has 3`); runs in `rules.actions` |
| `rules.triggers` | `kind`; `flags`; `ref`; `ref2` | code 0..6; flags by kind (0..2 for `datapoint`, 0..2 for `start`, none for the others); by kind: datapoint index, an expression, schedule index, local timer index, local parameter index; `silence`: an expression |
| `rules.actions` | `kind`; `flags`; `target`; `expr`; `count` | code 0..6; flags 0 for `write`, `level` code 0..2 for `log`, none for the others; by kind: datapoint index, local timer index, local parameter index, a run in `rules.logParts` of `count` 1..8; an expression for `write` and `setParameter`, an expression or `kNone` for `startTimer` and `restartTimer` |
| `rules.logParts` | `text`, `expr` | exactly one of a non-empty string and an expression (`malformed: a log part is a text or an expression`) |
| `rules.timelines` | plans; assignments; `flags` | runs of 1..8; flags 0 |
| `rules.plans` | `id`; `label`; points | id string; label; run in `rules.points` |
| `rules.points` | `state`; `sun`; `flags`; `seconds`; `notBefore`, `notAfter` | a local state index of its rule; code 0..4; flags 0..1; range 0..86399 for a clock point, −43200..43200 for a sun point; range 0..86399 where flagged |
| `rules.assignments` | `plan`; `days` | below the timeline's `planCount`; flags 0..6 |
| `constants` | every record | `type` code 0..11; `present` 1; `length` 0..28 for a string, 0..14 for raw, 0 otherwise; a string without a NUL in its bytes and with one after them; a boolean 0 or 1; a time of day with day of week 0..7, hour 0..23, minute and second 0..59; a date with year 1900..2155, month 1..12, day 1..31; a date and time likewise with hour 0..24; a controlled value's bit 0 or 1; a color's `valid` flags 0..3; a real finite |
| `blocks` | `id`, `type`; `label`; ports, parameters; `flags`; `enabledDatapoint` | id strings; label; runs in their tables; flags 0..1; datapoint index |
| `blocks.ports` | `id`; bound; `aggregation`; `mirror` | id string; run in `blocks.bound` (its count is checked by the type's `multiple`, 10.8); code 0..5; datapoint index or `kNone` |
| `blocks.bound` | each entry | datapoint index |
| `blocks.parameters` | `id`; `datapoint`; `flags`; `value` | id string; datapoint index or `kNone`; flags 0; finite |
| `ui.anonymous.datapoints` | `datapoint`; `flags` | datapoint index, strictly ascending (`malformed: the anonymous datapoints do not ascend strictly`); flags 0 |
| `ui.anonymous.functions` | each entry | function index, strictly ascending (`malformed: the anonymous functions do not ascend strictly`) |

A table of the anonymous pages without `ui.anonymous` is refused at the table's first record
(`ui.anonymous.datapoints[0]`, `ui.anonymous.functions[0]`: `malformed: a table of the anonymous
pages without the section ui.anonymous`): it would let `anonymous` in without a page to show.

### 10.6 Expressions

The device walks every expression of every rule: the timers' durations, the `match` conditions and
the `after` of `silence` triggers, the reactions' conditions, the actions' values and durations, the
log parts. Each is checked in the context of its rule (its counts of parameters, timers, states).

| Check | Path | Reason |
|---|---|---|
| The root is a node, its expression `[root, root + size)` lies within `nodes` and has at most 64 nodes. | `nodes[r].size` | `malformed: the expression at node 300 reaches beyond the 310 nodes`; `limitExceeded: 70 nodes in an expression, at most 64` |
| Each node's `kind` and `type` are codes (0..6, 0..11). | `nodes[n].kind`, `.type` | code |
| `argc` fits the kind: 0 for literal, datapoint, parameter, timer; 1 for unary; 2 for binary; the function's count for a call (9.14). | `nodes[n].argc` | `malformed: if() takes 3 arguments, the node has 2` |
| The children `a .. a + argc − 1` lie after the node and within its expression; `a` is `kNone` for a call without arguments. | `nodes[n].a` | `malformed: the children 3..4 of node 2 lie outside its expression (nodes 2..6, after the node)` |
| Every node of the expression is the child of exactly one node (the root of none): the expression is a tree. Children lie after their parent, so a walk ends. | `nodes[n]` | `malformed: node 7 is a child of two nodes`; `malformed: node 9 of the expression at node 5 is no node of its tree` |
| The depth (a leaf 1) is at most 12. | `nodes[r]` | `limitExceeded: an expression deeper than 12` |
| `size` is 1 plus the sizes of the children. | `nodes[n].size` | `malformed: size 3, the subtree has 4 nodes` |
| A literal: of type string, raw or dateTime `a` a constant of that type; of type boolean the payload 0 or 1; of type timeOfDay or date the payload's fields within the ranges of `constants` (10.5). | `nodes[n].a`, `.lit` | index; `malformed: constant 4 is a string, the literal a dateTime`; boolean; range |
| A datapoint node: `a` a datapoint index. A parameter node: a local parameter index. A timer node: a local timer index, and only as the argument of `running` or `remaining`. | `nodes[n].a` | index; `malformed: a timer node outside running() and remaining()` |
| A unary node: `op` 0..1; a binary node: `op` 2..14; a call: `op` 0..55. | `nodes[n].op` | code |
| The first argument of `sinceUpdate`, `sinceChange`, `changedWithin`, `lastKnown`, `quality` is a datapoint node; of `running`, `remaining` a timer node. | `nodes[n].a` | `malformed: the first argument of sinceChange() must be a datapoint node` |
| `state`: `lit` names a rule (`u32` below the count of `rules`) that has states; its argument, if any, a string literal. | `nodes[n].lit` | `malformed: state() reads rule 4, which has no states` |
| All expressions together have at most 4096 nodes (`nodes` itself). | `nodes` | `limitExceeded: 5000 nodes, at most 4096` |

### 10.7 Owners and their datapoints

Every datapoint whose `owner` is not 0 and every owner record's datapoint field name each other:

| Datapoint `owner` | The owner record that must name it back |
|---|---|
| 1 KNX | `knx.bindings[ownerIndex].datapoint` |
| 2 scheduler | member 0: `schedules[ownerIndex].output`; member 1: `schedules[ownerIndex].timeDatapoint` |
| 3 time master | `knx.bindings[ownerIndex].datapoint`, and `knx.timeMaster` `time`, `date` or `dateTime` by member 0, 1, 2 |
| 4 Zigbee | `zigbee.bindings[ownerIndex].datapoint` |
| 5 rule engine | member below `0x8000`: `rules.parameters[rules[ownerIndex].firstParameter + member].datapoint`, member below the rule's `parameterCount`; member `0x8000`: `rules[ownerIndex].stateDatapoint` |
| 6 block runtime | kind 0: `blocks[ownerIndex].enabledDatapoint`; kind 1: `blocks.parameters[firstParameter + index].datapoint`; kinds 2 and 3: `blocks.ports[firstPort + index].mirror` (index below the counts) |
| 7 external | `external.bindings[ownerIndex].datapoint` |

And the reverse: every `knx.bindings`, `zigbee.bindings`, `external.bindings` and `history` record,
every `output` and `timeDatapoint` of a schedule, every `datapoint` of a rule's parameter, every
`stateDatapoint`, `enabledDatapoint`, block parameter `datapoint` and port `mirror` that is not
`kNone` names a datapoint whose `owner`, `ownerIndex` and `ownerMember` point back to it (a `history`
record: whose `history` is its index). Reasons: `malformed: datapoint 7 names knx.bindings 3 as its
owner, which names datapoint 8`, `malformed: rules.parameters 4 names datapoint 12, whose owner is
none`.

The addresses and the bindings agree: each binding is in the listeners of its `send` and of each of
its `listen` addresses, and the listeners of all addresses together are as many as the bindings'
`send` and `listen` entries together (`malformed: knx.bindings 3 listens on address 1/1/2, whose
listeners do not name it`; `malformed: 18 listeners, the bindings use addresses 17 times`).

### 10.8 This firmware's catalogues, caps and limits

| Check | Path | Reason |
|---|---|---|
| A KNX binding's DPT is one this firmware encodes, and its value type fits the datapoint's (numeric types interchangeable, `doc/datapoints.md`); a time master's binding has 10.001, 11.001 or 19.001 by its member. | `knx.bindings[b].dptMain` | `unsupported: DPT 9.123 is not supported by this firmware`; `DPT 9.001 carries real values, the datapoint is boolean` |
| A Zigbee binding's cluster and attribute (or client cluster) are a function of this firmware's table whose value type fits the datapoint's, the command cluster is the function's or Level Control, the reporting resolves (`spec/zigbee.md` "Bindings", "Reporting overrides"). | `zigbee.bindings[z].cluster`, `.command`, `.reportMin`, ... | the texts of `spec/zigbee.md` |
| An external source's preset is one of this firmware's, a binding's variable one of the preset's. | `external.sources[s].preset`, `external.bindings[b].variable` | `unsupported: unknown preset 'meteo'`; `unsupported: open-meteo has no variable 'temp'` |
| A block's type exists in this firmware in the image's major version, at the image's minor version or newer. | `blocks[i].type` | `unknownType: org.homecontrol.room-controller 1.3 (this firmware has 1.0)` |
| The block's ports and parameters are at most the manifest's, each `id` the manifest's at that position. | `blocks.ports[p].id`, `blocks.parameters[q].id` | `unknownReference: port 'temprature' at position 0, the type has 'temperature'` |
| An output is bound to at most one datapoint and has a mirror; an input is bound to at most its `multiple`, has a mirror exactly when bound to more than one and then an aggregation the port allows (else 0); a required input is bound; the bound datapoints' types are accepted by the port (`spec/blocks.md` 5.3, 5.4). | `blocks.ports[p].…` | `forbidden: more datapoints than 'multiple'`; `missingInput: the required input 'temperature' is not bound`; `typeError: ...` (the texts of `spec/blocks.md` 11) |
| A parameter's value (where given, else the manifest's default) is valid for it, the type's `check()` accepts the values; a datapoint exactly where the manifest's parameter is adjustable. | `blocks.parameters[q].value` | `invalidParameter: ...` (the texts of `spec/blocks.md` 11) |
| What the rings and the archives' open slots need (3.7) is at most `historyBudget`, and that at most the device's cap; what the archives' segments need at most `archiveBudget`, and that at most the archive cap. | `image.historyBudget`, `image.archiveBudget` | the texts of 3.7 (`the history needs 70000 bytes, more than the budget of 65536`) |
| `tz` and `ntp` pass the settings' validators (as on the console). | `settings.tz`, `settings.ntp` | the validator's reason |
| The persistence keys of the rules and blocks do not collide in the device's store (`spec/rules.md` 10.1, `spec/blocks.md` 11). | `rules[i]`, `blocks[i]` | `limitExceeded: the persistence keys of 'a/x' and 'b/y' collide in the device's store` |
| The limits of section 5 and `spec/rules.md` 10.2: declared datapoints (owner 0, 1, 4, 7, and 2 with member 0) 1024; locations 256; functions 512; roles per function 16; schedules 128; rules 128; reactions per rule 16; triggers per reaction 8; actions per list 16; parameters per rule 8; timers 8; states 16; choices 16; log parts 8; plans per timeline 8, points per plan 16, per timeline 64, assignments 8, languages of a plan's label 4, points and plans of all timelines 256 and 64; blocks 32; Zigbee bindings 512; external sources 8 and bindings 32. | the count's record or section | `limitExceeded: 1100 declared datapoints, at most 1024` |

## 11. Versioning of the binary image

The header's `major.minor` (I4). This document is format **1.1**; the firmware reads exactly one
major version and every minor version up to its own.

- **Another major version** is refused from the header, before the slot is erased (`image.version`,
  10.1), naming both. A major version changes what existing fields mean or where they are.
- **A minor version only adds**, in four ways, each with the behaviour of an older firmware:
  1. *A section.* An older firmware skips an unknown section, unless its table entry says `required`:
     then it refuses the image with the hint (10.3). A section is required when ignoring it would
     change what the image does (a new kind of trigger table that the rules point into, as the strict
     members of I5 in the source); it is optional when it only adds what an older firmware may leave
     out (a display hint).
  2. *A longer record.* `entrySize` grows; an older firmware reads the fields it knows from each
     record's start and skips the rest. Fields added this way are ones an older firmware may ignore.
  3. *A code or a flag bit* in a known field (a new function, node kind, owner, value type). An older
     firmware refuses it with the hint (10.4), as the strict objects of the source did.
  4. *A meaning for a reserved field* whose 0 means what 1.0 means; an older firmware does not read it.
- **A newer firmware reads an older minor**: the sections and fields it lacks are absent or 0 with
  their 1.0 meaning.
- **E-Home compiles for the format the controller reports** (`format` in `GET /api/status` and the
  E-Home connection's `auth`): it writes at most that minor version and refuses, before sending, a
  project that needs more (`the controller reads format 1.0; this project needs 1.1 (<what>): update
  the firmware`). The refusals of an older firmware above meet only images deployed by hand.

## 12. Values across a swap

Before the slot is erased (section 6 step 2) the device copies into RAM what the new image may keep
(I8), keyed by a hash of ids, because the ids live in the slot. The **hash** is FNV-1a with 64 bits
over the id's UTF-8 bytes (offset basis `0xcbf29ce484222325`, prime `0x100000001b3`): the datapoint's
`name`; for a rule's timer or state the rule id and the timer or state id hashed each.

| What | Kept per entry | Matched in the new image |
|---|---|---|
| Datapoints of the image whose quality is not unknown (the implied ones too) | the hash of the id, the value (with its type), the quality, the update time | the datapoint with the same id hash and the same value type takes value, quality and update time (`spec/rules.md` 7.6 "Datapoint values") |
| Change tracking (`spec/rules.md` 7.7) | per tracked datapoint the id hash and the time of its last change | the same datapoint, if the new rules track it |
| Rules | the hash of the rule id; the hash of its current state's id; per running timer the hash of the timer id, its deadline and its start order; the diagnostics (`spec/rules.md` 9.1) | by rule id hash, then state and timers by their id hashes (`spec/rules.md` 7.6) |
| Blocks | the hash of the block id, of the type id, the major version, the status, the state record (`spec/blocks.md` 8.1) | by block id hash with type hash and major version (`spec/blocks.md` 8.1) |

The parameter overrides, a rule's persisted state, the inhabitant's timelines, the schedules'
records and the blocks' persisted records are in the store under their keys, not in the slot, and
follow as before (`spec/rules.md` 8.4, 8.5, 8.7, `spec/schedules.md` 4.4, `spec/blocks.md` 8). So do
the persisted values and the history rings, which section 6 step 2 writes by name before the erase.

**Collisions.** Two entries of one kind with the same hash are both dropped and logged
(`swap: 2 datapoints share the hash 0x1a2b…; their values are not carried`); a new datapoint takes at
most one entry. With 64 bits two different ids share a hash with a probability below 10⁻¹³ for a
thousand datapoints; a datapoint keeps its value only where the value type matches too.

**Size and lifetime.** The device may keep the old model's own value storage and add the hashes
rather than copy it. Logically an entry of a datapoint is 56 bytes (hash 8, update time 8, value 32,
quality and alignment 8): 11 KB for 200 datapoints with values, at most 56 KB for 1024. The table is
taken before the first byte of the new image is written and freed when an image has been applied; a
failed image does not free it (I9), so the last valid deployment that E-Home sends again finds the
values. A restart, `DELETE /api/image` and the factory reset drop it. At boot there is no table:
values come from the value store (`doc/persistence.md`).

## 13. Worked example: `minimal.json`

`spec/fixtures/images/minimal.json`,

```json
{"image": {"schemaVersion": "1.0", "projectId": "minimal", "revision": 0}}
```

compiles to 112 bytes (`spec/fixtures/images/minimal.bin`). It has the two required sections:
`image` and `strings`. The strings, sorted: the empty string at 0, `1.0` at 1, `minimal` at 5.

```
offset  bytes                                            field
0000    48 43 49 4d                                      header.magic "HCIM"
0004    01 00                                            header.major 1
0006    00 00                                            header.minor 0
0008    70 00 00 00                                      header.length 112
000c    42 2c 77 59                                      header.crc 0x59772c42
0010    00 00 00 00                                      header.flags: unsigned
0014    20 00                                            header.headerSize 32
0016    02 00                                            header.sectionCount 2
0018    00 00 00 00 00 00 00 00                          reserved
0020    01 00 00 00 40 00 00 00 20 00 00 00 20 00 00 00  sections[0]: id 0x01 image, flags 0, offset 0x40, length 32, entrySize 32
0030    02 00 00 00 60 00 00 00 0d 00 00 00 00 00 00 00  sections[1]: id 0x02 strings, flags 0, offset 0x60, length 13, entrySize 0
0040    05 00 00 00                                      image.projectId -> 5 "minimal"
0044    00 00 00 00                                      image.revision 0
0048    01 00 00 00                                      image.schemaVersion -> 1 "1.0"
004c    00 00 00 00                                      image.name: none
0050    00 00 00 00                                      image.generator: none
0054    00 00 00 00                                      image.generatedAt: none
0058    00 00 00 00                                      image.historyBudget 0 (no history)
005c    00 00 00 00                                      image.archiveBudget 0
0060    00                                               strings: "" at 0
0061    31 2e 30 00                                      "1.0" at 1
0065    6d 69 6e 69 6d 61 6c 00                          "minimal" at 5
006d    00 00 00                                         padding to 0x70
```

The whole file:

```
0000  48 43 49 4d 01 00 00 00 70 00 00 00 42 2c 77 59
0010  00 00 00 00 20 00 02 00 00 00 00 00 00 00 00 00
0020  01 00 00 00 40 00 00 00 20 00 00 00 20 00 00 00
0030  02 00 00 00 60 00 00 00 0d 00 00 00 00 00 00 00
0040  05 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00
0050  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0060  00 31 2e 30 00 6d 69 6e 69 6d 61 6c 00 00 00 00
```

The CRC-32 is that of these 112 bytes with bytes 12..15 zero (zlib's `crc32()`: `0x59772c42`).

**Signed** with the test key "installation" of `spec/fixtures/signatures/vectors.json` (fingerprint
`ce7d 4a41 a476 9ee3`; TEST KEY ONLY), the same image is 176 bytes
(`spec/fixtures/images/minimal.signed.bin`): `header.length` `b0 00 00 00` (176), `header.flags`
`01 00 00 00`, and so `header.crc` `87 ff f6 f6` (0xf6f6ff87, over the 112 bytes with these two
fields); bytes 0x10..0x6f otherwise as above; then the signature at 0x70:

```
SHA-256 of [0, 112)  c5e66fdbb811651835cd1b864e30e99b4b3a6070f51e8949d39c35807d3a79d8
M                    686f6d65636f6e74726f6c2d696d6167652d7632 || that digest
signature            d839e14f50d34bd397ef9d572dd7af3ea83fd8ce24a84ebb28eeed99b41e222d
                     903504e9072e01ac4f865c393cb34283fd39a2ef6e21aa0fd67be4bf920f2806
```

```
0000  48 43 49 4d 01 00 00 00 b0 00 00 00 87 ff f6 f6
0010  01 00 00 00 20 00 02 00 00 00 00 00 00 00 00 00
0020  01 00 00 00 40 00 00 00 20 00 00 00 20 00 00 00
0030  02 00 00 00 60 00 00 00 0d 00 00 00 00 00 00 00
0040  05 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00
0050  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0060  00 31 2e 30 00 6d 69 6e 69 6d 61 6c 00 00 00 00
0070  d8 39 e1 4f 50 d3 4b d3 97 ef 9d 57 2d d7 af 3e
0080  a8 3f d8 ce 24 a8 4e bb 28 ee ed 99 b4 1e 22 2d
0090  90 35 04 e9 07 2e 01 ac 4f 86 5c 39 3c b3 42 83
00a0  fd 39 a2 ef 6e 21 aa 0f d6 7b e4 bf 92 0f 28 06
```

**Mutations** (the first entries of the device's mutation corpus, `spec/fixtures/images/binary/`;
each with the CRC recomputed unless the row is about the CRC):

| Mutation of `minimal.bin` | Code | Path | Reason |
|---|---|---|---|
| Cut to 100 bytes | `image.format` | `header.length` | `the header says 112 bytes, the body has 100` |
| The source text `minimal.json` itself | `image.format` | `header.magic` | `not a runtime image: the magic is 7B 22 69 6D, not 48 43 49 4D ("HCIM"): a JSON text (E-Home compiles the image; send the binary form)` |
| `header.major` 2 | `image.version` | `header.major` | `format 2.0: this firmware reads format 1 (up to 1.1)` |
| Byte 0x66 changed, CRC kept | `image.damaged` | `header.crc` | `the CRC-32 is 59772c42, the bytes give …: the image was damaged on the way or in the slot` |
| Sent to a device that holds the installation key | `image.unsigned` | `header.flags` | `the image is not signed; …` |
| `minimal.signed.bin` to a device holding the "rotated" key | `image.bad_signature` | `signature` | `the signature does not verify under the installation key 90ae 3702 f3a9 63ab` |
| `sections[1].offset` 0x61 | `image.invalid` | `sections[1].offset` | `malformed: offset 0x61 is not a multiple of 8` |
| `sections[1].length` 12 (the last NUL outside) | `image.invalid` | `strings` | `malformed: the last byte is not a NUL` |
| `sections[0].entrySize` 28 | `image.invalid` | `sections[0].entrySize` | `malformed: entry size 28 for image, which needs at least 32` |
| `sections[0].length` 64, `entrySize` 32 (two records; the strings moved behind) | `image.invalid` | `sections[0].length` | `malformed: the section image holds 2 records, it has one` |
| `sections[1].id` 0x01 | `image.invalid` | `sections[1].id` | `malformed: section 0x01 twice` |
| `sections[1].id` 0x61, `flags` 1 | `image.invalid` | `sections[1].id` | `malformed: section 0x61 is required, but this firmware does not know it` |
| `sections[1].id` 0x61, `flags` 0 | `image.invalid` | `sections` | `malformed: the section strings (0x02) is missing` (0x61 skipped) |
| `image.projectId` 6 | `image.invalid` | `image[0].projectId` | `malformed: 6 is no string (not the start of one of the 13 bytes of strings)` |
| `image.projectId` 0 | `image.invalid` | `image[0].projectId` | `malformed: empty, it is required` |
| `header.minor` 1 and a third section 0x61 of 8 bytes, not required | — | — | accepted: the unknown section is skipped |

### 13.1 The compiled corpora

The device's tests read no JSON image. E-Home's compiler makes every binary image of the corpora,
the files are committed here, and E-Home's test compiles every source again and compares byte for
byte (issue #87, haumacher/ehome#176). A change of the format or the compiler regenerates them in one
commit with the change.

**Images** (`spec/fixtures/images/`). Beside each source `<name>.json` its image `<name>.bin`
(unsigned), and `minimal.signed.bin` (13). The cases `cases/accept-*.json` get
`cases/accept-<name>.bin`; the cases `cases/refuse-*.json` get none: they are E-Home's validation
(section 4), and the device never sees them. The device's own refusals have the mutation corpus
`spec/fixtures/images/binary/`: each mutation a file `<name>.bin` with `<name>.json` beside it giving
`{"from": "<the image it mutates>", "mutation": "<what was changed, in words>", "code": "<code>",
"path": "<path>", "reason": "<reason>"}` (`"code": null` for an image that must load), the table
above its first entries; a mutation whose outcome depends on the device's key has the member
`"deviceKey": "installation"` (the device holds the test installation key) or `"rotated"` (it holds
another key). `images/test-system.template.json` is a template, not an image: it gets no `.bin`. `images/anonymous-pages.json`
(format 1.1, homecontrol #91) has the anonymous pages of 3.9: its `.bin` was first written by the
device's test compiler (`core/test/image_compiler.cpp`), until E-Home's compiler writes the
sections (haumacher/ehome#184) and the two are compared byte for byte as for every other image.

Readings fixed while writing the compiler (haumacher/ehome#176):
- A node without a value type (a timer node) carries type code 0 (10.6).
- A schedule given in a rule case by its id alone gets its id as its `label`.
- A limit the device checks but E-Home's own validation refuses first (rule case 97: 17 points in a
  plan) gets no `.bin`: E-Home cannot produce it; the device's limit is covered by the mutation
  corpus where a mutation can express it.

**Rule and block cases** (`spec/fixtures/rules/`, `spec/fixtures/blocks/`, their `README.md`). A
case `NN-<name>.json` stays the test's script; its image is compiled into `NN-<name>.bin` beside it,
and the image of each `image` step of its `timeline` into `NN-<name>.step-<k>.bin`, `k` the step's
0-based position in `timeline`. A case is mapped to the source of an image as follows; the compiler
then compiles that source like any other (section 9):

| Case | Source member of the image |
|---|---|
| (the file name) | `image`: `schemaVersion` `"1.7"`, `projectId` the file name without `.json` (`05-timer-presence-light`), `revision` 0 for the case's image and the step's position plus 1 for a step's image |
| `datapoints` (`{"name", "type"}`) | `datapoints`, in the case's order: `id` the name, `type` the type; `writable` and `transmit` unless the case says `"writable": false`; an `"application": true` datapoint gets `virtual` (owner none, the application flag); every other one has no technology member and no application flag: owner none without the application flag, which no real image has (E-Home gives every datapoint without a technology the flag), marks the harness's test technology |
| `schedules` as ids or `{"id", "output"}` | `schedules`: per id an entry of kind `once` at 1970-01-01 00:00:00, not enabled and not adjustable (it never fires by itself; the case's `schedule` steps fire it), with the `output` where the case names one |
| `schedules` as full entries | `schedules`, as they are |
| `rules` | `rules`, as they are (the parameter, state and schedule time datapoints follow as in 3.3) |
| `blocks` | `blocks`, as they are (their mirrors follow, 3.3) |
| an `image` step | the image of the step: the case's `datapoints` with the step's `rules`, `blocks` and `schedules` (a step without `schedules` keeps the schedules of the image before it; a block case's step without `rules` or `blocks` has none) |

Everything else of a case is not part of an image and stays in the case's JSON, which the harness
reads itself: `title`, `description`, `note`; the datapoints' harness properties `refuseCommands`,
`refuseReads` and `"readable": false` (by datapoint name; `readable` is not the image's flag
`readable` but whether a technology can be asked, which E-Home's validation uses for `notReadable`);
`clock` (the wall clock and its time zone, set on the harness's time service, not the image's
`settings.tz`), `location` (the controller's own setting, not `settings.location`), `astro`, `ui`
(the setting `ui.lang`), `persisted` (loaded into the parameter store before the boot), `initial`
(the values at time 0), every step of `timeline` (the image steps by their `.bin`), `end` and
`expected`.

A case whose `expected` is `loadRefused` gets a `.bin` only when the refusal is one the device makes
(section 10.8: an unknown block type, a block's ports and parameters against the type's manifest
(`missingInput`, `typeError`, `invalidParameter`, `forbidden` for too many datapoints), a DPT, a
limit): the compiler's corpus mode leaves out its own check of that point and compiles the image, the
harness loads it and expects the refusal with the case's reason code (the path is the binary one, the
text the device's). Every other `loadRefused` case is a semantic refusal of E-Home's validation: it
has no `.bin`, the device's harness skips it, and E-Home's test checks the refusal with the case's
`path` and `text`.
