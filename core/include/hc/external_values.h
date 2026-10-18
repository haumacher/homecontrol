/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_EXTERNAL_VALUES_H
#define HC_EXTERNAL_VALUES_H

#include <cstddef>
#include <cstdint>

#include "hc/connectivity.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/json.h"
#include "hc/log.h"
#include "hc/platform/http_fetcher.h"
#include "hc/platform/settings.h"
#include "hc/technology.h"
#include "hc/time_service.h"

namespace hc {

/// One datapoint fed from a source: where its value is in the response and how it is scaled.
struct ExternalBindingSpec {
	/// The datapoint's name in the model.
	const char* datapoint = nullptr;
	/// For a source with a URL: the JSON path of the value in the response, dotted member names and
	/// `[n]` array indexes ("current.temperature_2m", "hourly.precipitation[3]", "[0].value").
	const char* path = nullptr;
	/// For a preset source: the preset's variable ("temperature", "temperature_3h"); see
	/// open_meteo_variables().
	const char* variable = nullptr;
	/// value = json * scale + offset (numbers only). Scale must not be 0.
	double scale = 1;
	double offset = 0;
	/// For the status display (the model has no units); nullptr: the preset's unit, or none.
	const char* unit = nullptr;
};

/// One HTTP JSON source (doc/external.md, spec/external-values.md).
struct ExternalSourceSpec {
	/// 1..ExternalValues::kMaxId characters of [A-Za-z0-9._-], unique.
	const char* id = nullptr;
	/// http:// or https://; `{lat}` and `{lon}` are filled from the location settings. Exactly one
	/// of url and preset.
	const char* url = nullptr;
	/// A built-in source ("open-meteo"): its URL is built from the variables the bindings use.
	const char* preset = nullptr;
	/// Seconds between two fetches, ExternalValues::kMinIntervalS..kMaxIntervalS.
	uint32_t interval_s = 900;
	const ExternalBindingSpec* bindings = nullptr;
	size_t binding_count = 0;
};

/// What the console and the UI show about a source.
struct ExternalSourceStatus {
	const char* id = "";
	const char* url = "";    ///< The URL template (placeholders unfilled).
	const char* preset = ""; ///< "" for a source with its own URL.
	uint32_t interval_s = 0;
	size_t bindings = 0;
	bool fetching = false;   ///< Its fetch is in flight.
	uint32_t failures = 0;   ///< Consecutive failed (or missed) fetches.
	uint32_t fetches = 0;    ///< Successful fetches since configure().
	uint64_t next_ms = 0;    ///< Monotonic time of the next attempt.
	uint64_t last_ok_ms = 0; ///< Monotonic time of the last success, 0 before the first.
	const char* last_error = ""; ///< Why the last attempt failed; "" after a success.
};

/// One binding of a source, for the status display.
struct ExternalBindingStatus {
	uint16_t datapoint = 0;
	const char* path = ""; ///< The JSON path (a preset variable is shown resolved).
	const char* unit = "";
	double scale = 1;
	double offset = 0;
};

/// A variable of a preset: its name in the image, its JSON path in the response, its unit.
struct ExternalPresetVariable {
	const char* name;
	const char* path;
	const char* unit;
	const char* query; ///< The Open-Meteo parameter it needs.
	bool hourly;       ///< In `hourly=` (else `current=`).
	uint8_t hour;      ///< Hours ahead for an hourly variable.
};

/// The variables of the preset "open-meteo"; `count` receives their number.
const ExternalPresetVariable* open_meteo_variables(size_t& count);

/// Checks a JSON path's syntax (the rules of ExternalBindingSpec::path, at most
/// ExternalValues::kMaxPath bytes). On refusal `error` names the problem.
bool external_path_valid(const char* path, const char** error = nullptr);

/// The value at `path` inside `root` (a valid path). False with `error` (static text) when a
/// member is missing, an index is out of range or a step meets the wrong kind of value; `failed_at`
/// (if given) receives the offset in `path` of the step that failed.
bool external_extract(const JsonSpan& root, const char* path, JsonSpan& out, const char** error = nullptr,
	size_t* failed_at = nullptr);

/// The external values technology (issue #17, doc/external.md): owns application datapoints fed
/// from HTTP JSON sources (weather from Open-Meteo, a PV inverter's API, any service that answers
/// GET with JSON).
///
/// The core decides when: each source is fetched every `interval_s` on a grid that starts
/// kStartDelayMs after configure() plus kStaggerMs per source (sources never burst); only one fetch
/// is in flight at a time (a source that comes due meanwhile waits for it). Nothing is fetched while
/// the network is down, the wall clock is not synchronized (TLS checks certificate dates) or, for a
/// URL with `{lat}`/`{lon}`, no location is set: such a due time counts as a missed fetch, and the
/// fetch follows kStartDelayMs after the condition clears (polled every kReadyPollMs while waiting:
/// the Connectivity's listener table is full on the device). A failed fetch is retried after a
/// Backoff of kRetryInitialMs doubling up to kRetryCapMs, then the grid resumes.
///
/// The platform's HttpFetcher does the request; the response is parsed with json_parse() and every
/// binding extracted before any is reported: a fetch reports all its values with quality good, or
/// none. A failure (network, HTTP status, a body too large, not JSON, a path missing, a value of the
/// wrong kind) is logged when its reason changes; after kStaleAfterFailures consecutive failures the
/// source's datapoints become stale; the next success makes them good again (logged).
///
/// Commands to these datapoints are refused ("an external value"). A read (read(), a rule's `read`
/// action) fetches the datapoint's source again at its next opportunity: at once, or when the rate
/// limit of fetch_now() (kManualSpacingMs since its last attempt) allows; it is refused while the
/// source is blocked (network down, wall clock not synchronized, no location for its URL).
///
/// Memory: configure() allocates one table for the sources and bindings (11,216 bytes at the
/// capacities below on the ESP32) and frees the previous one; nothing is allocated afterwards.
/// Threading: the event loop's thread.
class ExternalValues : public Technology, public FetchReceiver {
public:
	static constexpr uint32_t kMinIntervalS = 300;
	static constexpr uint32_t kDefaultIntervalS = 900;
	static constexpr uint32_t kMaxIntervalS = 86400;

