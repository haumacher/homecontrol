# Zigbee coordinator

How the controller runs its own Zigbee network (issue #15): the ESP32-C6's 802.15.4 radio as the
coordinator, Espressif's Zigbee SDK in a task of its own, and the Zigbee adapter that interviews
joining devices, keeps them in a device database, maps their clusters to datapoints
(`doc/datapoints.md`), configures their reporting and marks a silent device stale. The adapter is
portable core (namespace `hc::zigbee`), tested under the fake clock against a fake driver; the
platform only moves requests and frames between the stack and the event loop.

| Piece | Where |
|---|---|
| IEEE addresses, ZCL constants, ZCL value decoding | `core/include/hc/zigbee/zcl.h` |
| The device database (JSON in the `BlobStore`) | `core/include/hc/zigbee/device_database.h` |
| The cluster mapping: functions, datapoint names and values, overrides | `core/include/hc/zigbee/cluster_mapping.h` |
| The adapter (`Technology`): interview, reporting, commands, stale rule | `core/include/hc/zigbee/adapter.h` |
| The driver interface (requests to the network) | `core/include/hc/platform/zigbee_driver.h` |
| ESP: stack task, driver, event ring, `ZigbeeService`, the `zigbee` console command, the NVS `BlobStore` of the move out of `zb_db` | `platform/esp/components/hc_zigbee` |
| Stack configuration | `platform/esp/sdkconfig.defaults.app.esp32c6`, menu "HomeControl Zigbee" |
| Tests | `core/test/zigbee_*_test.cpp` |

The image format of the bindings and the JSON of the database and the adapter are in
`spec/zigbee.md`.

## Decision: esp-zigbee-lib 1.6 (ZBOSS) on IDF 5.5

Espressif ships two lines of its Zigbee SDK. The 2.x line (Espressif's own stack) recommends IDF
5.5.4; the 1.x line is the long-term-support line on the ZBOSS stack, recommends IDF 5.3.2 and
declares `idf >= 5.0`. The project took it on IDF 5.3.0 and keeps it on IDF 5.5.6 (the upgrade of
2026-11-03, ROADMAP.md), so it takes `espressif/esp-zigbee-lib ~1.6.8`
with `espressif/esp-zboss-lib ~1.6.4` (`main/idf_component.yml`, only for the targets esp32c6 and
esp32h2, pinned in `dependencies.lock.esp32c6`). It builds and links on 5.3.0 and 5.5.6. Moving to 2.x is
possible on IDF 5.5 now, but is a decision of its own; the core does not depend on the SDK, only `hc_zigbee` does.

The SDK runs as coordinator (`CONFIG_ZB_ZCZR`) on the native radio (`CONFIG_ZB_RADIO_NATIVE`).
Green Power stays enabled (the SDK's default for a coordinator: the Green Power proxy belongs to
Zigbee 3.0); Green Power devices themselves are not supported.

## Bring-up

`ZigbeeService::init()` (after the WiFi driver) checks that the partitions `zb_storage` and `zb_fct`
exist, starts the adapter with the device database in the blob store behind the write queue (moved
there once from the `zb_db` partition of the firmware before, see "Partitions") and the task
`hc_zigbee`. The task configures the stack: coordinator with `HC_ZIGBEE_MAX_CHILDREN` children, the
channel mask `HC_ZIGBEE_CHANNEL_MASK`, the PAN id `HC_ZIGBEE_PAN_ID` (0: the stack picks one), the
transmit power `HC_ZIGBEE_TX_POWER`, the stack's tables for a network of 32 nodes (the device
database's `kMaxDevices`; `esp_zb_overall_network_size_set`, 64 by default) and 48 packet buffers
(`esp_zb_io_buffer_size_set`, 80 by default; both before `esp_zb_init`, issue #45; the 802.15.4
driver keeps 10 received frames, `CONFIG_IEEE802154_RX_BUFFER_SIZE`), and endpoint 1 as an HA home
gateway:

- servers: Basic, Identify, On/Off and Level Control (the latter two receive the buttons' commands);
- clients: Identify, On/Off, Level Control, Color Control, Power Configuration, IAS Zone,
  Temperature, Humidity, Illuminance, Occupancy, Multistate Input.

The buttons' On/Off commands (0x00–0x02, 0x40–0x42) and Level Control commands (0x01–0x07) are
registered as privileged: the stack hands them to the application instead of switching its own
On/Off server.

On the first start the stack is factory new and forms the network; the stack keeps the network (PAN
id, channel, extended PAN id, network key, the children) in `zb_storage`, so every later start
restores the same network ("network restored" in the log). Formation and initialization are retried
every second when they fail. The network is not opened at start: joining needs `zigbee permit`.

Until the network is up every request of the adapter is refused (the driver does not take the
stack's lock before that).

### Channels and coexistence with WiFi

The ESP32-C6 has one 2.4 GHz radio front end for WiFi and 802.15.4; the IDF's software coexistence
(`CONFIG_ESP_COEX_SW_COEXIST_ENABLE`, enabled in `sdkconfig.defaults.app.esp32c6`) arbitrates
between them, and the service calls `esp_coex_wifi_i154_enable()` when the stack starts.

The default channel mask 0x02108000 allows Zigbee channels 15, 20 and 25, the ones that fall between
or beside the usual WiFi channels 1, 6 and 11 (channel 15 sits at 2425 MHz, between WiFi 1 and 6;
20 at 2450 MHz, between 6 and 11; 25 at 2475 MHz, above 11). The stack picks the quietest of them at
formation. A changed mask only affects a new formation (after an erase of `zb_storage`).

**Power save.** Espressif's gateway example sets WiFi to `WIFI_PS_MIN_MODEM` with coexistence: the
802.15.4 radio then also gets the air while WiFi sleeps. The setting `wifi.ps` (doc/network.md)
chooses: `min` (the default since 2026-10-18) or `none` (lowest latency of the web UI and KNX, for
a household without Zigbee devices); the coordinator logs a warning when it finds power save off at
network start, naming the setting. Measured on the C6 over the home LAN (60 requests each, the
board idle with KNX, the web server and the coordinator up): see the table below.

With a paired device (#15 stage two, a Philips Hue motion sensor SML002) the Zigbee side was
decisive: with `none` the sensor joined and answered the interview, then three of six bindings and
every read timed out, it announced itself again twice (rejoins after lost polls to its parent) and
within a minute its LED flashed red on motion (its "no network" signal); nothing it sent reached the
coordinator. With `min` the same sensor joined, answered every request, and its reports (occupancy
on motion, temperature, illuminance, battery) arrived. The KNX status reply after a command measured
217 to 357 ms under `min` in that run (four commands), not the stall of the earlier run, so the
default moved to `min`.

| `wifi.ps` | ICMP ping avg / max | `GET /api/auth/session` median / p90 / max | KNX status reply after a command |
|---|---|---|---|
| `none` | 19 to 27 ms / 70 ms | 69 to 79 ms / 84 ms / 87 ms | 16 to 200 ms |
| `min` | 62 to 768 ms / 3979 ms | 83 ms / 1220 to 1450 ms / 3003 ms | not received within 6 s (two of two runs) |
| `max` | 65 ms / 151 ms | 81 ms / 190 ms / 2194 ms | 311 ms |

Measured 2026-10-18 on the C6 with firmware of that day against a consumer access point (the
numbers depend on its DTIM period and on the coexistence arbiter): in that run `min` stalled the
link for seconds at a time and the actuator's status telegram did not reach the tunnel within the
window; the later run with the paired sensor (above) saw 217 to 357 ms. The stalls are a cost of the
access point's beacon timing; the lost Zigbee device is a loss of function, so `min` is the default
and `none` the per-household choice, with both named on the Settings page.

### Partitions

The SDK finds its storage by fixed labels: `zb_storage` (16 KB, the stack's NVRAM: network, keys,
children, bindings) and `zb_fct` (4 KB, factory data: install codes; unused but required).

**The device database** is the blob `zigbee-devices.json` of the LittleFS blob store (`/storage/state`,
doc/persistence.md), written through the write queue (homecontrol #111): a save hands the database to
the queue as the blob's source (`BlobStore::write_from()`), the queue asks it for the JSON on the loop
when its worker `hc_flash` is about to write, into one buffer of exactly the JSON's size, which lives
until the worker wrote it. So the loop never waits for the flash, the JSON (about 15 KB for 30
devices with the longest names, more than the queue's 8 KB arena) never goes into the arena and
never falls back to a write on the loop, saves while one is written wait as one entry without a
copy, and a failed write is retried with the content of its retry, without a buffer in between. A
load allocates one buffer of the blob, checks the text in a first pass and parses it into the table
in a second (no temporary table): no block larger than the JSON is needed.

Why not NVS on the worker: NVS takes one lock for all its partitions (`nvs::Lock`, ESP-IDF 5.5), so a
write of `zb_db` on the worker would make the loop's settings reads and writes wait for it; and
NVS writes the new version of a blob before it erases the old one, so a database above about 10 KB
(5 usable pages of 4000 bytes of `zb_db`'s 6, two copies) cannot be replaced at all, about 20
devices with the longest names. On LittleFS the write spreads over the storage partition's free blocks instead of six
sectors (the wear budget: doc/persistence.md).

**`zb_db`** (24 KB, NVS) held the database before homecontrol #111 (two NVS keys named by the hash of
the blob's name: `n<hash>` the name, `d<hash>` the content; firmware before issue #44 kept both in
one blob `b<hash>`, still read). At every start `ZigbeeService` opens it for the adapter's start, and
the database moves its blob once (`DeviceDatabase::set_legacy()`, before anything is written through
the queue): the old blob is read and loaded, written to the blob store directly, read back and
compared, and only then removed from `zb_db` (`hc.zigbee: device database moved to the blob store
(<bytes> B, <n> device(s))`). A failure keeps it in `zb_db` with a warning (`device database not
moved to the blob store (<reason>): the previous store keeps it and it is loaded from there; moved at
the next start`); the devices are loaded from it all the same, and the saves go to the blob store. A
blob in both stores is the blob store's (a save after a failed move): it is loaded, and the old one
removed after it loaded; one that does not load leaves the old one loaded and kept. Then the
partition is closed and deinitialized, which takes NVS's index of it off the heap. `factory reset
yes` still erases `zb_db`. The three partitions (44 KB) sit at the end of the flash
(`tools/gen_partitions.py`); the `storage` partition gives up the room, on the 4 MB flash from 312
to 268 KB. They exist on every flash size and target (the ESP32-S3 simply does not use them).

An OTA update does not change the partition table: a device updated over the air from a firmware
without these partitions keeps its old table, and `zigbee status` says "the partitions
zb_storage/zb_fct are missing". It needs one serial flash (`idf.py -B build-esp32c6 flash`, which
writes the partition table and `storage.bin` too).

## Joining a device

```
zigbee permit 120          # opens the network for 120 s (at most 254; 0 closes it)
```

then reset the device into pairing mode (a bulb: switch it off and on five or six times; a sensor
or button: hold its reset button until its LED blinks). The stack accepts it with the well-known
Trust Center link key and sends it the network key; the device announces itself and the adapter
interviews it. The trust center does not require the link key exchange after the join: legacy
devices of the Light Link and Home Automation 1.2 profiles (the Philips Hue motion sensor SML002 is
one) never request a key, and with the Zigbee 3.0 default the stack removed such a device 22 s
after the join, which sent it through leave and rejoin until it gave up. Zigbee 3.0 devices still
exchange the key. A legacy device has no link key at the trust center from 15 s after its join on;
the adapter gives it one again so that it can come back by a trust-center rejoin ("Rejoin policy"
below). The interview:

1. **Active endpoints** (ZDO), then the **simple descriptor** of every endpoint (profile, device
   type, input and output clusters; at most 4 endpoints and 12 clusters each are kept, the known
   clusters first).
2. **Basic cluster**: manufacturer name and model identifier.
3. **IAS Zone** endpoints: the zone type (it names the datapoint: contact, motion, smoke, water,
   else alarm), then the enrollment: the coordinator writes its IEEE address into the zone's CIE
   address, answers the zone's Zone Enroll Request and receives its status changes.

Each step has 10 s and 3 attempts. A device whose endpoints cannot be learnt stays uninterviewed and
is tried again when it is heard after a minute. One device is interviewed at a time; others wait
their turn.

**Incomplete interviews** (issue #47). A battery device answers the active endpoint request and may
fall asleep before the next request reaches it (seen with the Hue SML002: endpoint 2, the one with
the sensors, was skipped and the device counted as interviewed with endpoint 1 alone, for good). An
endpoint whose simple descriptor stays unanswered, or a Basic cluster that does, therefore ends the
interview *incomplete*, not done:

- What answered is kept and used: the database stores the endpoints learnt, their datapoints are
  loaded, read, bound and their reporting configured as after a finished interview. The device is
  **not** `interviewed`; it records what is missing (`missing`: the endpoints that did not answer,
  `names` when manufacturer and model did not), persisted in the database, so it is incomplete
  again after a restart. A re-interview of a device that knew more keeps the earlier descriptor of
  an endpoint that does not answer now (and the earlier names), but the device is incomplete.
- Coordinator bindings, reporting configuration, the reads and the stale timeout follow the
  endpoints the device described, never the image's knowledge alone: an image binding of a function
  on an endpoint not described yet is a datapoint (unknown until the device reports it) but gets no
  request until an interview learns the endpoint, so the short time a sleepy device listens is not
  spent on requests the retry repeats. The same holds for an interviewed device whose binding names
  an endpoint it does not have.
- The log says `interview of <ieee> incomplete (endpoint 2 did not answer)`, the WebSocket sends the
  `zigbee` event `interview_incomplete`, `GET /api/zigbee` shows `missing` and the English reason
  `incomplete`, the export `missing`, and the Zigbee page the state "Interview incomplete" with the
  reason in the UI's language.
- **Retry.** The device's own next frame (a report, a command, an announce: anything that proves it
  is awake, which is when a sleepy device answers) starts a retry, at most once per
  `kInterviewRetryMs` (60 s) counted from the end of the incomplete interview, so a device that never
  answers costs one short request round per minute while it is active, and nothing while it is
  silent. The retry asks only for what is missing (the missing descriptors, the Basic cluster only
  when the names are missing, the zone types of the endpoints it learns) so that it fits the short
  time a sleepy device listens after its frame; it logs `<device> heard again: retrying its
  incomplete interview (endpoint 2 did not answer)`. When everything answers, the device is
  `interviewed` and the model is rebuilt with the new endpoints' datapoints (the others keep their
  values). Once the retry delay has passed, a restart retries with the first frame.
- A fresh join (factory reset), a rejoin after a leave and `zigbee interview` / "Interview again"
  interview the device fully, as before.
- **The IAS zone type stays as before**: a zone type that does not answer names the zone "alarm" and
  does not make the interview incomplete. The zone type only chooses the generic datapoint's name
  (contact, motion, ...): counting it as missing would rename the datapoint when a retry reads it,
  under rules and E-Home bindings that already use the name; the zone's notifications work either
  way, and an image binding names the point independently of the type. `zigbee interview` reads it
  when wanted.

After the interview the adapter writes the database, loads the device's datapoints, binds each
mapped server cluster to the coordinator's endpoint 1 (bindings for the buttons' client clusters,
none for IAS Zone, which reports to its CIE address), configures the reporting of each mapped
attribute (table below), and reads the current values. Whether the device took each configuration,
and whether it keeps its promise, is watched ("Reporting watch" below).

A known device that joins afresh (the trust center reports an unsecured join, not a rejoin) was
factory-reset: its bindings and reporting configuration are gone, so the adapter interviews it again
with the announce that follows, while a battery device still listens (a sleepy device answers
requests only right after its join or when it polls; a manual `zigbee interview` of a resting Hue
sensor timed out three times). A rejoin after a power cycle changes nothing; the reports continue.

**Engineering the paired devices.** Pairing happens where the devices are; the wiring happens in
E-Home. The Zigbee page's "Export device list" downloads `zigbee-devices-<controller>.json`
(`GET /api/zigbee/export`, the format in `spec/zigbee.md` "Export"): every device with its
interview result and the functions the mapping derives. E-Home imports it, names and places the
devices, binds their functions to datapoints and ships the result as the runtime image's Zigbee
bindings; an E-Home on the same network does the same directly over the API (#16).

`zigbee devices` lists the devices with their datapoints and values. A device that leaves (without
rejoin) is kept in the database but marked as left; `zigbee remove <ieee>` asks it to leave and
deletes it. `zigbee identify <ieee>` makes it blink for 10 s, `zigbee label <ieee> <text>` names it,
`zigbee interview <ieee>` interviews it again (after a firmware update changed its endpoints).

## Datapoints

Every endpoint function becomes one datapoint named `zigbee.<ieee>.<endpoint>.<suffix>` (the IEEE
address in 16 lower-case hex digits), unless the runtime image binds the device's functions to
names of its own: a device with bindings gets exactly its bound datapoints (`spec/zigbee.md`). The
functions:

| Function | Cluster / attribute | Datapoint | Value | Reporting min..max s / change | Command |
|---|---|---|---|---|---|
| `on_off` | On/Off 0x0006 / 0x0000 | `.on` | boolean | 0..300 / – | Off, On (0x0006) |
| `level` | Level Control 0x0008 / 0x0000 | `.level` | real % = level × 100 / 254, one decimal; 0xFF invalid | 1..300 / 1 | Move to Level with On/Off, 0.5 s |
| `color_temperature` | Color Control 0x0300 / 0x0007 | `.color_temperature` | unsigned K = 1 000 000 / mireds; 0 and 0xFFFF invalid | 1..300 / 10 mireds | Move to Color Temperature, 0.5 s |
| `temperature` | Temperature Measurement 0x0402 / 0x0000 | `.temperature` | real °C = value / 100; 0x8000 invalid | 30..600 / 0.1 °C | – |
| `humidity` | Relative Humidity 0x0405 / 0x0000 | `.humidity` | real % = value / 100 | 30..600 / 1 % | – |
| `illuminance` | Illuminance Measurement 0x0400 / 0x0000 | `.illuminance` | real lux = 10^((value − 1) / 10000) | 10..600 / 500 | – |
| `occupancy` | Occupancy Sensing 0x0406 / 0x0000 | `.occupancy` | boolean (bit 0) | 0..600 / – | – |
| `ias_zone` | IAS Zone 0x0500 / 0x0002 | `.contact`, `.motion`, `.smoke`, `.water`, `.alarm` | boolean (bit 0, alarm 1) | status change notifications | – |
| `battery` | Power Configuration 0x0001 / 0x0021 | `.battery` | real % = value / 2; above 200 invalid | 3600..21600 / 1 % | – |
| `multistate` | Multistate Input 0x0012 / 0x0055 | `.multistate` | unsigned | – (read and reports the device sends) | – |
| `action` | client On/Off 0x0006 and Level Control 0x0008 commands | `.action` | enumeration: 0 off, 1 on, 2 toggle, 3 dim up, 4 dim down, 5 dim stop | – (commands) | – |

The reporting column holds the defaults the adapter configures after the interview (the change in
the attribute's raw units where the datapoint's unit has no fixed step: 10 mireds, illuminance 500).
A binding of the runtime image may override them per function (`reporting`: `min`, `max` and the
change in the datapoint's unit, `spec/zigbee.md` "Reporting overrides"; E-Home is the place to tune,
haumacher/ehome #64): the adapter keeps the defaults and the raw scales (`resolve_reporting()`,
`change_to_raw()` in `cluster_mapping.h`), every point carries its effective reporting, and the
export (`GET /api/zigbee/export`) shows the effective values per function. An image whose bindings
change a function's effective reporting configures it again on the interviewed devices concerned
when it is applied (not at the start: the devices keep what they were told); a device that misses
the request keeps its previous configuration until the next change or interview.

`on_off` is also commandable through Level Control (a dimmer without the On/Off cluster takes Move
to Level with On/Off). The writable datapoints (`.on`, `.level`, `.color_temperature`) carry the
flags writable and transmit; the server functions except `multistate` are read at start.

**Commands before the network is up** (homecontrol #72, spec/rules.md 7.3 step 5). Zigbee had the
gap of the KNX tunnel: the stack takes requests only once it has formed or restored the network a
few seconds after the boot (`ZigbeeService`'s `ready`), so the start cascade's writes to a Zigbee
datapoint were refused by the driver and lost. Now, while the adapter's network is not up
(`network_state()`), a command of a joined device's commandable point is checked as if it were sent
(level 0..100 %, colour temperature, a function with a command) and held: the model stores it and
marks the datapoint pending, a later command replacing it. The first held command logs `holding
commands until the Zigbee network is up (first: '<name>')`. When the network comes up the adapter
logs `sending <n> held command(s)` and sends them once, in datapoint order, one every
`kHeldSpacingMs` (100 ms; the first at once), each logged `sent the held command <value> for
'<name>'`; a device that is no longer joined then drops its command (logged). A report of the
datapoint before its command went out drops it (the device is newer). A command to a device that
has not joined, or out of range, is refused as before; while the network is up a command goes to
the driver at once, also unchanged. A reload of the datapoints (a device interviewed or removed)
keeps the mark with the value, by name.

### The model

On the device the adapter is a provider of dynamic datapoints of the image service (issue #29,
`doc/image.md` "Dynamic datapoints"): the service composes the model from the runtime image's
datapoints followed by the generic datapoints of the joined devices the image does not bind.

- **Bindings.** A datapoint of the image with a `zigbee` member (`spec/runtime-image.md` 3.3,
  `spec/zigbee.md`) stands for one function of one device endpoint. The loader checks it; at apply
  the service hands the bindings to the adapter (`ImageBindings`), which binds those datapoints by
  name. A device with any binding gets only its bound datapoints; they exist (unknown) before the
  device joins. Rules, functions and the UI refer to Zigbee datapoints through these names: the
  generic names are not in the image, so a rule cannot name them.
- **Joins and removals.** When an interview finishes or a device is removed, the adapter compares
  the generic datapoints it would state with those in the model and notifies the service when they
  differ (a label does not change them; a bound device's join does not either). The service reloads
  the model within a second (one reload per second at most) with the values kept, binds every
  component again and leaves the rules running. Reporting and the coordinator bindings are set up
  from the device's mapping right after the interview, before the reload; the device's values are
  read once its datapoints are in the model.
- **No image.** Without an image the model holds the Zigbee datapoints alone, so the first pairing
  on a fresh device works before E-Home deploys anything; removing the image keeps them (with their
  values) and brings back the generic datapoints of devices it had bound.
- **Duplicates.** An image datapoint named like a generic one (`zigbee.<ieee>.<endpoint>.<suffix>`)
  refuses the image (`datapoints[3].id: '...' is also a datapoint of the provider 'zigbee'`); a
  device that joins later with such a name leaves the model as it is (logged) until the image
  changes.

Alone (the adapter without an image service: the host tests, a firmware built without images) the
adapter loads the model itself with the Zigbee datapoints and reloads it when a device is
interviewed or removed; the values survive a reload. While the model holds datapoints of another
owner that load is refused ("the model holds datapoints of another owner").

## Stale rule

A device that has configured reporting must be heard within three times the shortest maximum
reporting interval of its datapoints (`kStaleFactor`), the effective ones (a binding's override
over the default). Only a promise the device confirmed counts (homecontrol #117): once it answered
one of its Configure Reporting requests with success in this boot, a point whose configuration it
did not confirm (not answered yet, timed out) sets no timeout; while it has confirmed none, every
point counts that it did not refuse (it keeps what it was configured with before the start, and a
device that is gone, silent since the start, must still turn stale); a refused configuration never
counts. The house controller (2026-11-22) marked the SML002 `silent for 1800 s (3 reporting
intervals)` from the 600 s of occupancy, whose configuration had timed out again and again. Every frame of the device counts as heard, so the most frequent promise is the
one to check (homecontrol #98; before, the largest interval counted, and the Hue motion sensor
SML002, whose battery reports at most every 21600 s, was stale only after 18 hours although its
temperature, illuminance and occupancy promise a report every 600 s): a bulb (max 300 s) within 15
minutes, a temperature sensor or the SML002 (max 600 s) within 30 minutes, a device whose only
periodic report is the battery (max 21600 s) within 18 hours. The adapter checks every 10 s; the
silence counts from the controller's start for a device not heard since (a persisted value restored
at boot is good until then, homecontrol #100; a read on start replaces it). A stale device's
datapoints get the quality stale; its next frame ends it and its values are read again. Devices
without configured reporting (pure buttons, IAS zones) never go stale. A function whose override
turns the periodic report off promises no report and does not count: `max` 65535 (ZCL: the device
issues no reports of the attribute) and `max` 0 (ZCL: reports on a change only). A device whose
every function has it off never goes stale, like a pure button.

## Reporting watch (homecontrol #108)

The house controller (2026-11-22): after a restart the Hue SML002's illuminance, occupancy and
battery had values within minutes, its temperature (max 600 s) only after 52 minutes, although the
sensor sent other frames throughout. The configuration went out once, at the interview, and nothing
checked it afterwards; the reads on start went out before the network was up (the stack refuses
every request then) or to a sleeping device.

- **State per point.** Each loaded point has a watch of this boot (`PointWatch`, 8 bytes in a table
  beside the point table, allocated with it: nothing in the device record, nothing persisted; its
  times are 16-bit ticks of 10 s, so `last_report_ago_s` comes in steps of 10 s): `requested` when the configuration went out, `configured` or
  `refused` (with the ZCL status) by the device's Configure Reporting Response (the ESP driver hands
  the response's status and, for a refusal, the attribute it names), `timed_out` when no answer came
  within `kReportingAnswerMs` (30 s; a sleepy device's request waits in its parent's queue for its
  next poll, about 7.7 s), `not_requested` while it has not gone out since the start. A refusal is a
  warning naming the device, function, endpoint, cluster, attribute and status; a timeout an info
  line; a late answer still counts. The watch also keeps the time of the point's last *report* (an
  attribute report; the answer to a read does not count). `GET /api/zigbee` shows them per device as
  `points` (`reporting`, `status`, `last_report_ago_s`, `repeats`, `silent`; spec/zigbee.md "Adapter
  view"), the Zigbee page beside each value ("reporting configured, last report 3 min ago"), with the
  badge "Reporting" for a refused or silent point.
- **Re-send after a silence.** A point with a periodic maximum that has not reported for (2 +
  repeats) × its maximum interval, counted from its last report or the last sending of its
  configuration, has its binding and configuration sent again — right after a frame of its device
  (`kWatchDelayMs`, 500 ms, after the frame, so that the frame's own reports count first): the device
  is awake then and its parent delivers the request at its next poll. One device at a time: another
  device's due point waits until the answers of the one before came or timed out (it is checked again
  at its own next frame). At most `kWatchRepeats` (3) re-sends until the point reports; each is an
  info line `<device>: temperature on endpoint 2 (cluster 0x0402, attribute 0x0000) has not reported
  for 1200 s (max 600 s) while the device is heard: binding and reporting configuration sent again (1
  of 3)`. A point still silent after the third is a warning (`... has not reported for 3000 s
  although the device is heard, after 3 repeats of its reporting configuration (max 600 s): its value
  comes only from reads`), `silent` in the view, and is not sent again until it reports (`reports
  again`) or the device is interviewed. A refused configuration is not sent again by the watch (the
  device would refuse it again); a new interview does. A device that is not heard at all goes stale
  (above) instead: the watch only acts on frames.
- **Configuration and reads on start.** The start (once the network is up: the stack takes no request
  before it, and until #108 the start's reads went out at once and were refused) reads each device's attributes and
  sends the reporting configuration of every point not configured since the start, one device every
  `kRefreshSpacingMs`. So after a restart every point's state is known again, and a device that lost
  its configuration gets it back. A device that may sleep waits for its first frame after the start
  (`<n> device(s) that may sleep are read and their reporting configured when next heard` at the start,
  `<device> heard (short 0x..): reading its datapoints and configuring its reporting now` at its frame;
  a refresh asked for by the device's own frame goes before the others). Whether a device sleeps is
  its receiver while idle (`rx_on_when_idle` in the database): the MAC capability of its announce, or
  the entry of the coordinator's or a router's neighbour table (the topology pass), learnt and saved
  with the next check (`<device> sleeps (its receiver is off while idle): ...`). A device whose
  receiver is not known yet (a database written before #108) is read at the start and again at its
  first frame.
- **Configurations and bindings without an answer (homecontrol #113).** A configuration that timed
  out (`... no answer within 30 s to the reporting configuration of battery ...: sent again after its
  next frame`) and a binding the device did not answer are sent again right after the device's next
  frame, at most `kTimeoutResends` (3) times each (`the reporting configuration of battery on
  endpoint 2 had no answer: sent again after its frame (1 of 3)`; `the binding of occupancy on
  endpoint 2 to the coordinator had no answer: ...`), under the one-device-at-a-time rule; a
  configuration the silence watch sent again keeps the watch's own schedule. The answer to a bind
  request is a ZDO status, logged by its name: 0x85 `TIMEOUT` is the stack's own answer when the
  device did not answer in time (no frame of the device, so it does not count as heard), an info
  line `<device>: no answer to the binding of cluster 0x0406 on endpoint 2 to the coordinator (ZDO
  status 0x85 TIMEOUT): sent again after its next frame`; any other status is the device's refusal, a
  warning `<device> refused the binding of cluster 0x0400 on endpoint 2 to the coordinator: ZDO
  status 0x8c TABLE_FULL`, not sent again. The ZDO answer carries only the status: the ESP driver
  keeps the endpoint and cluster of its last 16 bind requests (a static table, 97 bytes) and passes
  the request's slot as the answer's context; an answer whose slot a later request took names no
  cluster (`a binding`), and every binding of the device is then concerned. Until #113 every answer
  was logged as `refused a binding to the coordinator (status 0x85)`.
- **Reads (homecontrol #114).** The house controller (2026-11-22): the SML002's first frame after a
  restart was an occupancy report; its four points were read at once (with the four bindings and
  configurations: twelve requests that wait in its parent's queue for its polls), the illuminance
  got its first value 7½ minutes later with its first report, and `rule-hoflicht` logged three
  `conditionNoValue` meanwhile. The read had gone out and no answer came, and nothing asked again: a
  read without an answer, and one the stack did not take (its lock busy; the adapter ignored that),
  was lost until the device reported on its own. Now every read is watched per point (in the same
  `PointWatch`, its spare byte: the reads sent and the tick of the last one): its answer, a failure
  status or a report of the point ends it with one line, `<device>: illuminance on endpoint 2 read: 1
  (read 2 of 3)` (`... answered status 0x86`, `... reported 1 before the answer to its read`), at info
  for a device that sleeps or a repeated read, at debug for a listening device's first answer (the
  ring keeps the boot of a sleepy device, not the routine of every lamp). A read without an answer
  for `kReadAnswerMs` (30 s) is sent again right after the device's next frame (`no answer to the
  read of illuminance on endpoint 2 within 30 s: read again (2 of 3)`), `kReadTries` (3) reads in
  all, then given up (`no answer to 3 reads of illuminance on endpoint 2: its value comes with its
  next report`). The application's read (a rule's `read` action) is watched the same way.

**Requests to a sleepy device (homecontrol #117).** The house controller (2026-11-22, c8ea5a2): after
a restart the SML002 (a sleepy child of the coordinator) answered its four reads within 1.5 s of its
first frame, while the bindings and reporting configurations of occupancy and battery timed out
again and again (`0x85 TIMEOUT`); the warning `silent for 1800 s` followed. The adapter had handed the
stack twelve requests for the sensor in one go: four reads, then a binding and a configuration per
point in the point table's order (temperature, illuminance, occupancy, battery). What the stack does
with them (esp-zboss-lib for esp-zigbee-lib 1.6.8, the `zczr` debug archive disassembled with its
symbols, `file.c:line` from its trace calls):

- A frame for a child whose receiver is off waits in the coordinator's MAC indirect queue until the
  child polls (`zb_mac_put_data_to_pending_queue()`, `mac_cr_data.c`). The queue holds
  `gc_mac_pending_queue_size` frames for all sleepy children together, a quarter of the packet
  buffers (`zb_config_io_buffer_size()`: `n >> 2`): **12** with the 48 buffers of
  `esp_zb_io_buffer_size_set()` (issue #45; 20 with the default 80). A frame beyond it is refused
  with `TRANSACTION_OVERFLOW` (0xF1, `mac_cr_data.c:784`).
- Each frame in the queue expires after macTransactionPersistenceTime (`ZB_MAC_INDIRECT_POLL_EXPIRE_TMO`,
  7680 ms) with `TRANSACTION_EXPIRED` (0xF0, `zb_mac_pending_data_timeout()`, `:708-747`): the sensor
  must poll for each one within 7.68 s of its queuing; it fetches them one per poll and polls again at
  once while its parent signals more data.
- A request sent with an APS acknowledgement is repeated up to `ZB_N_APS_MAX_FRAME_RETRIES` (3) times,
  every `ZB_N_APS_ACK_WAIT_DURATION_FROM_SLEEPY` (10 s): each repeat is another frame in the same queue.

Twelve requests filled the queue alone, in the order sent, and the last of them (occupancy's and
battery's binding and configuration) were the ones dropped; their repeats after the sensor's next
frames (two or four at a time, plus the reads again) met the same queue. **The decision:** a device
that sleeps gets at most `kSleepyBurst` (3) requests after one frame of its own, a *round*: reads first
(the values), then a point's binding together with its configuration (2), then reads repeated after a
timeout; the rest waits for its next frame, which is its answer to these (each answer is a frame of the
device, and `kWatchDelayMs` after it the next round goes). The start's refresh, the interview's
configuration (`setup_reporting()`), the watch after a frame (`check_watches()`) and the re-sends of
bindings and configurations without an answer all take their requests from the round; a read that
does not fit waits as `PointWatch::kReadWanted` (dropped when the point reports first), a
configuration as `not_requested` (`configure_due()`), a binding as `kBindDue`, a repeated read stays
due. A sleepy device's configuration changed by new bindings (`reconfigure_reporting()`) is no longer
sent at once into the queue, where it would expire before the device polls, but waits for its next
frame the same way. The log says so (`<device>: the reporting of 0 attribute(s) requested, 4 more after
its next frames (it sleeps: 3 requests per frame of its own)`, then `<device>: the reporting of
occupancy on endpoint 2 requested` per round). A device whose receiver is on gets its requests at once,
as before.

**Undelivered requests.** The stack confirms every ZCL request the coordinator sends (its APS
confirmation, `esp_zb_zcl_command_send_status_handler_register()`, public API of esp-zigbee-lib): a
failure (the queue full or expired, no route, no acknowledgement) is one info line `<device>: a request
to endpoint 2 (ZCL transaction 37) was not delivered: ESP_FAIL` (the wrapper maps the stack's status
through `zb_ret_to_esp_err()`, which keeps only a few distinct codes). It is no frame of the device (not
heard), and the request's own watch times out as before. A binding (ZDO) has no such confirmation: its
`0x85 TIMEOUT` is logged as before.

**A child's frames after a restart (homecontrol #117, the restart case).** The issue suspected the
stack state restored at a start of dropping the child's frames. The disassembly rules out the
candidates:

- **The incoming NWK frame counters are not persisted.** The neighbour table's dataset
  (`zb_nvram_write_neighbour_tbl_dataset()`) writes 16-byte records without the counter, the restore
  (`zb_nvram_read_neighbour_tbl_dataset()`) sets bytes 0-9 of each 22-byte entry, and the table is
  allocated with `calloc()` (`zb_memconfig.c`): a restored child's counter is 0.
  `zb_nwk_unsecure_frame()` (`nwk_secur.c`) drops a frame only when its counter is below the stored one
  with the same key sequence number, or the stored one is 0xFFFFFFFF (`nwk_secur.c:267`): after a start every counter passes and the next one is
  stored.
- **The coordinator's outgoing counters** (NWK and APS) are persisted rounded up to the next multiple of
  1024 (`zb_nvram_write_counters_dataset()`, written by `zb_nwk_secure_frame()` whenever the NWK counter
  crosses a multiple of 1024): after a restart they continue above any value used before, so the child
  does not drop the coordinator's frames either.
- **The key pairs of #99** are written only for a device without a verified or provisional key
  (`restore_key()`); the boots of the house controller log `holds a link key for 2 of 2 known device(s)
  (0 restored)`: nothing was rewritten at those starts, no incoming APS counter reset. HA 1.2 data
  frames carry no APS security anyway (`zb_nlde_data_indication_continue()` unsecures only frames with
  the APS security bit).

What stays unexplained is the boot of 32c9e2f, where the sensor polled for 45 minutes (`expires_in_s`
renewed) and no frame of it reached the adapter: nothing was sent to it in that time (a sleepy device
gets its requests after its first frame), so the coordinator did not disturb it. Its next occurrence
shows in the log with the lines above (an undelivered request, a timeout) or their absence.

**Short addresses (homecontrol #109).** Every change of a device's short address (an announce with a
new one, the trust center's notice of a rejoin, a frame from a known IEEE with another address; a
device losing its address to another) is written with the next check (`kCheckPeriodMs`, 10 s), one
write for every change since the last, through the database's save (`save_database()`, the device
database's blob, written by the write queue's worker, see "Partitions"). So a restart starts from the address the previous
boot learnt, and the next rejoin logs `short <that address> -> <new>`.

## Radio facts, topology and the join log (homecontrol #98)

To tell a lost parent from an empty battery or a dead device (homecontrol #99), the adapter keeps
per device what the stack knows of its radio link and its place in the mesh, for this boot only (not
persisted), and shows it in the adapter view (`GET /api/zigbee`, spec/zigbee.md "Adapter view") and
on the Zigbee page.

**`lqi`, `rssi`.** With each frame the stack hands over (reports, read responses, commands, ZDO
answers, announces), the platform asks ZBOSS for the sender's values (`zb_zdo_get_diag_data()`,
ZBOSS's own API of the managed `esp-zboss-lib`; the esp-zigbee-lib's report callback carries
neither) and puts them into the event. What they are, read from the prebuilt libraries of
esp-zigbee-lib 1.6.8 / esp-zboss-lib (the debug archives disassemble with their symbols; there is
no source):

- `zb_zdo_get_diag_data()` looks the short address up in the **coordinator's neighbour table**
  (`zb_nwk_neighbor_get_by_short()`) and copies that entry's `lqi` and `rssi`; without an entry it
  gives LQI 0 and RSSI 127 (`ZB_MAC_LQI_UNDEFINED`, `ZB_MAC_RSSI_UNDEFINED`), and the adapter takes
  RSSI 127 as "none". So a device behind a router has no values (it is no neighbour), and a router's
  values are those of its own frames.
- The entry is written on the MAC data indication path (`zb_mcps_data_indication_cont`), i.e. by
  every NWK frame the neighbour sends (data and NWK commands): `lqi` and `rssi` of **that last
  frame**, not an average (`zb_nwk_neighbour_rssi_store()` would smooth the RSSI by a third per
  frame, but the field is set to the new value right before, so the stored value is the last one).
  The data polls of a sleepy child are MAC commands, not data indications: as far as the
  disassembly shows, they do not update it (they restart `expires_in_s`).
- The **LQI is derived from the RSSI** by the ESP port (`esp_zb_rssi_to_lqi()` in `libzboss_port`):
  0 below -80 dBm, 255 from -29 dBm, linear in between ((RSSI + 80) × 255 / 50). LQI 0 with RSSI
  -90 dBm is therefore a real value: a link below -80 dBm, near the radio's sensitivity (the house
  controller's plug, 2026-11-21: RSSI -94 and -90 dBm, LQI 0). That LQI 0 coincides with
  `ZB_MAC_LQI_UNDEFINED` is why the RSSI, not the LQI, tells "none".

**The coordinator's child table.** Every 10 s (with the stale check) the adapter reads the
coordinator's neighbour table through the driver (`ZigbeeDriver::read_neighbors()`, on the ESP
`esp_zb_nwk_get_next_neighbor()` under the stack's lock, waiting at most 20 ms for it: a busy stack
skips one read). A device listed as a child shows `parent: "coordinator"`, its end-device timeout
`timeout_s` (`device_timeout`) and the remaining time of its entry at the read `expires_in_s`
(`timeout_counter`); a polling sleepy device keeps restarting it. A router (from the table or its
announce's capability) has no parent.

**The routers' tables.** The parent of a device that is not the coordinator's child is in a
router's neighbour table, which the coordinator cannot read locally: the adapter asks each router
with the ZDO request Mgmt_Lqi_req (`esp_zb_zdo_mgmt_lqi_req()`, `ZigbeeDriver::request_neighbor_table()`)
in a pass over the known routers (a device the announce's capability or a neighbour table names a
router). The pass runs every 5 minutes and 3 s after a device announced itself or turned stale
(time for the router to list a rejoined child); one request is in flight, a router has 5 s per page,
a table is read in pages (about three entries each, at most 16 pages per router), a request for
another pass while one runs starts one after it. From the answers:

- a router's **child** entry gives the device's `parent` (the router's IEEE, its label as
  `parent_label`), `depth` and `parent_lqi`, the router's link quality of the device: the link the
  coordinator cannot see. A new or changed parent is logged (`<device> is a child of <router> (the
  router's link quality of it 120, depth 2)`). An *unauthenticated child* entry (a child in the
  middle of its join or rejoin) counts as a child. A device keeps a known parent until two passes in
  a row read that router's table completely without listing it (`is no longer a child of <router>
  (not in its table twice)`); complete is every page answered up to the table's total, with the total
  of the first page unchanged in the later ones. A silent router, an error status, an empty page
  before the end, the page limit and a table whose total changed while it was read (an entry added
  or aged out moves the following entries between two pages) prove nothing (debug: `neighbour table
  read up to entry 2 of 3 (the table changed while it was read): its children keep their parent`).
  Until homecontrol #114 one read decided, and an empty page or the page limit counted as the
  table's end: the house controller's SML002 lost its parent for a pass although the plug kept
  being its parent.
- a router's entry of the **coordinator** gives `coordinator_lqi`, the router's link quality of the
  coordinator's frames (the other direction of the link the coordinator measures as the router's
  `lqi`/`rssi`).
- a router that does not answer is logged once (`did not answer the request for its neighbour table
  ...`) until it answers again; its children keep the parent known. A rejected request (status not
  0, e.g. a router without Mgmt_Lqi) moves on to the next router (debug).

A parent also comes from the trust center's Update Device (`parent_short`), without `parent_lqi`
and `depth` until a router's table confirms it.

**The log.** The first read of the child table names every child (`<device> is a child of the
coordinator (short 0xaf9e, end-device timeout 15360 s, 9 s left)`), later reads every change
(`became a child`, `is no longer a child of the coordinator: removed from its child table
(end-device timeout ..., N s were left 10 s before; last heard M s ago)`). Every Update Device of the
trust center (unsecured join, secured rejoin, trust center rejoin, leave) is one info line with the
parent and the trust center's decision (`<device>: secured rejoin (short 0xaf9e) through the
coordinator, trust center: authorize`), every authorization (`authorization (legacy device):
success`, a failure at warn) and every MAC association (`associated with the coordinator`) too.

**A rejoin with a new address.** A device that announces itself with another short address than the
one known (the database keeps it across restarts) rejoined with a new address; that is one info line
instead of "announced itself": `<device> rejoined with a new address: short 0xaf9e -> 0x5025, no
trust-center notice (its parent is looked up in the routers' tables)`, or, when the trust center's
Update Device for it came within 10 s before, `... through <parent>, after the trust center's
secured rejoin notice` (the notice moves the short address first; the announce line still names the
old one). Without a notice the parent known stays, unconfirmed (no `parent_lqi`), and the pass that
follows the announce confirms it, names another router or, after two complete reads of its table
without the device, drops it.

**What the stack signals for a rejoin** (esp-zigbee-lib 1.6.8 headers, the call graph of the prebuilt
libraries):

- `ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE`: every device announce the coordinator receives (broadcast by the
  device after any join or rejoin); the only signal that comes for every rejoin.
- `ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE`: raised by the trust center in two places only:
  `zb_authenticate_dev()`/`zb_apsme_update_device_request()`, when a device joins or rejoins **the
  coordinator itself** as its parent; and `zb_apsme_update_device_indication()`, when **a router sends
  the trust center an APS Update Device** for a device that joined or rejoined through it (with a
  duplicate filter). A secured NWK rejoin through a router produces the signal only if that router
  sends Update Device; the house controller's Signify plug (LOM007) sent none for the SML002
  (2026-11-21: the announce with the new address 0x5025 was the only trace).
- `ESP_ZB_NWK_SIGNAL_DEVICE_ASSOCIATED`: only a MAC association at the coordinator
  (`zb_mlme_comm_status_indication()`), i.e. a join while the network is open, never a NWK rejoin.
- `ESP_ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED`: the trust center's authorization of a joining device.
- `ZB_SE_SIGNAL_CHILD_REJOIN` (ZBOSS) exists for Smart Energy builds only; the esp-zigbee-lib maps no
  signal for a rejoin at a router.

What the stack does not signal stays invisible: a NWK rejoin request the coordinator refuses below
the trust center leaves no line; the absence of a rejoin line while the device's LED says it
searches is then the evidence. A refusal by the trust center is signalled (`trust center: deny`) and
logged at warn (homecontrol #99).

## Rejoin policy (homecontrol #99)

A device that holds the network key and lost its parent must come back by whatever rejoin it tries,
with the network closed; a device the controller does not know must not get the network key. The
house controller's Hue motion sensor SML002 (a legacy HA 1.2 device) came back after every restart
of the coordinator by a secured rejoin through the plug, but on 2026-11-20 it searched with the
network closed and nothing reached the log until the network was opened.

**What the stack does** (esp-zigbee-lib 1.6.8 / esp-zboss-lib, the `zczr` debug archives
disassembled with their symbols; ZBOSS's trace calls carry the line numbers of its sources, quoted
below as `file.c:line`):

- **NWK layer.** A rejoin request at the coordinator (`zb_nlme_rejoin_request_pre_handler()` →
  `zb_nlme_rejoin_build_response()`, `nwk_cr_join.c`) is answered without looking at the permit-join
  state: a secured one goes to `zb_nwk_accept_child()` (capacity only); an unsecured one, a
  trust-center rejoin, first through `zb_check_dev_cap_for_tc_rejoin()`, which accepts any IEEE
  without a neighbour entry; refused only with distributed security or a stale key sequence number
  (status "access denied", `nwk_cr_join.c:399`, `:445`). The beacons' router and end-device
  capacity come from `nwk_have_space_for_children()` alone (`zb_nwk_do_update_beacon_payload()`).
- **Trust center** (`zb_authenticate_dev()`, `zdo_secur.c`, for the coordinator's own child and,
  through `zb_apsme_update_device_indication()`, for a router's Update Device), by the Update Device
  status:
  - secured rejoin (0): accepted.
  - unsecured join (1, an association): only while joining is permitted (or with the policy
    "authenticate always"); refused otherwise (`zdo_secur.c:853`). This gate stays.
  - trust-center rejoin (3): ignored with `ignore_tc_rejoin` (`:822`), refused with `tc_rejoin` off
    (`:828`); accepted when the trust center holds a verified or provisional link key for the device
    (`zb_secur_get_verified_or_provisional_link_key()`); without one only with
    `allow_unsecure_tc_rejoins` (`zb_aib_tcpol_get_allow_unsecure_tc_rejoins()`), refused otherwise
    (`:837`), whether the network is open or not. Through a router the refusal is a Remove Device to
    the router (`:1091`); the signal `ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE` carries `tc_action` deny.
  - Defaults (`zdo_secur_init()`): `tc_rejoin` on, `ignore_tc_rejoin` off,
    `allow_unsecure_tc_rejoins` off, the link key exchange required (turned off above for the legacy
    devices).
- **A legacy device's key.** At the join the trust center gives a device without a key a provisional
  one (`zb_secur_create_best_suitable_link_key_pair_set()`: the pre-configured standard key, type
  global, `zdo_secur.c:1921`). With the key exchange not required (`zigbee_service.cpp`,
  `esp_zb_secur_link_key_exchange_required_set(false)`), `bdb_link_key_transport_with_alarm()`
  schedules `zb_legacy_device_auth_signal_alarm()` 15 s after the key transport instead of the
  removal; that alarm **deletes the provisional key** (`zdo_app.c:1343`,
  `zb_secur_delete_link_key_by_idx()`) and signals "authorization (legacy device): success". The stack
  also forgets a device's keys when it leaves (`zb_nwk_forget_device()` →
  `zb_secur_delete_link_keys_by_addr_ref()`: a router's Update Device "left", the coordinator's child
  leaving).

**The cause.** From 15 s after its join on, the trust center holds no key for a legacy device. Its
secured rejoins pass, but its trust-center rejoin, the rejoin a device falls back to when the secured
one fails (its network key or frame counter no longer fits, its parent does not answer), is refused
at `zdo_secur.c:837`, also with the network open. Firmware before 7b0c18d passed only the unsecured
join of `ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE` on and dropped a trust-center rejoin's signal without a
line, so the silence of 2026-11-20 fits it; which rejoin brought the sensor back at 18:53 (after a
second press of its button, its old address, no unsecured join) the log of that firmware does not
tell. The next loss shows its kind: `trust center rejoin ... trust center: deny` at warn was this
refusal.

**The decision.**

- **Policy, at every network start** (`ZigbeeAdapter::network_state()`, through
  `ZigbeeDriver::set_rejoin_policy(true, false)`): ZBOSS's public "TC rejoin settings"
  (`zboss_api.h`) `zb_secur_set_tc_rejoin_enabled(ZB_TRUE)`, `zb_secur_set_ignore_tc_rejoin(ZB_FALSE)`,
  `zb_secur_set_unsecure_tc_rejoin_enabled(ZB_FALSE)`: the defaults, set explicitly so that a library
  update does not turn them silently. Log: `trust center: trust-center rejoins accepted from the
  devices it holds a link key for, refused from any other`.
- **A key for every known device** (in the device database and not left): `ZigbeeDriver::trust_device()`
  looks for a verified or provisional key (`esp_zb_secur_has_link_key_pair_set()`, the lookup of
  `:828`) and gives a device without one what the stack gives a joiner: the standard key
  `ZigBeeAlliance09`, global, provisional (`esp_zb_secur_update_key_pair()`, written to the stack's
  NVRAM). The outcome is read from the table (`esp_zb_secur_has_link_key_pair_set()` after the write,
  `hc::zigbee::restore_key()` in `hc/zigbee/key_table.h`), never from the write's answer: the wrapper
  `esp_zb_secur_update_key_pair()` inverts it. `zb_secur_update_key_pair()` returns the written key
  pair (non-null) on success and null on a failed NVRAM write (`zdo_secur.c:1764`), and the wrapper
  answers ESP_FAIL for non-null, ESP_OK for null (`bnez a0` → -1 in `esp_zigbee_test_utils.c`). The
  first test build (f19ec7b) took that ESP_FAIL for a refusal and logged `did not take one` for a key
  it had written; a refusal now names the stack's answer (`stack answer N`). At the network's start for every known device, and again where the stack drops the key:
  after a legacy device's authorization, a router's Update Device "left", the coordinator's child
  leaving to rejoin. Each restored key is one info line (`<device>: the trust center holds its link
  key again after its authorization (the well-known key it joined with): its trust-center rejoin is
  accepted`), a refused one a warning; the start ends with `trust center: holds a link key for N of
  M known device(s) (K restored)`. A Zigbee 3.0 device keeps its verified key; nothing is written
  for it.
- **Not `allow_unsecure_tc_rejoins`.** It would accept a trust-center rejoin of any IEEE (the NWK
  layer accepts one without a neighbour entry) and transport the network key encrypted with the
  well-known key: the network open to every stranger, always.
- **Not the association.** A device that associates afresh (unsecured join) is a new pairing and
  needs the network open, as before; a device with the network key rejoins.
- **Safety.** The trust center accepts a trust-center rejoin only for an IEEE in the database. What
  remains is legacy security itself: a device that claims the IEEE of a known legacy device (sent in
  the clear) gets the network key by a trust-center rejoin, as any device does during a permit join;
  Zigbee 3.0 devices keep their own key and are not exposed.
- **A device that stops being known loses its key** (`ZigbeeDriver::forget_device()`), whatever became
  of its leave: `remove()` drops the trust center's keys for it before it sends the leave (no short
  address, a refused or lost leave change nothing), and a device that left without rejoin (marked
  left) loses them too. The driver deletes every key pair of the IEEE and checks that none is left
  (`esp_zb_secur_has_link_key_pair_set()`). A key the stack cannot drop refuses the removal (warn line,
  409 over the API, nothing changed: "try again when the network is up"); for a leave it is a warning
  naming the device. The network's start restores keys only for the devices in the database, so a
  removed device stays without one. A removed device that never took its leave still holds the
  network key and can come back by a secured rejoin; only a new network key keeps it out.
- **The API.** `esp_zb_secur_has_link_key_pair_set()` and `esp_zb_secur_update_key_pair()` are the
  esp-zigbee-lib's only access to the key table; they are declared in its test header
  (`include/test/esp_zigbee_test_utils.h`, "mainly for certification and tests; ... might change").
  No header of esp-zigbee-lib 1.6.8 or 1.6.9 (the newest 1.x, 2026-11-20) nor of esp-zboss-lib declares
  a function that drops a key pair: `esp_zb_apsme_remove_device_request()` only sends a Remove Device
  to a parent, the install-code removal and the IEEE joining list (not compiled into this ZBOSS) do not
  touch it, and overwriting the key with `esp_zb_secur_update_key_pair()` cannot remove it (an entry
  of another attribute is added beside it, `zdo_secur.c:1707-1744`) and would leave an unknown key that
  breaks a later pairing of the device. So `forget_device()` calls ZBOSS's own
  `zb_secur_delete_link_keys_by_long_addr()` (`zdo_secur.c:2028`, what `zb_nwk_forget_device()` calls
  when a device leaves), a global symbol of the pinned `libzboss_stack` declared in `zigbee_service.cpp`
  with the signature its disassembly shows; a release without it fails at the link. The versions are
  pinned (`dependencies.lock.esp32c6`, 1.6.8); a change of these functions breaks the build, not the
  behaviour. No upgrade (1.6.9 adds nothing here).
- A trust-center rejoin that races the restore after a leave (the device rejoins within the
  milliseconds before the event loop restored its key) is refused once (warn line) and passes at the
  device's next attempt.

**Verification without a person** (`ZigbeeAdapter::request_rejoin()`, `POST
/api/zigbee/devices/<ieee>/rejoin`, `zigbee rejoin <ieee>`): the controller forces the device's loss
with a ZDO Mgmt_Leave with rejoin set (remove children off), addressed to its short address; a
sleepy child of a router takes it from the router's indirect queue at its next poll. One request at
a time. While the device has not taken it, the request goes out again when the device is heard
(its own frames are when it polls), at the earliest 30 s after the last send, at most 5 times. The
log follows it: `<device> (short 0xaf9e) asked to leave and rejoin`, `<device> took the request to
leave and rejoin` (its Mgmt_Leave_rsp) or `did not take ... (status 0x..)`, then the trust center's
notice and the announce (`secured rejoin` / `trust center rejoin ... trust center: authorize`,
`rejoined with a new address` or `announced itself`), and `<device> came back N s after the
requested rejoin`; an hour without the announce ends it with `did not come back within 60 min` at
warn. The adapter view carries `rejoin` while it is pending (spec/zigbee.md); the Zigbee page's
**Rejoin** button sends it.

## Threading

The stack runs in its own task and calls back there. Its callbacks (signals, attribute reports,
read responses, ZDO answers, commands) copy what the adapter needs into a ring of 8 events (16 before issue #45) and post
one drain message through the `LoopQueue`; the event loop delivers them to the adapter. A full ring
drops the frame, counts it (`zigbee status`: "events N dropped") and logs the first drop of a burst
(`hc.zigbee: Zigbee event dropped: the ring to the event loop is full ...`). The adapter's requests run on
the event loop's task: the driver takes the stack's lock (at most 500 ms; the periodic read of the
neighbour table at most 20 ms) and calls the SDK directly. A router's Mgmt_Lqi_rsp comes back through
the ring like every ZDO answer (at most 4 neighbour records per event). Allocation happens at `init()` (the adapter, the driver, the task) and at the model
reloads (the adapter's point tables, the specs it states); reports, commands and the stale check
allocate nothing.

## From the web UI

The **Zigbee** page (doc/web.md, "Pages") does what the console does, over the API (spec/api.md,
"Zigbee"): **Pair** opens the network for 254 s (`POST /api/zigbee/permit`) and counts down;
**Close** ends it early. While it is open, the WebSocket's `zigbee` events (`joined`,
`interviewing`, `interviewed`, `interview_failed`, `interview_incomplete`, `left`, ...) appear in a list under the network
card as they happen, and the device cards follow (the page reads `GET /api/zigbee` again on each
event). Each device card shows the state (an incomplete interview with what did not answer),
manufacturer and model, IEEE, last seen, the radio link (LQI and RSSI of the last frame, the parent
with the router's link quality of it and the depth, a router's link quality of the coordinator, the
child entry's timeout and remaining time, where known) and its
datapoints' live values, with **Identify** (10 s), **Rename** (the label), **Interview again**,
**Rejoin** (after a confirmation; "Rejoin policy"; a pending request shows as a row) and
**Remove** (after a confirmation; asks the device to leave and forgets it). The adapter reports
every change of its device list through `ZigbeeEventListener` (one listener, the `WebApi`, which
forwards to the live feed) and counts them in `generation`. On a chip without 802.15.4 the page
shows the reason the coordinator does not run.

## Console

```
zigbee status                  network (PAN id, channel, extended PAN id), joining, devices, datapoints, dropped events
zigbee devices                 every device: IEEE, short address, manufacturer / model, label, state; its datapoints and values
zigbee permit <seconds>        open the network for joining (1..254), 0 closes it
zigbee remove <ieee>           ask the device to leave and delete it
zigbee identify <ieee>         make the device identify itself for 10 s
zigbee label <ieee> [<text>]   name the device (at most 32 bytes; without text: clear)
zigbee interview <ieee>        interview the device again
zigbee rejoin <ieee>           make the device leave and rejoin at once (homecontrol #99, "Rejoin policy")
```

An IEEE address is 16 hex digits, colons allowed (`00:15:8d:00:01:02:03:04`). On a chip without
802.15.4 every command answers why Zigbee is not available.

## Wiring in `main.cpp`

`hc_zigbee` joins the `REQUIRES` of `main/CMakeLists.txt`; `main.cpp` holds the service next to the
KNX service and registers the console command:

```cpp
#include "zigbee_service.h"

hc::esp::ZigbeeService& zigbee() {
	static hc::esp::ZigbeeService instance(controller(), g_queue, g_model);
	return instance;
}

int zigbee_console(void*, int argc, char** argv) {
	return hc::esp::zigbee_console_command(zigbee(), argc, argv);
}

// in loop_task, after knx().init() and before image().start(): the adapter registers as a provider
// of the image service (doc/image.md "Dynamic datapoints"), then starts
	zigbee().init(image());

// in the console's extension table:
	{"zigbee", hc::esp::kZigbeeCommandHint, hc::esp::kZigbeeCommandHelp, &zigbee_console, nullptr},
```

E-Home's pairing through the API (issue #16) uses `ZigbeeService::permit_join()` and
`ZigbeeService::write_json()`.

## Not done

- **Labels and units in the model.** The adapter states a unit per generic datapoint and the image
  labels its datapoints, but the model carries neither yet (the UI shows the names).
- **Green Power devices**, install codes, touchlink, OTA updates of Zigbee devices.
- **Colour** other than colour temperature (hue/saturation, XY), scenes and groups.
- **Power source** of a device (mains or battery) is not read; the stale rule follows the reporting
  intervals instead.
- **Measurements on the device**: heap of the stack at run time, frame loss under WiFi load.
