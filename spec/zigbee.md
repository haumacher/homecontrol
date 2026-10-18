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
| `client` | `true`: the device's client cluster commands (a button: `cluster` 6 or 8), the `action` function; `attribute` is then ignored and `command` not allowed. Default `false`. |
| `command` | The cluster commands go to (0..65534); absent: read-only. Only the function's own cluster, or Level Control (8) for `on_off` (cluster 6), is accepted. |

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
| `datapoints` | `N Zigbee bindings exceed the limit of 512` |
| `schedules[i].output` | `outputConflict: output '...' is owned by the Zigbee adapter` |

Bound datapoints are in the model whether the device has joined or not; the datapoint of a device
that has not joined yet stays unknown until the device reports. A generic name the image uses as
an `id` refuses the image while that device is joined (`datapoints[i].id: '...' is also a datapoint
of the provider 'zigbee'`).

Without an image service (the adapter alone) the same bindings are given to
`ZigbeeAdapter::configure()` as `ZigbeeOverride` records (`name` plus the members above), which
checks them with the reasons `binding without a datapoint name`, `binding with a reserved IEEE
address`, `binding endpoint outside 1..240`, the function and command reasons above, `a datapoint
name is bound twice` and `a function of an endpoint is bound twice`.

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
| `interviewed` | Whether the interview finished (endpoints known). |
| `last_seen` | Wall-clock seconds (Unix time) of the last frame, absent before the clock is set. |
| `endpoints` | At most 4: `id`, `profile` and `device` from the simple descriptor, `in`/`out` the server/client clusters (at most 12 each, the known clusters first), `zone_type` of an IAS zone (absent otherwise). |

Unknown members are ignored; a malformed database is refused as a whole and nothing of it is used.

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
the start). `GET /api/zigbee` (spec/api.md) pages the same device objects with `write_device_json()`
and the network with `write_network_json()`.