	static constexpr size_t kMaxSources = 8;
	/// Bindings of all sources together.
	static constexpr size_t kMaxBindings = 32;
	static constexpr size_t kMaxId = 15;
	static constexpr size_t kMaxPath = 63;
	static constexpr size_t kMaxUnit = 15;
	/// Longest URL template (the filled URL must fit HttpFetcher::kMaxUrl).
	static constexpr size_t kMaxUrl = HttpFetcher::kMaxUrl;

	/// Largest response body accepted.
	static constexpr size_t kMaxBody = 8192;

	/// First fetch of the first source after configure() or after the conditions cleared.
	static constexpr uint32_t kStartDelayMs = 5000;
	/// Offset between the first fetches of consecutive sources.
	static constexpr uint32_t kStaggerMs = 20000;
	/// How often the conditions are checked while a source waits for them.
	static constexpr uint32_t kReadyPollMs = 30000;
	/// Retry delays after a failure: 1, 2, 4, ... minutes up to an hour.
	static constexpr uint32_t kRetryInitialMs = 60000;
	static constexpr uint32_t kRetryCapMs = 3600000;
	/// A fetch without a result after this long counts as failed (the platform's own timeouts are
	/// shorter; this guards against a lost result).
	static constexpr uint32_t kFetchTimeoutMs = 120000;
	/// Consecutive failures that make the source's datapoints stale.
	static constexpr uint32_t kStaleAfterFailures = 2;
	/// Least time between two attempts of a source requested with fetch_now().
	static constexpr uint32_t kManualSpacingMs = 60000;

