#!/usr/bin/env bash
# Copyright (c) 2026 Bernhard Haumacher
#
# The local check of a firmware release (doc/ota.md "Releases and the web flasher"): builds and signs
# what the release workflow (.github/workflows/release.yml) builds and signs, on this machine, with
# the local copy of the release key. It publishes nothing; a release is published by pushing its tag:
#
#   git tag -a vX.Y.Z -m 'HomeControl X.Y.Z' && git push origin vX.Y.Z
#
#   scripts/release.sh             # the same as --stage
#   scripts/release.sh --stage     # build, sign and stage into build-release/<version>/
#   scripts/release.sh --dry-run   # the same, then print the commands that publish the release
#   scripts/release.sh -h | --help # this text
#
# The key is ~/.config/homecontrol/release-key.secret (HC_RELEASE_SECRET for another file); the
# signature must verify against the committed platform/esp/release-key.pub.
#
# Checks: a clean tree, not behind origin, ESP-IDF v5.5.6. Warns when HEAD is not on the tag vX.Y.Z of
# hc::kVersion (the workflow refuses a tag that is not kVersion) and when the release exists already.
#
# Steps: the host tests (build-release-host/), the OTA updater and the application for esp32c6 in
# build directories of their own (platform/esp/build-esp32c6-release and
# platform/esp/updater/build-esp32c6-release, wiped first, so the sdkconfig comes from the defaults
# alone: 4 MB table, the committed release key, no HC_EXTRA_CA), then scripts/release-assets.py
# (assemble, sign) into build-release/<version>/:
#
#   assets/   the release assets: the flash parts (bootloader.bin, partition-table.bin,
#             hc_updater.bin, homecontrol.bin, ota_data_ota0.bin, storage.bin), the ESP Web Tools
#             manifest install.json, the signed update manifest manifest.json + manifest.sig, the
#             language packs (i18n-manifest.json, lang-<code>.json.gz), SHA256SUMS, and the ELF
#             files for core dumps (homecontrol.elf, hc_updater.elf: not on the site)
#   site/     the GitHub Pages site built from this release alone (scripts/pages-site.py):
#             cd build-release/<version>/site && python3 -m http.server 8000

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESP="${REPO}/platform/esp"
BUILD_NAME="build-esp32c6-release"
APP_BUILD="${ESP}/${BUILD_NAME}"
UPDATER_BUILD="${ESP}/updater/${BUILD_NAME}"
HOST_BUILD="${REPO}/build-release-host"
RELEASE_SECRET="${HC_RELEASE_SECRET:-${HOME}/.config/homecontrol/release-key.secret}"
IDF_EXPORT="${HC_IDF_EXPORT:-${HOME}/tools/esp/esp-idf-v5.5/export.sh}"
IDF_VERSION="v5.5.6"
GH_REPO="haumacher/homecontrol"

MODE="stage"
case "${1:-}" in
	"" | --stage) MODE="stage" ;;
	--dry-run) MODE="dry-run" ;;
	-h | --help) sed -n '3,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
esac

fail() {
	echo "ERROR: $*" >&2
	exit 1
}

cd "$REPO"

# ---- 1. Version, tag and tree ---------------------------------------------------------------

VERSION="$(sed -n 's/.*constexpr const char\* kVersion = "\([^"]*\)".*/\1/p' core/include/hc/version.h)"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] \
	|| fail "hc::kVersion \"${VERSION}\" is not a release version X.Y.Z[-pre] (no build metadata)"
TAG="v${VERSION}"
echo "release ${VERSION} (tag ${TAG})"

if [[ -n "$(git status --porcelain)" ]]; then
	git status --short >&2
	fail "the tree has uncommitted or untracked changes; a release is built from a commit"
fi

HEAD_TAGS="$(git tag --points-at HEAD --list 'v*')"
if [[ "$HEAD_TAGS" != "$TAG" ]]; then
	echo "WARNING: HEAD carries $([[ -z "$HEAD_TAGS" ]] && echo 'no tag' || echo "the tag(s) $(echo $HEAD_TAGS)"), a release needs exactly ${TAG}" >&2
fi

git fetch --quiet --tags origin
UPSTREAM="$(git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null)" \
	|| fail "the branch has no upstream; release from master tracking origin/master"
BEHIND="$(git rev-list --count "HEAD..${UPSTREAM}")"
[[ "$BEHIND" -eq 0 ]] || fail "HEAD is ${BEHIND} commit(s) behind ${UPSTREAM}: pull first"

if gh release view "$TAG" -R "$GH_REPO" >/dev/null 2>&1; then
	echo "WARNING: the release ${TAG} exists already; a new release needs a new hc::kVersion" >&2
fi

# ---- 2. Tools --------------------------------------------------------------------------------

[[ -r "$RELEASE_SECRET" ]] || fail "no release key at ${RELEASE_SECRET} (HC_RELEASE_SECRET)"
if [[ -z "${IDF_PATH:-}" ]]; then
	[[ -f "$IDF_EXPORT" ]] || fail "IDF_PATH is not set and ${IDF_EXPORT} does not exist (HC_IDF_EXPORT)"
	# shellcheck disable=SC1090
	source "$IDF_EXPORT" >/dev/null
fi
idf.py --version | grep -q "${IDF_VERSION}\b" || fail "a release needs ESP-IDF ${IDF_VERSION}, found $(idf.py --version)"

# ---- 3. Host tests ---------------------------------------------------------------------------

rm -rf "$HOST_BUILD"
cmake -S platform/posix -B "$HOST_BUILD" -G Ninja >/dev/null
cmake --build "$HOST_BUILD"
ctest --test-dir "$HOST_BUILD" --output-on-failure

# ---- 4. Firmware (the same command lines as the workflow) ------------------------------------

# Wiped, so that every value comes from the sdkconfig defaults and no cached development option
# (a test menuconfig choice, HC_EXTRA_CA, another key or flash size) reaches the release.
rm -rf "$APP_BUILD" "$UPDATER_BUILD"
IDF_ARGS=(-B "$BUILD_NAME" -D IDF_TARGET=esp32c6 -D HC_FLASH_SIZE=4MB -D "HC_EXTRA_CA=")
(cd "${ESP}/updater" && idf.py "${IDF_ARGS[@]}" build)
(cd "$ESP" && idf.py "${IDF_ARGS[@]}" build)

# ---- 5. Stage --------------------------------------------------------------------------------

STAGE="${REPO}/build-release/${VERSION}"
rm -rf "$STAGE"
python3 scripts/release-assets.py assemble --app "$APP_BUILD" --updater "$UPDATER_BUILD" --version "$VERSION" \
	--out "${STAGE}/assets"
python3 scripts/release-assets.py sign --key "$RELEASE_SECRET" --out "${STAGE}/assets" --site "${STAGE}/site"

echo
echo "staged ${STAGE}:"
ls -l "${STAGE}/assets"
echo "site preview: cd ${STAGE}/site && python3 -m http.server 8000"

if [[ "$MODE" == "dry-run" ]]; then
	echo
	echo "publishing the release (the workflow release.yml builds, signs and publishes it, then the site):"
	[[ "$HEAD_TAGS" == "$TAG" ]] || printf '+ %s\n' "git tag -a ${TAG} -m 'HomeControl ${VERSION}'"
	printf '+ %s\n' "git push origin ${TAG}"
fi
