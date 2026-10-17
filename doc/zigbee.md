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
| ESP: stack task, driver, event ring, `ZigbeeService`, the `zigbee` console command, NVS `BlobStore` | `platform/esp/components/hc_zigbee` |
| Stack configuration | `platform/esp/sdkconfig.defaults.app.esp32c6`, menu "HomeControl Zigbee" |
| Tests | `core/test/zigbee_*_test.cpp` |

The image format of the bindings and the JSON of the database and the adapter are in
`spec/zigbee.md`.

## Decision: esp-zigbee-lib 1.6 (ZBOSS) on IDF 5.3

Espressif ships two lines of its Zigbee SDK. The 2.x line (Espressif's own stack) recommends IDF
5.5.4; the 1.x line is the long-term-support line on the ZBOSS stack, recommends IDF 5.3.2 and
declares `idf >= 5.0`. The project builds on IDF 5.3.0, so it takes `espressif/esp-zigbee-lib ~1.6.8`
with `espressif/esp-zboss-lib ~1.6.4` (`main/idf_component.yml`, only for the targets esp32c6 and
esp32h2, pinned in `dependencies.lock.esp32c6`). It builds and links on 5.3.0. Moving to 2.x means
an IDF upgrade first; the core does not depend on the SDK, only `hc_zigbee` does.

The SDK runs as coordinator (`CONFIG_ZB_ZCZR`) on the native radio (`CONFIG_ZB_RADIO_NATIVE`).
Green Power stays enabled (the SDK's default for a coordinator: the Green Power proxy belongs to
Zigbee 3.0); Green Power devices themselves are not supported.

## Bring-up

`ZigbeeService::init()` (after the WiFi driver) checks that the partitions `zb_storage` and `zb_fct`
exist, opens the device database in the `zb_db` partition, starts the adapter and the task
`hc_zigbee`. The task configures the stack: coordinator with `HC_ZIGBEE_MAX_CHILDREN` children, the
channel mask `HC_ZIGBEE_CHANNEL_MASK`, the PAN id `HC_ZIGBEE_PAN_ID` (0: the stack picks one), the
transmit power `HC_ZIGBEE_TX_POWER`, and endpoint 1 as an HA home gateway:

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
chooses: `none` (the default, lowest latency of the web UI and KNX) or `min`; the coordinator logs
a warning when it finds power save off at network start, naming the setting. Measured on the C6
over the home LAN (60 requests each, the board idle with KNX, the web server and the coordinator
up): see the table below. Whether `none` costs Zigbee frames in practice is measured once a device
is paired (#15 stage two); a household with Zigbee devices that report late sets `min`.

| `wifi.ps` | ICMP ping avg / max | `GET /api/auth/session` median / p90 / max | KNX status reply after a command |
|---|---|---|---|
| `none` | 19 to 27 ms / 70 ms | 69 to 79 ms / 84 ms / 87 ms | 16 to 200 ms |
| `min` | 62 to 768 ms / 3979 ms | 83 ms / 1220 to 1450 ms / 3003 ms | not received within 6 s (two of two runs) |
| `max` | 65 ms / 151 ms | 81 ms / 190 ms / 2194 ms | 311 ms |

Measured 2026-10-18 on the C6 with firmware of that day against a consumer access point (the
numbers depend on its DTIM period and on the coexistence arbiter): with `min` the link stalls for
seconds at a time and the actuator's status telegram did not reach the tunnel within the window,
which is why `none` is the default and `min` is a per-household choice with its cost named on the
Settings page. The measurement is repeated with a paired Zigbee device in #15 stage two, to weigh
the Zigbee side.

### Partitions

The SDK finds its storage by fixed labels: `zb_storage` (16 KB, the stack's NVRAM: network, keys,
children, bindings) and `zb_fct` (4 KB, factory data: install codes; unused but required). The
device database lives in `zb_db` (24 KB, NVS) until the LittleFS `BlobStore` of
`doc/persistence.md` exists. The three partitions (44 KB) sit at the end of the flash
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
interviews it:

1. **Active endpoints** (ZDO), then the **simple descriptor** of every endpoint (profile, device
   type, input and output clusters; at most 4 endpoints and 12 clusters each are kept, the known
   clusters first).
2. **Basic cluster**: manufacturer name and model identifier.
3. **IAS Zone** endpoints: the zone type (it names the datapoint: contact, motion, smoke, water,
   else alarm), then the enrollment: the coordinator writes its IEEE address into the zone's CIE
   address, answers the zone's Zone Enroll Request and receives its status changes.

Each step has 10 s and 3 attempts. A device whose endpoints cannot be learnt stays uninterviewed and
is tried again when it is heard after a minute; missing names or zone types do not fail the
interview. One device is interviewed at a time; others wait their turn.

After the interview the adapter writes the database, loads the device's datapoints, binds each
mapped server cluster to the coordinator's endpoint 1 (bindings for the buttons' client clusters,
none for IAS Zone, which reports to its CIE address), configures the reporting of each mapped
attribute (table below), and reads the current values.

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

`on_off` is also commandable through Level Control (a dimmer without the On/Off cluster takes Move
to Level with On/Off). The writable datapoints (`.on`, `.level`, `.color_temperature`) carry the
flags writable and transmit; the server functions except `multistate` are read at start.

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

A device that has configured reporting must be heard within three times the largest maximum
reporting interval of its datapoints (`kStaleFactor`): a bulb (max 300 s) within 15 minutes, a
temperature sensor (max 600 s) within 30 minutes, a battery-only device (max 21600 s) within 18
hours. The adapter checks every 10 s; the silence counts from the controller's start for a device
not heard since. A stale device's datapoints get the quality stale; its next frame ends it and its
values are read again. Devices without configured reporting (pure buttons, IAS zones) never go
stale.

## Threading

The stack runs in its own task and calls back there. Its callbacks (signals, attribute reports,
read responses, ZDO answers, commands) copy what the adapter needs into a ring of 16 events and post
one drain message through the `LoopQueue`; the event loop delivers them to the adapter. A full ring
drops the frame and counts it (`zigbee status`: "events N dropped"). The adapter's requests run on
the event loop's task: the driver takes the stack's lock (at most 500 ms) and calls the SDK
directly. Allocation happens at `init()` (the adapter, the driver, the task) and at the model
reloads (the adapter's point tables, the specs it states); reports, commands and the stale check
allocate nothing.

## Console

```
zigbee status                  network (PAN id, channel, extended PAN id), joining, devices, datapoints, dropped events
zigbee devices                 every device: IEEE, short address, manufacturer / model, label, state; its datapoints and values
zigbee permit <seconds>        open the network for joining (1..254), 0 closes it
zigbee remove <ieee>           ask the device to leave and delete it
zigbee identify <ieee>         make the device identify itself for 10 s
zigbee label <ieee> [<text>]   name the device (at most 32 bytes; without text: clear)
zigbee interview <ieee>        interview the device again
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
- **Device database on LittleFS**: the database uses its own NVS partition until the `BlobStore` of
  `doc/persistence.md` exists.
- **Measurements on the device**: heap of the stack at run time, frame loss under WiFi load.