	ExternalValues(EventLoop& loop, const TimeSource& time, const Settings& settings, const Connectivity& connectivity,
		DatapointModel& model, HttpFetcher& fetcher);
	~ExternalValues();

	ExternalValues(const ExternalValues&) = delete;
	ExternalValues& operator=(const ExternalValues&) = delete;

	/// Takes the sources. Checks everything before anything changes: ids, URLs and their
	/// placeholders, presets and their variables, intervals, paths, scales and units; each
	/// datapoint exists, has a type an external value can feed (real, signed, unsigned,
	/// enumeration, boolean, string), is owned by nobody else, is bound only once, and is not
	/// writable without kTransmit (its commands would bypass the refusal). A refusal (false, the
	/// reason in `error` and in the log) keeps the previous configuration. Binds the datapoints and
	/// schedules the first fetches. Call again after every model load (the ids are resolved here).
	bool configure(const ExternalSourceSpec* specs, size_t count, const char** error = nullptr);

	/// The same from the runtime image's `external` member (spec/external-values.md):
	/// `{"sources": [{"id": ..., "url"|"preset": ..., "interval": ..., "bindings": [...]}]}`.
	bool configure_json(const char* text, size_t length, const char** error = nullptr);

	/// Forgets the sources (their datapoints stay bound until the next model load; their commands
	/// stay refused). A fetch in flight is dropped when its result arrives.
	void clear();

	/// Configured sources.
	size_t size() const;

	/// The state of source `index`; false if there is none.
	bool status(size_t index, ExternalSourceStatus& out) const;

	/// Binding `binding` of source `index`; false if there is none.
	bool binding(size_t index, size_t binding, ExternalBindingStatus& out) const;

	/// Fetches source `id` now (after a fetch in flight). Refused (false, `error`) for an unknown
	/// id, while the conditions are missing (the reason), and within kManualSpacingMs of its last
	/// attempt (the service's rate limit).
	bool fetch_now(const char* id, const char** error = nullptr);

	/// Whether a fetch is in flight.
	bool fetching() const { return in_flight_; }

	// Technology
	const char* name() const override { return "external"; }
	bool command(const Datapoint& datapoint, const Value& value) override;
	bool read(const Datapoint& datapoint) override;

	// FetchReceiver
	void fetch_done(const FetchResult& result) override;

private:
	struct Table;
	struct Source;

	bool fill(Table& table, const ExternalSourceSpec* specs, size_t count);
	bool fill_json(Table& table, const char* text, size_t length);
	bool finish(Table& table);
	bool commit(Table* table, const char** error);
	bool refuse(const char** error);
	bool reject(const char* fmt, ...) HC_PRINTF_FORMAT(2, 3);

	void timer_due();
	void service();
	void arm();
	const char* source_blocked(const Source& source, uint64_t now) const;
	bool start(Source& source, uint64_t now);
	void process(Source& source, const char* body, size_t length);
	void succeed(Source& source, uint64_t now);
	void fail(Source& source, const char* reason, bool retry);
	uint64_t next_grid(const Source& source, uint64_t now) const;
	bool fill_url(const Source& source, char* out, size_t size, const char** error) const;

	EventLoop& loop_;
	const TimeSource& time_;
	const Settings& settings_;
	const Connectivity& connectivity_;
	DatapointModel& model_;
	HttpFetcher& fetcher_;

	Table* table_ = nullptr;
	uint32_t generation_ = 0;
	TimerHandle timer_;
	bool in_flight_ = false;
	uint32_t in_flight_generation_ = 0;
	size_t in_flight_source_ = 0;
	uint64_t in_flight_deadline_ = 0;
	char url_[kMaxUrl + 1] = {};
	char error_[192] = {};
};

} // namespace hc

#endif // HC_EXTERNAL_VALUES_H
