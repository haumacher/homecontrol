/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_UPDATE_CHECK_H
#define HC_UPDATE_CHECK_H

#include <cstddef>
#include <cstdint>

#include "hc/platform/settings.h"

namespace hc {

/// The setting keys of the firmware update (doc/ota.md). Persisted names: never rename.
namespace setting {

/// The update source configured by the user: the base URL under which `manifest.json` and the
/// image live (console `update url set`). Empty or missing: the periodic check is disabled.
constexpr const char* kUpdateUrl = "update.url";

/// The update instruction, written by the application before it boots into the updater and erased
/// by the application after the next confirmed start. `update.image` is written last and erased
/// first: its presence marks a complete instruction.
constexpr const char* kUpdateImage = "update.image";
constexpr const char* kUpdateVersion = "update.version";
constexpr const char* kUpdateSha256 = "update.sha256";
constexpr const char* kUpdateSize = "update.size"; ///< integer, bytes

/// Download attempts the updater has started for the pending instruction (integer, written by the
/// updater before each attempt).
constexpr const char* kUpdateAttempts = "update.attempts";

} // namespace setting

/// A semantic version (semver.org 2.0): MAJOR.MINOR.PATCH, optionally `-<pre-release>` and
/// `+<build>`. The firmware's own version is hc::kVersion ("0.1.0-dev"); a manifest may append
/// the build id ("0.1.0-dev+g1a2b3c4").
struct Version {
	/// Longest version text in characters.
	static constexpr size_t kMaxText = 63;

	uint32_t major = 0;
	uint32_t minor = 0;
	uint32_t patch = 0;
	char prerelease[kMaxText + 1] = {}; ///< Empty for a release.
	char build[kMaxText + 1] = {};      ///< Empty if none; ignored for precedence.

	bool is_prerelease() const { return prerelease[0] != '\0'; }
};

/// Parses `text` strictly (no leading `v`, no leading zeros in numbers, identifiers of
/// [0-9A-Za-z-]). On refusal returns false with `error` pointing to the reason.
bool parse_version(const char* text, Version& out, const char** error = nullptr);

/// Semver precedence: negative if `a` is older than `b`, 0 if equal, positive if newer. A
/// pre-release is older than its release (0.1.0-dev < 0.1.0); build metadata does not count.
int compare_versions(const Version& a, const Version& b);

/// What the update source publishes at `<update.url>/manifest.json`, a flat JSON object:
///
///     {"version": "0.2.0", "url": "homecontrol.bin", "sha256": "<64 hex>", "size": 1034576,
///      "min_version": "0.1.0"}
///
/// `url` is absolute (http:// or https://) or relative to the update URL; `min_version` is
/// optional. Unknown members are ignored (a newer source may add some).
struct UpdateManifest {
	/// The longest manifest accepted, in bytes.
	static constexpr size_t kMaxLength = 1024;

