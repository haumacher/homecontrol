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

## Bindings (runtime image, issue #8)

A runtime image may bind a device's functions to datapoint names of its own. A device with at least
one binding gets exactly its bound datapoints (its unbound functions get none); a device without
bindings gets the generic datapoints. The image carries the bindings as a list of objects:

```json
{
	"name": "living.ceiling",
	"ieee": "00158d0001020304",
	"endpoint": 11,
	"cluster": 6,
	"attribute": 0,
	"client": false,
	"command": 6
}
```

| Member | Meaning |
|---|---|
| `name` | The datapoint name (the rules of `doc/datapoints.md`). Required, unique among all bindings. |
| `ieee` | The device. Not 0 and not all ones. |
| `endpoint` | 1..240. |
| `cluster`, `attribute` | Select the function (the cluster/attribute pairs of the function table). |
| `client` | `true`: the device's client cluster commands (a button: `cluster` 6 or 8), the `action` function; `attribute` and `command` are then ignored and must not command. Default `false`. |
| `command` | The cluster commands go to; absent: read-only. Only the function's own cluster, or Level Control (8) for `on_off` (cluster 6), is accepted. |

The controller checks the whole list before it changes anything (`ZigbeeAdapter::configure()`): at
most 512 bindings, every binding maps a known function, no name and no (device, endpoint,
function) appears twice. A refusal names the reason:

- `binding without a datapoint name`
- `binding with a reserved IEEE address`
- `binding endpoint outside 1..240`
- `a client binding (button commands) needs cluster 6 (On/Off) or 8 (Level Control)`
- `a client binding (button commands) cannot be commanded`
- `no known function for this cluster and attribute (doc/zigbee.md lists them)`
- `the command cluster cannot set this attribute`
- `a datapoint name is bound twice`
- `a function of an endpoint is bound twice`

Bound datapoints are loaded whether the device has joined or not; the datapoint of a device that
has not joined yet stays unknown until the device reports.

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
			"last_seen": 1792234567,
			"datapoints": ["zigbee.00158d0001020304.1.temperature", "zigbee.00158d0001020304.1.humidity"]
		}
	]
}
```

`pan_id`, `channel` and `extended_pan_id` are present while the network is up; `permit_join_s` is
the remaining time the network is open for joining (0: closed). `datapoints` lists the names of the
device's datapoints in the model (empty while they are not loaded). On a controller without Zigbee
the view is `{"available": false, "reason": "..."}`.
