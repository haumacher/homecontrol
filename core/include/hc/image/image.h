/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_IMAGE_H
#define HC_IMAGE_IMAGE_H

#include <cstddef>
#include <cstdint>

#include "hc/log.h"

namespace hc {
namespace image {

/// The runtime image (spec/runtime-image.md): what E-Home deploys, as one JSON document.

/// The schema version this firmware reads, the only place it is set: major 1, minor 2 (1.1 adds the
/// control pages `ui.pages`, issue #54; 1.2 the adjustable schedule members, issue #58, and the
/// history archives, issue #56). An image with a higher major version is refused (named in the
/// report); a higher minor version loads (its additions are ignorable), except where a strict
/// object (rules, schedules) meets a member this firmware does not know: that refusal says the
/// image is newer than the firmware reads (spec/runtime-image.md I4).
constexpr uint32_t kSchemaMajor = 1;
constexpr uint32_t kSchemaMinor = 2;

/// "1.2": kSchemaMajor.kSchemaMinor as text (the API's `schema`, spec/api.md).
const char* schema_version_text();

/// Largest image in bytes. The device reads the whole text into RAM once at load (the parse works
/// in place on it), so a load needs this much free heap in one block on top of the tables.
constexpr size_t kMaxImageBytes = 262144;

/// Nesting the image may use: rule expressions are 12 deep, a call adds an array level each.
constexpr size_t kMaxNesting = 48;

/// Limits of one image beyond those of the components (rules: spec/rules.md 10.2; schedules:
/// spec/schedules.md 6; history: doc/persistence.md).
constexpr size_t kMaxDatapoints = 1024;
constexpr size_t kMaxLocations = 256;
constexpr size_t kMaxFunctions = 512;

/// Limits of the control pages (`ui.pages`, spec/runtime-image.md 3.9 and 5), over all pages.
constexpr size_t kMaxPages = 16;
constexpr size_t kMaxSections = 128;
constexpr size_t kMaxElements = 512;   ///< Function, datapoint and chart elements.
constexpr size_t kMaxSeries = 256;     ///< The series of all charts.
constexpr size_t kMaxReferences = 128; ///< The reference lines of all charts.
/// Longest page id and icon name (A-Z a-z 0-9 _ . -).
constexpr size_t kMaxPageId = 32;
constexpr size_t kMaxIcon = 31;

/// The time ranges of a chart (`range` and `presets`): 1 h, 24 h, 7 d, 30 d, 1 y.
constexpr size_t kChartRanges = 5;
/// "1h", "24h", "7d", "30d", "1y" for 0..4; nullptr beyond.
const char* chart_range_name(size_t range);
/// The range's length in seconds (a year is 365 days).
uint32_t chart_range_seconds(size_t range);
/// The index of a range name; -1 if unknown.
int chart_range_of(const char* name);

/// How a chart series reduces the samples of a bucket.
enum class Consolidation : uint8_t {
	mean,        ///< The mean line (default for a real or boolean: the on-share of a boolean strip).
	last,        ///< The bucket's last value (default for unsigned and signed).
	band,        ///< The mean line with the min/max band (a band only on a chart of a single series).
	consumption, ///< Bars of the delta between consecutive buckets' last values (meter readings).
};
/// "mean", "last", "band", "consumption".
const char* consolidation_name(Consolidation consolidation);
/// The consolidation of a name; false if unknown.
bool consolidation_of(const char* name, Consolidation& out);

/// Longest texts kept from the `image` member (bytes, without the NUL).
constexpr size_t kMaxProjectId = 63;
constexpr size_t kMaxName = 63;
constexpr size_t kMaxGenerator = 63;
constexpr size_t kMaxGeneratedAt = 31;
constexpr size_t kMaxVersionText = 15;

/// Where a load came from (logged; the status shows it).
enum class Source : uint8_t {
	boot,    ///< The stored image, at start.
	api,     ///< PUT /api/image.
	console, ///< The console's `image load <url>`.
	file,    ///< The POSIX build's --image <file>.
	restore, ///< The stored image again, after a new one failed while it was applied.
};

/// "boot", "api", "console", "file", "restore".
const char* source_name(Source source);

/// The metadata of an image (its `image` member) and what it holds.
struct ImageInfo {
	char schema_version[kMaxVersionText + 1] = {};
	char project_id[kMaxProjectId + 1] = {};
	uint32_t revision = 0;
	char generated_at[kMaxGeneratedAt + 1] = {};
	char generator[kMaxGenerator + 1] = {};
	char name[kMaxName + 1] = {};
	size_t bytes = 0;      ///< Size of the JSON text.
	size_t datapoints = 0; ///< In the model, with those the image implies (the time master's).
	size_t knx_datapoints = 0;
	size_t zigbee_datapoints = 0; ///< With a `zigbee` binding.
	size_t rules = 0;
	size_t schedules = 0;
	size_t locations = 0;
	size_t functions = 0;
	size_t history_rings = 0;
	bool time_master = false;
};

/// The outcome of a validation or load: every refusal with its JSON path and reason, up to
/// kMaxRefusals (`total` counts all; the validation stops at kMaxTotal). The first refusal is held
/// inline; the room for the others (kMaxRefusals, about 3 KB) is allocated with the second and freed
/// by clear(), so a report without refusals (every successful load) costs one refusal. Without the
/// heap for the others only the first is kept and the others are counted.
struct Report {
	static constexpr size_t kMaxRefusals = 12;
	static constexpr size_t kMaxTotal = 100;
	static constexpr size_t kMaxPath = 95;
	static constexpr size_t kMaxReason = 159;

	struct Refusal {
		char path[kMaxPath + 1];
		char reason[kMaxReason + 1];
	};

	/// What kind of failure: none, the JSON does not parse, the schema version is newer, the
	/// content is invalid, the device cannot hold it (memory), applying it failed, or the device
	/// holds the installation key and the image comes without a signature or with one that does
	/// not verify (issue #35, spec/api.md "Signed runtime images"), or the previous image is still
	/// being stored (homecontrol #68: nothing was looked at; deploy again when the store is done).
	enum class Kind : uint8_t {
		none, syntax, version, invalid, too_large, no_memory, apply, unsigned_image, bad_signature, busy
	};

	Report() = default;
	~Report() { delete[] more_; }
	Report(const Report&) = delete;
	Report& operator=(const Report&) = delete;

	Kind kind = Kind::none;
	/// The `count` refusals kept (the inline one or the allocated room).
	const Refusal* refusals = &first_;
	size_t count = 0;
	size_t total = 0;

	bool ok() const { return kind == Kind::none; }
	/// Empties the report and frees the room of the further refusals.
	void clear() {
		kind = Kind::none;
		count = 0;
		total = 0;
		delete[] more_;
		more_ = nullptr;
		refusals = &first_;
	}
	/// Adds a refusal (kept if there is room; counted always) and sets `kind` if none is set.
	void add(Kind failure, const char* path, const char* format, ...) HC_PRINTF_FORMAT(4, 5);
	bool full() const { return total >= kMaxTotal; }

private:
	Refusal first_ = {};
	Refusal* more_ = nullptr;
};

/// "none", "syntax", "version", "invalid", "too large", "no memory", "apply", "unsigned", "bad signature",
/// "busy".
const char* report_kind_name(Report::Kind kind);

} // namespace image
} // namespace hc

#endif // HC_IMAGE_IMAGE_H