	char version[Version::kMaxText + 1] = {};
	char url[Settings::kMaxStringLength + 1] = {};
	char sha256[65] = {}; ///< 64 lower-case hex digits
	uint32_t size = 0;
	char min_version[Version::kMaxText + 1] = {}; ///< Empty if not given.
};

/// Parses a manifest. Refuses (false, `error` names the reason) anything malformed: invalid JSON,
/// a missing or mistyped member, an invalid version, a checksum that is not 64 hex digits, a size
/// of 0 or above 16 MB, a text longer than its field.
bool parse_update_manifest(const char* text, size_t length, UpdateManifest& out, const char** error = nullptr);

/// An update source or image URL: `http://` or `https://`, a host, at most
/// Settings::kMaxStringLength characters, no spaces or control characters.
bool update_url_valid(const char* url, const char** error = nullptr);

/// `<base>/manifest.json` (a trailing slash of `base` is dropped). False if it does not fit.
bool update_manifest_url(const char* base, char* out, size_t size);

/// Resolves the manifest's `url` against the update source `base`: an absolute URL stays, one
/// starting with `/` keeps the scheme and host of `base`, anything else is appended to `base`. The
/// result must be a valid update URL that fits a setting (Settings::kMaxStringLength).
bool resolve_update_url(const char* base, const char* reference, char* out, size_t size,
	const char** error = nullptr);

/// The verdict of check_update().
enum class UpdateVerdict : uint8_t {
	apply,      ///< Install the manifest's image.
	up_to_date, ///< The manifest offers nothing newer.
	refused,    ///< The manifest cannot be used (reason given).
};

/// "apply", "up to date", "refused".
const char* update_verdict_name(UpdateVerdict verdict);

struct UpdateCheckResult {
	UpdateVerdict verdict = UpdateVerdict::refused;
	/// Why (static text); for `apply` a short description ("newer release", "forced").
	const char* reason = "";
	/// The resolved image URL (only for `apply`).
	char image_url[Settings::kMaxStringLength + 1] = {};
};

/// Decides whether the firmware `running` (its hc::kVersion) installs the update the manifest
/// describes, fetched from the update source `base`:
///
/// - the manifest's version must be newer than the running one (semver precedence; a `-dev` build
///   is older than its release, so it updates to it);
/// - a `min_version` above the running version refuses the update (the device needs an
///   intermediate release first) - except on a pre-release build: `-dev` builds update to any
///   newer release;
/// - `force` (the console's `update now`) installs whatever valid image the manifest names, newer
///   or not.
///
/// A malformed running version or manifest version, or an image URL that cannot be resolved, is
/// refused with the reason.
UpdateCheckResult check_update(const char* running, const UpdateManifest& manifest, const char* base,
	bool force = false);

/// The update instruction in the settings (setting::kUpdate*), the hand-over from the application
/// to the updater.
struct UpdateInstruction {
	char image_url[Settings::kMaxStringLength + 1] = {};
	char version[Version::kMaxText + 1] = {};
	char sha256[65] = {};
	int32_t size = 0;
	int32_t attempts = 0;
};

enum class InstructionStatus : uint8_t {
	none,    ///< No instruction (`update.image` missing).
	pending, ///< A complete instruction.
	corrupt, ///< `update.image` is there but the rest is missing or unreadable (reason given).
};

/// "none", "pending", "corrupt".
const char* instruction_status_name(InstructionStatus status);

/// Reads the instruction. `attempts` is 0 if the updater has not counted one yet.
InstructionStatus load_update_instruction(const Settings& settings, UpdateInstruction& out,
	const char** error = nullptr);

/// Stores the instruction for the manifest's image at `image_url` (attempts reset to 0; the image
/// key last). False with the reason if a write fails.
bool store_update_instruction(Settings& settings, const UpdateManifest& manifest, const char* image_url,
	const char** error = nullptr);

/// Removes the instruction (the image key first). False if a key could not be erased.
bool clear_update_instruction(Settings& settings);

/// Whether the running firmware is the version the instruction asked for (precedence equal):
/// the update completed. Otherwise the updater gave up and booted the previous application.
bool update_instruction_fulfilled(const char* running, const UpdateInstruction& instruction);

/// What the updater finds in the application partition (ota_0).
enum class AppImageState : uint8_t {
	missing, ///< No image that passes the IDF's image verification (erased, half written).
	valid,   ///< A verified image that has not been rolled back.
	aborted, ///< A verified image the bootloader rolled back: it never confirmed itself.
};

/// "missing", "valid", "aborted".
const char* app_image_state_name(AppImageState state);

/// The updater's next step.
enum class UpdaterAction : uint8_t {
	boot_app, ///< Make ota_0 the boot partition and restart.
	download, ///< Download the instruction's image into ota_0 (after `delay_ms`).
	wait,     ///< Nothing to do: no instruction and no application; look again after `delay_ms`.
};

/// "boot application", "download", "wait".
const char* updater_action_name(UpdaterAction action);

struct UpdaterStep {
	UpdaterAction action;
	uint32_t delay_ms;
	const char* reason;
};

/// Download attempts before the updater gives up on an instruction and boots ota_0 if it can.
constexpr int32_t kUpdateMaxAttempts = 3;
/// Delay between attempts.
constexpr uint32_t kUpdateRetryDelayMs = 10000;
/// Delay of the endless retry once the attempts are used up and ota_0 cannot be booted, and of
/// the reminder while there is nothing to do.
constexpr uint32_t kUpdateIdleRetryMs = 5u * 60u * 1000u;

/// The updater's decision table (doc/ota.md, "The updater"):
///
/// - no (or a corrupt) instruction: boot ota_0 if it holds a verified image, rolled back or not
///   (a freshly flashed board, a reset in the first minute of a new image); otherwise wait;
/// - a pending instruction with attempts left: download (the first attempt at once, then after
///   kUpdateRetryDelayMs) - also over a rolled-back image, which is the re-download;
/// - attempts used up: boot ota_0 if it holds a valid image that was not rolled back (the download
///   failed before it erased the old application); otherwise retry every kUpdateIdleRetryMs.
UpdaterStep decide_updater_step(InstructionStatus instruction, int32_t attempts, AppImageState app);

} // namespace hc

#endif // HC_UPDATE_CHECK_H
