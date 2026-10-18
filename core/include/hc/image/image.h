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

/// The schema version this firmware reads: major 1, minor 0. An image with a higher major version
/// is refused (named in the report); a higher minor version loads (its additions are ignorable).
constexpr uint32_t kSchemaMajor = 1;
constexpr uint32_t kSchemaMinor = 0;

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
	/// content is invalid, the device cannot hold it (memory), or applying it failed.
	enum class Kind : uint8_t { none, syntax, version, invalid, too_large, no_memory, apply };

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

/// "none", "syntax", "version", "invalid", "too large", "no memory", "apply".
const char* report_kind_name(Report::Kind kind);

} // namespace image
} // namespace hc

#endif // HC_IMAGE_IMAGE_H
