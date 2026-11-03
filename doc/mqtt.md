# MQTT: the controller on a broker, Home Assistant, long-term history

*Operation of issue #18. The protocol, topics and payloads are specified in `spec/mqtt.md`; this
page says how to switch it on, what to expect on the broker and in Home Assistant, and what it
costs on the device.*

## What it is for

A household that already runs Home Assistant (or Node-RED, Telegraf, Grafana) wants the
controller's datapoints there without a second integration per technology: the controller
publishes every datapoint's state to an MQTT broker, accepts commands from it, announces the
runtime image's functions to Home Assistant through MQTT discovery (lights, dimmers, blinds,
switches, sensors, climate, scenes, buttons appear as entities of one device), and exports its
history rings so a collector (Telegraf into InfluxDB) keeps months where the device keeps days.
Without an `mqtt` member in the runtime image nothing of this runs.

## Switching it on

1. **The runtime image** names the broker (E-Home writes it; a hand-made descriptor adds it):

   ```json
   "mqtt": {"broker": "mqtt://192.0.2.5:1883", "username": "homecontrol", "baseTopic": "homecontrol/house"}
   ```

   `baseTopic` defaults to `homecontrol/<device>`; `discovery` and `history` are on by default
   (`spec/mqtt.md` section 2 lists every member). `mqtts://` uses the device's certificate bundle;
   the host build has no TLS and refuses it at connect, logged.
2. **The password** is a device setting, never in the image: on the Settings page (the field
   "MQTT password" is write-only: the device says whether one is stored, never shows it, an empty
   save removes it), through `PUT /api/settings/mqtt.password`, or on the console with
   `mqtt set password <pw>`. The user name may come from the image or from the setting
   `mqtt.user` (the image's wins). A change of either reconnects at once.
3. **Watch** `mqtt status` on the console (state, broker, user, base topic, discovery, history,
   the last error) or the log: `connected to mqtt://... ; base topic ...`.

An image with `mqtt.password` is refused as a whole, with the path `mqtt`: the password must not
travel through E-Home, files and `GET /api/image`.

## On the broker

With `mosquitto_sub -v -t 'homecontrol/#' -t 'homeassistant/#'`:

- `<base>/status` is `online` (retained) while the controller is connected; the broker publishes
  the last will `offline` when the connection drops, the controller publishes it before an orderly
  disconnect (a new image with another broker, the host build's SIGINT).
- `<base>/dp/<name>/state` carries `{"value": ..., "quality": "good"|"stale", "time": <UTC s>}`
  for every datapoint with a value, again after every connect and image load. States are retained
  by default (`retain: false` in the image turns that off).
- `<base>/dp/<name>/set` takes a JSON value (`true`, `21.5`, `{"hour": 6, "minute": 30, "second": 0}`)
  and runs it as a command with the model's rules (writable, the owner's acceptance); a refusal is
  published on `<base>/dp/<name>/error` and logged. Retained commands are refused: they would
  repeat at every reconnect.
- `<base>/fn/<function>/set` takes `OPEN`, `CLOSE`, `STOP` for a blind.
- `<base>/history/<name>` carries `[[t, value], ...]` batches of at most 32 samples right after a
  connect and then every `history.interval` minutes (default 15), per history ring.
- `homeassistant/<component>/<node id>/<function>/config` holds one retained discovery config per
  function of the image. After every connect and image load the controller publishes them again
  and for 10 s collects the retained configs and states under its node id; everything the image no
  longer has is cleared with an empty retained payload. Removal therefore works across image swaps
  and reboots without the controller remembering the previous image.

The **node id** is the device's mDNS host name (`homecontrol-<MAC>`), on the host
`homecontrol-<host name>`: the MQTT client id, Home Assistant's node id and device identifier.

## In Home Assistant

With the MQTT integration configured for the same broker (discovery prefix `homeassistant`, the
default), the controller appears as one device named after the image (`image.name`, else the node
id) with manufacturer and model "HomeControl" and the firmware version, and an entity per function.
The state payload's `value`, `quality` and `time` show as the entity's attributes. Lights, switches
and scenes command the datapoints the function names; a dimmer's brightness is 0..100; a blind takes
open/close/stop and a position; a climate entity shows the current temperature and sets the
setpoint (HVAC modes are not mapped in this version, `spec/mqtt.md` section 4). Entities of
functions removed from the image disappear within the sweep after the next image load.

## Long-term history

Telegraf's `mqtt_consumer` input with `data_format = "json"` on `<base>/history/#` (or a small
Node-RED flow) writes the batches into InfluxDB; the sample time is the point's time, so a batch
exported twice (the cursor starts over after a reboot) overwrites instead of duplicating. The export
waits for a synchronized wall clock.

## Connection behaviour

Connects when the network is up; a failed attempt or a lost connection is retried after 5 s,
doubling up to 5 minutes; an attempt without an answer after 30 s counts as failed; keepalive 60 s.
An image swap that keeps the broker, the user name and the base topic keeps the connection and
republishes; any other change retires it (the configs cleared when the broker or prefix changes or
discovery is switched off, `offline`, disconnect) and connects anew. A model reload because a
Zigbee device joined or left keeps the connection and republishes every state, so the new device's
datapoints appear.

## Console

`mqtt status`, `mqtt publish-discovery` (publishes every config again and sweeps; for a Home
Assistant that was reinstalled), `mqtt set password <pw>`, `mqtt clear password`. `settings list`
masks the password.

## On the device

The component `hc_mqtt` runs esp-mqtt (part of ESP-IDF, TLS through the certificate bundle) in its
own task; its events reach the event loop through the loop queue, the core's `hc::Mqtt` decides
everything. Cost on the ESP32-C6: about 15 KB of flash; nothing else without an `mqtt` member in the
image (issue #44). An image's configuration allocates the core's payload and history buffers (about
3.6 KB) and one arena for the functions and the per-datapoint table (a few hundred bytes for a
small house), the first connect the four inbound slots (about 3 KB) and esp-mqtt's task stack and
buffers (about 8 KB); an image without `mqtt` frees them again. The POSIX build speaks MQTT 3.1.1 over TCP with a client of its own (`hc_posix_web`),
without TLS; the SBC fallback talks to a broker on the LAN exactly like the device.

## Limits

Topic 255 bytes (a longer datapoint topic is not published, logged); discovery config 2048 bytes;
32 samples per history message; inbound payload 512 bytes on the device (a longer command is
refused); 16 messages per loop turn (a burst is spread over turns). On the device esp-mqtt's
outbox holds at most 8 KB (`EspMqttClient::kOutboxLimit`, 16 KB before issue #45: some 50 states
or 7 discovery configs, each a heap block until the broker took it): a publication it refuses waits
in the core (the latest state of every datapoint, the next discovery config) and is tried again
after 250 ms, so the discovery of a large image is paced by the broker, not dropped by the limit. No QoS 2.
One broker.

## Tests

`core/test/mqtt_test.cpp` (the client under a fake broker: connection, backoff, states, commands,
discovery of every kind against `spec/fixtures/mqtt/*.discovery.txt`, the sweep, image swaps, the
history export), `core/test/mqtt_codec_test.cpp` (the packet codec), `core/test/mqtt_probe_test.cpp`
(the image service's hooks, the settings), `platform/posix/test/posix_mqtt_client_test.cpp` (the
host client against an in-process broker). End to end on the desk: the host controller against a
minimal broker script showed the connect, the discovery configs of the example image, the removal
of a stale config, the states and the orderly disconnect.
