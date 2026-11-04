# Zigbee: bindings, device database, adapter view

The contract between the controller's Zigbee adapter (`doc/zigbee.md`) and E-Home: how a runtime
image names Zigbee datapoints, what the controller stores about the joined devices, and the JSON
view the console and the API (issue #16) show. Numbers in JSON are decimal; IEEE addresses are
strings of 16 lower-case hex digits (the controller also accepts colons when parsing).

## Generic datapoints

Without a binding, every mapped function of a joined device is a datapoint named

```
zigbee.<ieee>.<endpoint>.<suffix>
```

with the suffixes of the function table in `doc/zigbee.md` (`on`, `level`, `color_temperature`,
`temperature`, `humidity`, `illuminance`, `occupancy`, `contact`/`motion`/`smoke`/`water`/`alarm`
for an IAS zone by its zone type, `battery`, `multistate`, `action`). Example:
`zigbee.00158d0001020304.1.temperature`.

## Bindings (runtime image, issues #8 and #29)

A runtime image binds a device's functions to datapoints of its own: a datapoint of the image's
`datapoints` (`spec/runtime-image.md` 3.3) with a `zigbee` member instead of `knx`. The datapoint's
`id` is the name; its type and flags are the image's. A device with at least one binding gets exactly
its bound datapoints (its unbound functions get none); a device without bindings gets the generic
datapoints, added to the model behind the image's (`doc/image.md` "Dynamic datapoints").

```json
{"id": "living.ceiling", "label": "Ceiling light", "type": "boolean", "writable": true, "transmit": true,
 "zigbee": {"ieee": "00158d0001020304", "endpoint": 11, "cluster": 6, "attribute": 0, "command": 6}}
```

| Member | Meaning |
|---|---|
| `ieee` | The device: 16 hex digits, colons allowed. Not 0 and not all ones. Required. |
| `endpoint` | 1..240. Required. |
| `cluster`, `attribute` | Select the function (the cluster/attribute pairs of the function table in `doc/zigbee.md`), decimal 0..65535. Required; `attribute` not for a client binding. |
| `client` | `true`: the device's client cluster commands (a button: `cluster` 6 or 8), the `action` function; `attribute` is then ignored and `command` not allowed. Default `false`. Each command the remote sends is one update of the datapoint (0 off, 1 on, 2 toggle, 3 dim up, 4 dim down, 5 dim stop), also a repeated one; the datapoint is the `press` of a `button` function with its `presses` (`spec/runtime-image.md` 3.5, 1.4). |
| `command` | The cluster commands go to (0..65534); absent: read-only. Only the function's own cluster, or Level Control (8) for `on_off` (cluster 6), is accepted. |
| `reporting` | The attribute reporting the controller configures for this function instead of the function table's defaults (below, "Reporting overrides"). Optional; not for a client binding (`action`), `multistate` and `ias_zone`. |

The loader checks every binding before anything changes and refuses with the path and the reason:

| Path | Reason |
|---|---|
| `datapoints[i].zigbee.ieee` | `ieee is missing`, `"..." is not an IEEE address (16 hex digits, colons allowed; not 0 and not all ones)` |
| `datapoints[i].zigbee.endpoint` | `endpoint is missing`, `endpoint must be 1..240` |
| `datapoints[i].zigbee.cluster`, `.attribute`, `.command` | missing (`attribute is missing (only a client binding, a button's commands, has none)`) or not an integer in range |
| `datapoints[i].zigbee` | `cluster C attribute A: no known function for this cluster and attribute (doc/zigbee.md lists them)` |
| | `cluster C attribute A: the command cluster cannot set this attribute` |
| | `client cluster C: a client binding (button commands) needs cluster 6 (On/Off) or 8 (Level Control)` / `... cannot be commanded` |
| | `the function F carries T values, the datapoint is U` (numeric types and enumeration are interchangeable) |
| | `a transmitting Zigbee datapoint needs the cluster its commands go to (command); ...` |
| | `a datapoint is either knx or zigbee`, `a datapoint is either zigbee or virtual` |
| | `device D endpoint E: the function F is bound twice (also datapoints[j] 'name')` |
| `datapoints[i].zigbee.reporting` | `reporting must be an object` |
| | `the function action (a client binding, button commands) configures no reporting` |
| | `the function multistate configures no reporting (its reports are events)` |
| | `the function ias_zone configures no reporting (zone status change notifications)` |
| `datapoints[i].zigbee.reporting.min` | `min must be an integer`, `min must be 0..65534` |
| | `min must not exceed max (an absent max is the function's default); max 0: no periodic reports, max 65535: no reports` (only `min` given) |
| `datapoints[i].zigbee.reporting.max` | `max must be an integer`, `max must be 0..65535` |
| | `max must not be below min (an absent min is the function's default); 0: no periodic reports, 65535: no reports` |
| `datapoints[i].zigbee.reporting.change` | `change must be a number` |
| | `the function on_off has no reportable change (a discrete value reports every change)`, the same for `occupancy` |
| | `the function illuminance has no reportable change in lux (its attribute is logarithmic: a lux step has no fixed attribute step); min and max are allowed` |
| | `the function color_temperature has no reportable change in K (its attribute counts mireds, 1000000 / K: a kelvin step has no fixed attribute step); min and max are allowed` |
| | `change must be 0..327.67 (°C)` (temperature), `change must be 0..100 (%)` (level, humidity, battery) |
| `datapoints` | `N Zigbee bindings exceed the limit of 512` |
| `schedules[i].output` | `outputConflict: output '...' is owned by the Zigbee adapter` |

Bound datapoints are in the model whether the device has joined or not; the datapoint of a device
that has not joined yet stays unknown until the device reports. A generic name the image uses as
an `id` refuses the image while that device is joined (`datapoints[i].id: '...' is also a datapoint
of the provider 'zigbee'`).

Without an image service (the adapter alone) the same bindings are given to
`ZigbeeAdapter::configure()` as `ZigbeeOverride` records (`name` plus the members above, `reporting`
as a `ReportingOverride`), which checks them with the reasons `binding without a datapoint name`,
`binding with a reserved IEEE address`, `binding endpoint outside 1..240`, the function and command
reasons above, the reporting reasons above (the function, `min`, `max` and `change` ones; the types
of the record leave no room for the others), `a datapoint name is bound twice` and `a function of
an endpoint is bound twice`.

### Reporting overrides (haumacher/ehome #64)

After a device's interview (finished or incomplete) the controller configures the reporting of each
bound (or generic) function on an endpoint the device described with the defaults of the function
table in `doc/zigbee.md`; a binding of an endpoint not described (yet) is a datapoint but is
configured, bound and read only once an interview learns the endpoint (doc/zigbee.md "Incomplete
interviews"). A binding's `reporting`
replaces them for that function; each member is optional, and a missing one keeps the default. The
image carries values in the datapoint's unit; the controller keeps the defaults and the knowledge of
the attributes' raw scales.

```json
{"id": "bath.temperature", "type": "real",
 "zigbee": {"ieee": "00158d0001020304", "endpoint": 1, "cluster": 1026, "attribute": 0,
            "reporting": {"min": 30, "max": 900, "change": 0.5}}}
```

| Member | Meaning |
|---|---|
| `min` | The minimum reporting interval in seconds, an integer 0..65534. |
| `max` | The maximum reporting interval in seconds, an integer 0..65535, at least the effective `min`, except the two values ZCL (Configure Reporting) gives a meaning of their own: **0** no periodic reports, the device reports on a change only (at most every `min`); **65535** the device issues no reports of the attribute at all, its value comes only from reads (at the controller's start, after a device's silence, a rule's `read`). Neither counts toward the device's stale time (`doc/zigbee.md` "Stale rule"). |
| `change` | The reportable change in the datapoint's unit, a number ≥ 0 (0: every change is reported): °C for `temperature`, % for `level`, `humidity` and `battery`. The controller rounds it to the attribute's step: temperature and humidity × 100, battery × 2, level × 254 / 100 (at most 327.67 °C, 100 %). |

`change` is refused for the discrete functions `on_off` and `occupancy` (ZCL defines no reportable
change for boolean and bitmap types: every change is reported), and for `illuminance` and
`color_temperature`: their attributes are on a scale where a step in the datapoint's unit has no
fixed attribute step (illuminance is 10000 × log10(lux) + 1, so a fixed attribute step is a ratio of
about 12 % for the default 500, not a number of lux; colour temperature counts mireds, 1000000 / K,
so 10 mireds are 25 K at 2000 K and 400 K at 6500 K). Their `min` and `max` are allowed; the change
stays the default. `reporting` as a whole is refused where the controller configures no reporting:
a client binding (`action`, button commands), `multistate` (its reports are events) and `ias_zone`
(zone status change notifications go to the enrolled CIE address).

The defaults, in the units of the image (the export shows the effective values, below):

| Function | `min` | `max` | `change` |
|---|---|---|---|
| `on_off` | 0 | 300 | – |
| `level` | 1 | 300 | 0.4 (%, one attribute step) |
| `color_temperature` | 1 | 300 | – (10 mireds) |
| `temperature` | 30 | 600 | 0.1 |
| `humidity` | 30 | 600 | 1 |
| `illuminance` | 10 | 600 | – (attribute step 500) |
| `occupancy` | 0 | 600 | – |
| `battery` | 3600 | 21600 | 1 |

The controller configures the effective values when a device's interview finishes. When an image is
applied whose bindings change the effective reporting of a function of an interviewed device (an
override added, changed or removed, a device bound or unbound), the controller configures that
function's reporting again at once (and binds its cluster to the coordinator); a device that does
not answer (a sleepy device that misses the request, or one that refuses it) keeps its previous
configuration, and a refusal is logged, as after an interview. A function the previous bindings
did not make a datapoint (newly bound on a bound device) is configured and bound as well; a
function the new bindings drop keeps what it had. Loading the image at the controller's start configures nothing: the devices
kept their configuration.

An image without `reporting` loads as before and the devices get the defaults
(`spec/fixtures/images/zigbee-bindings.json`); the refusals and accepted forms are in the corpus
`spec/fixtures/images/cases/refuse-zigbee-reporting.json` and `accept-zigbee-reporting.json`.

## Device database

The controller keeps the joined devices in the blob `zigbee-devices.json` (at most 32 devices):

```json
{
	"format": 1,
	"devices": [
		{
			"ieee": "00158d0001020304",
			"short": 6699,
			"manufacturer": "LUMI",
			"model": "lumi.weather",
			"label": "Bathroom",
			"interviewed": true,
			"last_seen": 1792234567,
			"endpoints": [
				{"id": 1, "profile": 260, "device": 770, "in": [0, 1026, 1029, 1027], "out": [0], "zone_type": 21}
			]
		}
	]
}
```

| Member | Meaning |
|---|---|
| `format` | 1. Another value is refused (the database starts empty, the blob stays until the next write). |
| `ieee` | The device's IEEE address. |
| `short` | The last known network address (absent when unknown). |
| `manufacturer`, `model` | Basic cluster attributes 0x0004 and 0x0005 (at most 32 bytes; empty when the device did not answer). |
| `label` | The user's name for the device (`zigbee label`, at most 32 bytes). |
| `interviewed` | Whether the interview finished (endpoints, clusters and names known). |
| `missing` | Only for an incomplete interview (`interviewed` false, `endpoints` not empty): `{"endpoints": [2], "names": false}`, the endpoints whose simple descriptor did not answer (ids 1..240, at most 8) and whether the Basic cluster's manufacturer and model did not (doc/zigbee.md "Incomplete interviews"). The endpoints present are used; the device is interviewed again when it is heard. Absent otherwise; a reader drops it for an interviewed device or one without endpoints. Added for issue #47; older blobs without it load as before. |
| `last_seen` | Wall-clock seconds (Unix time) of the last frame, absent before the clock is set. Written with the next change of the database and at the latest an hour after it moved on, so after a restart it is at most an hour behind the last frame heard before it. |
| `endpoints` | At most 4: `id`, `profile` and `device` from the simple descriptor, `in`/`out` the server/client clusters (at most 12 each, the known clusters first), `zone_type` of an IAS zone (absent otherwise). |

Unknown members are ignored; a malformed database is refused as a whole and nothing of it is used.

## Export (issue #40)

`ZigbeeAdapter::write_device_export_json()`, served paged by `GET /api/zigbee/export` (spec/api.md)
and downloaded by the web UI as one file, is the contract with E-Home's importer: what the installer
pairs in the field, E-Home names, places and wires, and ships back as the bindings above.

```json
{
	"format": "homecontrol-zigbee-export", "version": 1, "controller": "homecontrol-a1b2c3", "exported": 1792231392,
	"network": {"up": true, "pan_id": 6699, "channel": 20, "extended_pan_id": "60554bfffe010203", "permit_join_s": 0},
	"devices": [
		{
			"ieee": "00158d0001020304", "manufacturer": "LUMI", "model": "lumi.weather", "label": "Bathroom", "interviewed": true,
			"endpoints": [{"id": 1, "profile": 260, "device": 770, "in": [0, 3, 1026, 1029, 1027], "out": [], "zone_type": 21}],
			"functions": [
				{"name": "zigbee.00158d0001020304.1.temperature", "function": "temperature", "endpoint": 1, "cluster": 1026,
				 "attribute": 0, "type": "real", "unit": "°C", "writable": false,
				 "reporting": {"min": 30, "max": 900, "change": 0.5}, "datapoint": "bath.temperature"},
				{"name": "zigbee.00158d0001020304.1.humidity", "function": "humidity", "endpoint": 1, "cluster": 1029,
				 "attribute": 0, "type": "real", "unit": "%", "writable": false,
				 "reporting": {"min": 30, "max": 600, "change": 1}}
			]
		}
	]
}
```

- `interviewed`: whether the interview finished. `false` with `endpoints` present is an incomplete
  interview: `missing` (as in the device database: `{"endpoints": [2], "names": false}`) names the
  endpoints that did not answer and whether the names did not; the endpoints and functions listed
  are real but the list is not final (the controller retries when the device is heard). An
  importer keeps what it has and shows the device as incomplete; a later export brings the rest.
  `missing` is absent for an interviewed device and one never interviewed (no endpoints). Added
  for issue #47 (`version` stays 1: an added member).
- `endpoints`: the interview result as kept in the database (`ZigbeeEndpoint`: at most 4 endpoints,
  12 clusters per direction; `zone_type` only for an IAS Zone endpoint whose type was read).
- `functions`: every function the mapping (`doc/zigbee.md`, the function table) derives from the
  endpoints, bound or not: `name` the generic datapoint name, `function` the table's key, `cluster`
  and `attribute` the binding's `cluster`/`attribute` (no `attribute` for `action`, a client
  binding), `type` the value type a binding's `type` must fit, `unit` where the function has one,
  `writable` whether a binding may be commanded, `reporting` the reporting the controller uses for
  it, and `datapoint` the name the loaded model gives it (the image's binding, else the generic
  name; absent while no model is loaded, and for a function of a bound device that no binding names:
  such a device has only its bound datapoints).
- `reporting`: `{"min": s, "max": s, "change": x}`, the effective values (the function table's
  defaults, or the loaded image's override, "Reporting overrides" above) in the image's units, so
  the same members written back into a binding change nothing. `change` is absent where a binding
  cannot set it (`on_off`, `occupancy`, `illuminance`, `color_temperature`); `level`'s change is
  rounded to one decimal (one attribute step is 0.39 %). `reporting` is absent for `action`,
  `multistate` and `ias_zone`, which have no reporting configured.
- `version` counts incompatible changes of this object; a reader refuses a higher one.

## Adapter view

`ZigbeeAdapter::write_json()` (the console, the API's pairing view):

```json
{
	"network": {"up": true, "pan_id": 6699, "channel": 20, "extended_pan_id": "60554bfffe010203", "permit_join_s": 0},
	"devices": [
		{
			"ieee": "00158d0001020304",
			"short": 6699,
			"manufacturer": "LUMI",
			"model": "lumi.weather",
			"label": "Bathroom",
			"interviewed": true,
			"stale": false,
			"left": false,
			"interviewing": false,
			"last_seen": 1792234567,
			"seen_ago_s": 12,
			"datapoints": ["zigbee.00158d0001020304.1.temperature", "zigbee.00158d0001020304.1.humidity"]
		}
	]
}
```

`pan_id`, `channel` and `extended_pan_id` are present while the network is up; `permit_join_s` is
the remaining time the network is open for joining (0: closed). `datapoints` lists the names of the
device's datapoints in the model (empty while they are not loaded). On a controller without Zigbee
the view is `{"available": false, "reason": "..."}`.
`left`: the device announced its leave (it is kept; a rejoin clears it); `interviewing`: its
interview runs now; `seen_ago_s`: seconds since it was last heard (absent before it was heard since
the start). For an incomplete interview (doc/zigbee.md "Incomplete interviews"; `interviewed` false
with datapoints) the device also has `missing` (as in the device database) and `incomplete`, the
English reason, e.g. `"incomplete": "endpoint 2 did not answer"` or `"endpoints 2, 3 did not answer;
manufacturer and model did not answer"`; a UI translates from `missing`. `GET /api/zigbee` (spec/api.md) pages the same device objects with `write_device_json()`
and the network with `write_network_json()`.
