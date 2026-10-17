# External values: weather and other HTTP services as datapoints

Issue #17. `hc::ExternalValues` (core, `core/include/hc/external_values.h`) is a technology that
owns application datapoints fed from HTTP JSON sources: the weather from Open-Meteo, a PV
inverter's local API, any service that answers a GET with JSON. The sources are configuration (the
runtime image's `external` member, `spec/external-values.md`), not code. Rules and the UI see only
datapoints (ROADMAP: everything is a datapoint): "close the blinds above 25 °C outside" is a rule
on `weather.temperature`.

## Core and platform

| Who | What |
|---|---|
| core: `ExternalValues` | The sources and bindings, when to fetch, the conditions, the backoff, parsing (`json_parse`, `JsonSpan`), extraction, conversion, reporting, the stale rule, the refusals. |
| platform: `hc::HttpFetcher` (`core/include/hc/platform/http_fetcher.h`) | One GET at a time: `fetch(url, max_bytes, receiver, error)`; the outcome (`FetchResult`: ok, HTTP status, body, reason) arrives on the event loop's thread, never from within `fetch()`. |
| ESP: `hc::esp::EspHttpFetcher` (component `hc_external`) | esp_http_client with the IDF certificate bundle, a short-lived worker task per fetch (8 KB stack, the OTA update check's pattern), an 8 KB response buffer, the outcome posted through the `LoopQueue`. |
| POSIX: `hc::posix::HttplibFetcher` | cpp-httplib's client on a worker thread, the outcome through the web server's `LoopExecutor`. HTTPS only when the build finds OpenSSL's development files (CMake prints which); otherwise `https://` is refused with that reason and `http://` works. |
| tests: `FakeFetcher` (`core/test/external_values_test.cpp`) | Records the requests; the test answers them. |

## When a source is fetched

- **Grid and stagger.** After `configure()` source *i* is first fetched `kStartDelayMs` (5 s) plus
  *i* × `kStaggerMs` (20 s) later, then every `interval` (300..86400 s, default 900) on that grid.
  A source's grid does not drift with the fetch duration.
- **One at a time.** Only one fetch is in flight; a source that comes due meanwhile waits for the
  result and then runs. Several sources never burst onto the network.
- **Conditions.** Nothing is fetched while the network is down, the wall clock is not synchronized
  (TLS checks the certificates' dates) or, for a URL with `{lat}`/`{lon}`, no valid location is set
  (`location.lat`, `location.lon`, `doc/time.md`). A due time that passes so is a *missed* fetch
  (logged at info level with the reason, counted like a failure, no backoff); the fetch follows
  `kStartDelayMs` (plus the stagger) after the condition clears. The conditions are polled every
  `kReadyPollMs` (30 s) only while a source waits for them: the Connectivity's listener table is
  full on the device (KNX tunnel, Improv, OTA, network services), and the time service's should
  stay free for the rule side.
- **Backoff.** A failed fetch is retried after 1 minute, then 2, 4, 8 … up to 60 minutes
  (`hc::Backoff`, `kRetryInitialMs`, `kRetryCapMs`); the first success resets it and puts the source
  back on its grid. A fetch without a result after `kFetchTimeoutMs` (120 s; the platforms time out
  after 15 s) counts as failed; a result arriving later is ignored.
- **On request.** `fetch_now(id)` (console `external fetch <id>`) fetches a source at once, after a
  fetch in flight; refused within a minute of its last attempt (rate limit) and while a condition
  is missing, with the reason.

## A fetch

A failure is anything of: the fetch could not start (busy, URL), no connection or TLS failure, an
HTTP status other than 200, a body larger than 8192 bytes (the reason names the limit), a body
that is not JSON, a path that is missing, an index out of range, a value of the wrong kind
(`null`, an object, a string for a number datapoint, a number out of an integer datapoint's range).
Every binding is extracted and converted before any is reported: a response reports all its values
with quality good, or none (values of one response belong together).

The reason of a failure is logged (tag `hc.ext`) when it differs from the previous one, so an
outage is one line, not one per retry. After `kStaleAfterFailures` (2) consecutive failed or missed
fetches the source's datapoints become stale (`report_stale`, the last value stays) with one log
line; the next success makes them good again and logs "recovered after N failure(s)".

Commands to these datapoints are refused: the model refuses a non-writable datapoint
(`not_writable`), and `ExternalValues::command()` refuses a writable transmitting one with a log line
("an external value … is read only"). A writable datapoint without transmit is refused at
configuration, because the model would store its commands without asking the owner.

## Configuration

`configure(specs, count)` (programmatic) and `configure_json(text, length)` (the image's member) check
everything before anything changes; a refusal logs and returns the reason and keeps the previous
configuration. The datapoints are resolved by name and bound at configuration: call it again after
every model load. A configuration made while a fetch is in flight drops that fetch's result.

The Open-Meteo preset expands into an ordinary source: its URL asks only for the variables the
bindings use, each variable becomes its JSON path and default unit (`spec/external-values.md` lists
them). The location goes out with two decimals (about 1 km): what a weather grid resolves, and no
more of the house's position.

## Memory

`configure()` allocates one table (sources, bindings, the values of one response) of 11,216 bytes on
the ESP32 at the capacities of 8 sources and 32 bindings, and frees the previous table (during a
reconfiguration both exist); nothing is allocated in steady state. The `ExternalValues` object itself
is 640 bytes; it uses one event-loop timer. The ESP fetcher has its buffers in the object (about
8.7 KB, static); each fetch creates a worker task (8 KB stack) and esp_http_client with
mbedTLS allocates its TLS buffers for the duration of the request, as the OTA update check does.

## Device wiring and console

`hc::esp::ExternalService` (component `hc_external`) owns the fetcher and the `ExternalValues`, wired
to the controller's loop, time service, settings and connectivity and to the datapoint model; the
image loader (issue #8) calls `values().configure_json()`. Console:

```
hc> external status
weather: preset open-meteo every 900 s, 12 fetch(es), 0 failure(s) in a row
  url   https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&current=temperature_2m&...
  last  ok 312 s ago
  next  in 588 s
  weather.temperature          10.4       °C    good    <- current.temperature_2m
hc> external fetch weather
```

On POSIX the fetcher is built into the web library; the development loop without OpenSSL serves
the captured response locally:

```
python3 -m http.server 8000 --directory spec/fixtures/external
# a source with "url": "http://127.0.0.1:8000/open-meteo-example.json"
```
