# MQTT: topics, Home Assistant discovery, history export

*Specification of issue #18: what the controller publishes to an MQTT broker and what it accepts
from it, configured by the runtime image's optional `mqtt` member. Implemented by `hc::Mqtt`
(`core/include/hc/mqtt.h`, the operation in `doc/mqtt.md`) over the platform's `MqttClient`
(esp-mqtt on the device, a small own MQTT 3.1.1 client on POSIX). Fixtures: `spec/fixtures/mqtt/`.*

## 1. Decisions

- **Q1. Optional and off by default.** Without an `mqtt` member the controller does not talk MQTT.
  The member names the broker; the image describes the whole configuration (runtime-image I6).
- **Q2. The password is a device setting, never part of the image.** The image travels through
  E-Home, files and `GET /api/image`; a secret does not belong there. The broker password is the
  setting `mqtt.password` (console: `mqtt set password <pw>`); the user name may be in the image
  (`username`) or, if the image names none, the setting `mqtt.user`. An image with
  `mqtt.password` is refused. The web API's tokens are not used for MQTT.
- **Q3. One JSON state payload per datapoint.** `{"value": ..., "quality": ..., "time": ...}` on
  `<base>/dp/<name>/state`: one message per change carries the value with its quality and time
  (a separate attributes topic would double the traffic). Home Assistant reads it with templates;
  a collector (Telegraf, Node-RED) gets the quality with the value.
- **Q4. Commands are JSON values on `<base>/dp/<name>/set`**, the value forms of `spec/api.md`
  ("Values"), executed with the rules of `DatapointModel::command()`. One wildcard subscription
  covers every datapoint; a refusal is published on `<base>/dp/<name>/error` and logged. A
  retained command is refused: it would repeat at every reconnect (a blind moving at every
  network hiccup).
- **Q5. Removal by a sweep of the retained topics.** After every connect and image load the
  controller subscribes for a while to its own discovery configs and state topics and clears
  (empty retained payload) every retained one the image no longer has. This removes vanished
  functions across image swaps *and* reboots without remembering the previous image.
- **Q6. History export in bounded batches with a cursor per datapoint.** The rings hold hours to
  days; long-term storage is the broker's subscriber's job (InfluxDB through Telegraf).
- **Q7. QoS.** Status and discovery messages are QoS 1 and retained; states QoS 0 (retained by
  default: the latest value is what matters, and the next change follows); history QoS 0 (a lost
  batch is not resent; Q6). The controller never asks for QoS 2.

## 2. The image member

```json
"mqtt": {
	"broker": "mqtt://192.0.2.5:1883",
	"username": "homecontrol",
	"baseTopic": "homecontrol/house",
	"retain": true,
	"discovery": {"enabled": true, "prefix": "homeassistant"},
	"history": {"enabled": true, "interval": 15}
}
```

| Member | Type | Required | Meaning |
|---|---|---|---|
| `enabled` | boolean | no, true | `false` keeps the configuration but does not connect (a connection retires: section 6). |
| `broker` | `mqtt://host[:port]` or `mqtts://host[:port]`, at most 127 bytes | yes | Host name or IPv4 literal; port 1..65535, default 1883 (8883 with TLS). No user, path or query. `mqtts://` verifies the server with the device's certificate bundle; the POSIX build has no TLS and refuses it at connect (logged). |
| `username` | string, 1..63 | no | The user name; without it the setting `mqtt.user`, else none. |
| `password` | — | **refused** | The setting `mqtt.password` (Q2). |
| `baseTopic` | 1..64 bytes of printable ASCII without spaces, `+`, `#`, empty levels or a leading or trailing `/` | no, `homecontrol/<device>` | Every topic of this controller starts with it. `<device>` is the node id without its `homecontrol-` prefix (`homecontrol/a1b2c3` for `homecontrol-a1b2c3.local`). |
| `retain` | boolean | no, true | Whether the state topics are retained. |
| `discovery.enabled` | boolean | no, true | Home Assistant discovery (section 4). `false` also clears the configs the sweep finds. |
| `discovery.prefix` | like `baseTopic`, at most 32 bytes | no, `homeassistant` | Home Assistant's discovery prefix. |
| `history.enabled` | boolean | no, true | The history export (section 5). |
| `history.interval` | integer 1..1440 (minutes) | no, 15 | Period of the export. |

