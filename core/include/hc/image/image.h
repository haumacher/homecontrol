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

/// The runtime image (spec/runtime-image.md): what E-Home deploys, compiled into the binary image
/// the device keeps in its slot and works from (sections 9 to 13, issue #87; hc/image/binary.h).

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
	boot,    ///< The slot's image, at start.
	api,     ///< PUT /api/image (HTTP or the E-Home connection).
	console, ///< The console's `image load <url>`.
	file,    ///< The POSIX build's --image <file>.
};

/// "boot", "api", "console", "file".
const char* source_name(Source source);

/// The metadata of an image (its `image` record, copied out of the slot) and what it holds.
struct ImageInfo {
	char schema_version[kMaxVersionText + 1] = {};
	char project_id[kMaxProjectId + 1] = {};
	uint32_t revision = 0;
	char generated_at[kMaxGeneratedAt + 1] = {};
	char generator[kMaxGenerator + 1] = {};
	char name[kMaxName + 1] = {};
	uint16_t format_major = 0; ///< The image's format version (header).
	uint16_t format_minor = 0;
	size_t bytes = 0;      ///< The image's length (header), the signature included.
	uint32_t crc = 0;      ///< The header's CRC-32.
	bool is_signed = false;
	size_t datapoints = 0; ///< In the image (the implied ones included).
	size_t knx_datapoints = 0;
	size_t zigbee_datapoints = 0; ///< With a `zigbee` binding.
	size_t rules = 0;
	size_t schedules = 0;
	size_t blocks = 0;
	size_t locations = 0;
	size_t functions = 0;
	size_t history_rings = 0;
	bool time_master = false;
};

/// The outcome of a validation or load: every refusal with its path into the binary image
/// (spec/runtime-image.md 4, 10) and reason, up to
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

	/// What kind of failure (spec/runtime-image.md 4): none; refused from the header before the slot
	/// was touched (format, version, too_large, unsigned_image); refused after it was written (damaged,
	/// bad_signature, invalid, no_memory, apply: the device has no image afterwards); busy (another
	/// image is being received: nothing was looked at).
	enum class Kind : uint8_t {
		none, format, version, too_large, unsigned_image, damaged, bad_signature, invalid, no_memory, apply, busy
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

/// "none", "format", "version", "too large", "unsigned", "damaged", "bad signature", "invalid",
/// "no memory", "apply", "busy".
const char* report_kind_name(Report::Kind kind);
/// The API's code of a kind: "image.format", "image.version", "image.too_large", "image.unsigned",
/// "image.damaged", "image.bad_signature", "image.invalid", "image.no_memory", "image.apply_failed",
/// "image.busy"; "" for none.
const char* report_kind_code(Report::Kind kind);
/// The HTTP status of a kind (spec/runtime-image.md 4): 400, 422, 413, 403, 422, 403, 422, 507, 500, 503.
int report_kind_status(Report::Kind kind);
/// Whether a refusal of this kind leaves the running image untouched (refused from the header).
bool report_kind_untouched(Report::Kind kind);

} // namespace image
} // namespace hc

#endif // HC_IMAGE_IMAGE_H
