# External values in the runtime image

The runtime image's member `external` declares HTTP JSON sources whose values feed datapoints
(issue #17; the device side is `doc/external.md`). The image loader (issue #8) hands the member's
JSON to `hc::ExternalValues::configure_json()` after the datapoint model is loaded. The member is
optional; an image without it has no external values.

Example (`spec/fixtures/external/image-external.json`, tested in `core/test/external_values_test.cpp`):

```json
{
	"sources": [
		{
			"id": "weather",
			"preset": "open-meteo",
			"interval": 900,
			"bindings": [
				{"datapoint": "weather.temperature", "variable": "temperature"},
				{"datapoint": "weather.temperature_3h", "variable": "temperature_3h"}
			]
		},
		{
			"id": "pv",
			"url": "http://pv.example/api/status.json",
			"interval": 300,
			"bindings": [
				{"datapoint": "pv.power", "path": "inverters[0].ac.power", "scale": 0.001, "unit": "kW"},
				{"datapoint": "pv.online", "path": "online"}
			]
		}
	]
}
```

## `external`

| Member | Type | Required | Meaning |
|---|---|---|---|
| `sources` | array of source objects | yes | At most 8 sources, at most 32 bindings in all. |

## Source

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | string | yes | 1..15 characters of `[A-Za-z0-9._-]`, unique. Names the source in the log, the console and the status. |
| `url` | string | one of `url`, `preset` | `http://` or `https://`, a host, no spaces, quotes, backslashes, control or non-ASCII characters (percent-encode them). The placeholders `{lat}` and `{lon}` are replaced by the device's location (settings `location.lat`, `location.lon`, two decimals); no other `{...}`. At most 383 bytes after filling. |
| `preset` | string | one of `url`, `preset` | A built-in source; the device builds its URL from the variables the bindings name. Known: `open-meteo`. |
| `interval` | integer, seconds | no (900) | 300..86400. Five minutes at least: the services' rate limits. |
| `bindings` | array of binding objects | yes | At least one. |

## Binding

| Member | Type | Required | Meaning |
|---|---|---|---|
| `datapoint` | string | yes | The datapoint's name in the image. Its type is real, signed, unsigned, enumeration, boolean or string; it is not owned by another technology, not bound twice, and not writable without transmit (commands to external values are refused, see below). |
| `path` | string | with `url` | Where the value is in the response: member names separated by `.`, array indexes as `[n]` (`current.temperature_2m`, `hourly.precipitation[3]`, `[0].value` for a top-level array). At most 63 bytes. A member name cannot contain `.`, `[`, `]`, spaces, quotes or backslashes. |
| `variable` | string | with `preset` | The preset's variable (table below). |
| `scale` | number | no (1) | Not 0. value = JSON number × scale + offset. |
| `offset` | number | no (0) | |
| `unit` | string | no | At most 15 bytes, for display only (the model has no units); a preset variable has a default. |

Unknown members are refused with their name (a typo must not pass silently); so is every violation
above, and the device keeps its previous configuration.

### Values

A JSON number feeds a real datapoint as is (after scale and offset) and a signed, unsigned or
enumeration datapoint rounded to the nearest integer within its range. `true`/`false` feed a
boolean datapoint, a string (at most 28 bytes) a string datapoint. Anything else — `null`, an
object or array, a number for a boolean, a missing member, an index out of range — fails the fetch:
the device reports either all values of a response or none.

## Preset `open-meteo`

The [Open-Meteo](https://open-meteo.com/) forecast API, free for non-commercial use, no key. URL
(the `current` and `hourly` lists hold only what the bindings use; `forecast_hours` is the furthest
hour plus one):

```
https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&current=<...>&hourly=<...>&forecast_hours=7&wind_speed_unit=ms&timezone=UTC
```

`current` holds the last 15-minute step; `hourly` starts at the current hour, so index n is n hours
ahead (`spec/fixtures/external/open-meteo-example.json` is a captured response).

| Variable | Path | Unit | Datapoint type |
|---|---|---|---|
| `temperature` | `current.temperature_2m` | °C | real |
| `apparent_temperature` | `current.apparent_temperature` | °C | real |
| `humidity` | `current.relative_humidity_2m` | % | real or unsigned |
| `precipitation` | `current.precipitation` | mm (last 15 min) | real |
| `rain` | `current.rain` | mm (last 15 min) | real |
| `wind_speed` | `current.wind_speed_10m` | m/s | real |
| `wind_direction` | `current.wind_direction_10m` | ° | real or unsigned |
| `cloud_cover` | `current.cloud_cover` | % | real or unsigned |
| `pressure` | `current.pressure_msl` | hPa | real |
| `weather_code` | `current.weather_code` | WMO code (0 clear … 99 thunderstorm with hail) | enumeration or unsigned |
| `temperature_1h`, `temperature_3h`, `temperature_6h` | `hourly.temperature_2m[1]`, `[3]`, `[6]` | °C | real |
| `precipitation_1h`, `precipitation_3h`, `precipitation_6h` | `hourly.precipitation[1]`, `[3]`, `[6]` | mm (the hour before) | real |

Sunrise and sunset are not taken from here: the device computes them (`doc/time.md`).

## On the device

The datapoints of a source are owned by the technology `external`: commands to them are refused
("an external value"). A value has quality good after a fetch; after two consecutive failed or
missed fetches of its source it becomes stale (the last value stays). `doc/external.md` has the
fetch schedule, the conditions and the backoff.