Unknown members are ignored (runtime-image I5); `comment` and `description` are allowed. The member
is checked by `Mqtt::check_json()` (the loader's validation pass) and applied after the datapoint
model; a refusal names the member (`mqtt.baseTopic: must be ...`).

The **node id** is the device's mDNS host name (`homecontrol-<last three MAC bytes>`, `doc/network.md`):
the MQTT client id, Home Assistant's node id and device identifier.

## 3. Topics and payloads

`<base>` is the base topic; `<name>` a datapoint's id with `/`, `+`, `#`, `%` and control bytes
percent-encoded (`sensor/odd+name#1` → `sensor%2Fodd%2Bname%231`), so every datapoint is one topic
level.

| Topic | Direction | Retained | QoS | Payload |
|---|---|---|---|---|
| `<base>/status` | out | yes | 1 | `online` after every connect; `offline` as the last will and before an orderly disconnect. |
| `<base>/dp/<name>/state` | out | `retain` | 0 | `{"value": <value>, "quality": "good"\|"stale", "time": <UTC seconds>}` on every change of value or quality, and for every datapoint with a value after each connect and image load. `time` is the wall-clock time of the update; absent while the clock is not synchronized or for a value restored from before a reboot. Not published while the value is unknown. |
| `<base>/dp/<name>/set` | in | — | ≤1 | A JSON value of the datapoint's type (`spec/api.md` "Values": `true`, `21.5`, `{"hour": 6, "minute": 30, "second": 0}`); a number is accepted for every numeric type when it converts exactly. |
| `<base>/dp/<name>/error` | out | no | 0 | `{"error": "<reason>", "payload": "<the first 48 bytes of the command>"}` for a refused command: unknown datapoint, not writable, not JSON, wrong type, the owner refused, retained, too long. |
| `<base>/fn/<function>/set` | in | — | ≤1 | `OPEN`, `CLOSE` or `STOP` for a `blind` (section 4). |
| `<base>/fn/<function>/error` | out | no | 0 | As for datapoints. |
| `<base>/history/<name>` | out | no | 0 | `[[t, value], ...]`: section 5. |
| `<prefix>/<component>/<node id>/<function>/config` | out | yes | 1 | Home Assistant discovery: section 4. An empty payload removes it. |

The controller subscribes `<base>/dp/+/set` and `<base>/fn/+/set` after every connect, and during
a sweep `<prefix>/+/<node id>/+/config` and `<base>/dp/+/state`.

**The sweep** (Q5) lasts 10 s after each connect, image load and `mqtt publish-discovery`. Every
message delivered with the retained flag (the broker's store, not live traffic) is checked: a
config whose function (with that component) is not in the image, or whose node id is ours while
discovery is off, and a state topic of a datapoint the model does not have, are cleared with an
empty retained payload. If the outgoing queue is full, the sweep runs again.

## 4. Home Assistant discovery

For every function of the image (`spec/runtime-image.md` 3.5) one retained config. All topics are
relative to `"~"` (Home Assistant's base-topic abbreviation) = `<base>`. Every config has:

| Key | Value |
|---|---|
| `~` | the base topic |
| `name` | the function's label (a string; of a label object `en`, else the first language) |
| `unique_id` | `<node id>_<function id>` |
| `availability_topic` | `~/status` (payloads `online`/`offline`, Home Assistant's defaults) |
| `json_attributes_topic` | the state topic of the function's main datapoint (below), so `value`, `quality` and `time` appear as attributes |
| `device` | `{"identifiers": [<node id>], "name": <image.name, else the node id>, "manufacturer": "HomeControl", "model": "HomeControl", "sw_version": <firmware version>}` |

`S(role)` is the role's datapoint state topic `~/dp/<name>/state`, `C(role)` its command topic
`~/dp/<name>/set`. The templates read the state payload: `V` = `{{ value_json.value }}`,
`B` = `{{ 'true' if value_json.value else 'false' }}`, `OO` = `{{ 'ON' if value_json.value else 'OFF' }}`.

| Kind | Component | Mapping (keys present only with their role) | Main datapoint |
|---|---|---|---|
| `light` | `light` | `command_topic` C(command), `payload_on` `true`, `payload_off` `false`; `state_topic` S(state) with `state_value_template` B | state |
| `dimmer` | `light` | `brightness_command_topic` C(levelCommand), `brightness_scale` 100 (with `levelCommand` or `level`); `brightness_state_topic` S(level) with `brightness_value_template` V. With `command`: `command_topic` C(command), `payload_on` `true`, `payload_off` `false`. Without: `command_topic` C(levelCommand), `on_command_type` `brightness`, `payload_off` `0`. `state_topic` S(state) with B, else S(level) with `{{ 'true' if value_json.value > 0 else 'false' }}` | state, else level |
| `blind` | `cover` | `command_topic` `~/fn/<id>/set`, `payload_open` `OPEN`, `payload_close` `CLOSE`, `payload_stop` `STOP` (with `stop`, else `null`); `position_topic` S(position) with `position_template` V; `set_position_topic` C(positionCommand); `position_open` 0, `position_closed` 100 (KNX: 0 % is open); without `position` but with `atBottom` (1.6): `state_topic` S(atBottom) with `value_template` `{{ 'closed' if value_json.value else 'open' }}` (closed at the bottom, else open; `atTop` alone gives no state, its false value does not say closed); `tilt_status_topic` S(slat) with `tilt_status_template` V, `tilt_command_topic` C(slatCommand), `tilt_min` 0, `tilt_max` 100; `device_class` `blind` | position, else atBottom |
| `switch` | `switch` | `command_topic` C(command), `payload_on` `true`, `payload_off` `false`; `state_topic` S(state) with `value_template` B | state |
| `sensor`, boolean value | `binary_sensor` | `state_topic` S(value), `value_template` OO | value |
| `sensor`, other | `sensor` | `state_topic` S(value), `value_template` V (time of day `HH:MM:SS`, date `YYYY-MM-DD`, date-time `YYYY-MM-DD HH:MM:SS`; controlled, color and raw as JSON); for unsigned, signed and real: `unit_of_measurement` (the function's `unit`, else the datapoint's), `device_class` from the unit (table below), `state_class` `measurement` (`total_increasing` for energy) | value |
| `climate` | `climate` | `current_temperature_topic` S(temperature) with `current_temperature_template` V; `temperature_state_topic` S(setpoint) with V; `temperature_command_topic` C(setpointCommand); `action_topic` S(heating), else S(heatingLevel) (1.6), with `{{ 'heating' if value_json.value else 'idle' }}` for a boolean and `{{ 'heating' if value_json.value > 0 else 'idle' }}` for a number (an actuating value in percent); `modes` `["heat"]`, `temperature_unit` `C`, `temp_step` 0.5 | temperature |
| `scene` | `scene` | `command_topic` C(command), `payload_on` the scene's value: `scene − 1` for a number or enumeration (KNX DPT 17.001 counts scenes from 0), `{"control":false,"value":scene − 1}` for a controlled datapoint (DPT 18.001) | — |
| `button`, writable press | `button` | `command_topic` C(press), `payload_press` `true` (boolean) or `{"control":true,"value":1}` (controlled) | — |
| `button`, read-only press | `binary_sensor` | `state_topic` S(state), else S(press), `value_template` OO (`value_json.value.value` for a controlled press) | state, else press |
| `button`, enumeration press (a Zigbee remote's action, image 1.4) without `state` | `sensor` | `state_topic` S(press), `value_template` V (the element of the last press, spec/runtime-image.md 3.5) | press |

| Unit | `device_class` |
|---|---|
| `°C`, `°F`, `K` | `temperature` |
| `%` | `humidity` |
| `lx` | `illuminance` |
| `W`, `kW` | `power` |
| `Wh`, `kWh` | `energy` (`state_class` `total_increasing`) |
| `V` | `voltage` |
| `A` | `current` |
| `hPa`, `mbar` | `pressure` |
| `m/s`, `km/h` | `wind_speed` |

Not mapped in this version: a climate's `mode`/`modeCommand` (Home Assistant's HVAC modes and
presets do not correspond to KNX's HVAC mode or a Zigbee thermostat's without a mapping table in the
image), the function's `room` (MQTT discovery has no per-entity area). A config longer than 2048
bytes is not announced (logged).

Configs are published after every connect and every image load; the sweep (section 3) removes the
others.

## 5. History export

For every datapoint with a history ring (`history` in `spec/runtime-image.md` 3.3) the samples since
the last export go to `<base>/history/<name>`:

```json
[[1792270800, 20.5], [1792271100, 20.75], [1792271400, 21]]
```

- `t`: UTC seconds of the sample; `value`: a number (a real with seven significant digits, the ring's
  single precision), `true`/`false` for a boolean.
- **When**: right after every connect, then every `history.interval` minutes; not while the wall clock
  is unsynchronized (the next interval tries again).
- **Batches**: at most 32 samples per message, ascending; a ring's samples are at least its spacing
  apart, so a window of 32 spacings holds at most 32 and nothing is decimated.
- **Cursor**: per datapoint the time of the last exported sample; it advances only with a message the
  client accepted, follows the datapoint's name across image swaps, and starts over (the whole ring)
  after a reboot: a collector deduplicates by time (InfluxDB overwrites a point with the same time and
  tags).
- Only samples before the second the export started go out; the next export takes the rest.

## 6. Connection

- The client id is the node id, clean session, keepalive 60 s, the last will `offline` on
  `<base>/status` (retained, QoS 1).
- The controller connects when the network is up (checked every 5 s while down); a failed attempt or
  a lost connection is retried after 5 s, doubling up to 5 minutes; an attempt without an answer
  after 30 s counts as failed.
- An image swap that keeps `broker`, the user name and `baseTopic` keeps the connection; any other
  change (and `enabled: false`, and an image without `mqtt`) *retires* it: the discovery configs are
  cleared (when the broker or prefix changes, discovery is switched off or MQTT is), `offline` is
  published, and after the outgoing queue drained (at most 3 s) the controller disconnects and, with
  a configuration, connects anew.

## 7. Limits

| | Limit |
|---|---|
| Topic | 255 bytes (a longer datapoint topic is not published, logged) |
| Discovery config | 2048 bytes |
| History message | 32 samples |
| Inbound payload | the device keeps the first 512 bytes (a longer command is refused); POSIX accepts packets up to 256 KB |
| Messages per loop turn | 16 (a burst is spread over turns) |

## 8. Fixtures

- `spec/fixtures/mqtt/kinds.json`: an image with every kind and role variant and an `mqtt` member.
- `spec/fixtures/mqtt/kinds.discovery.txt`, `example.discovery.txt`: the configs for `kinds.json`
  and for `spec/fixtures/images/example.json` (with `{"broker": "mqtt://broker.example:1883"}`),
  node id `homecontrol-a1b2c3`, firmware version `test`; one line per config, `<topic> <payload>`.

## 9. Changes

- 1.0 (issue #18): first version.
