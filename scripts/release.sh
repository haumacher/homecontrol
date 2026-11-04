#!/usr/bin/env bash
# Copyright (c) 2026 Bernhard Haumacher
#
# Builds, signs and publishes a firmware release (doc/ota.md "Releases and the web flasher").
#
#   scripts/release.sh             # build, sign, stage, push the tag, publish the GitHub release
#   scripts/release.sh --stage     # build, sign and stage into build-release/<version>/ only
#   scripts/release.sh --dry-run   # the same as --stage, then print the publishing commands
#   scripts/release.sh -h | --help # this text
#
# The release key never leaves this machine: the update manifest is signed here with
# ~/.config/homecontrol/release-key.secret (HC_RELEASE_SECRET to use another file), checked against
# the committed platform/esp/release-key.pub, and only the signature is published. CI holds no key.
#
# Preconditions of a release: a clean tree, HEAD exactly on the tag vX.Y.Z where X.Y.Z is
# hc::kVersion (core/include/hc/version.h), not behind origin, and no published release of the tag
# yet. --stage and --dry-run only warn about a missing tag (their output never leaves this
# machine), everything else they check as a release does.
#
# Steps: the host tests (build-release-host/), the OTA updater and the application for esp32c6 in
# build directories of their own (platform/esp/build-esp32c6-release and
# platform/esp/updater/build-esp32c6-release, wiped first, so the sdkconfig comes from the defaults
# alone: 4 MB table, the committed release key, no HC_EXTRA_CA), then the staged release in
# build-release/<version>/:
#
#   assets/   the GitHub release assets, flat: the flash parts (bootloader.bin, partition-table.bin,
#             hc_updater.bin, homecontrol.bin, ota_data_ota0.bin, storage.bin), the ESP Web Tools
#             manifest install.json, the signed update manifest manifest.json + manifest.sig, the
#             language packs (i18n-manifest.json, lang-<code>.json.gz), SHA256SUMS, and the ELF
#             files for core dumps (homecontrol.elf, hc_updater.elf: not on the site)
#   site/     a preview of the GitHub Pages site built from this release alone
#             (scripts/pages-site.py, as .github/workflows/pages.yml builds it):
#             cd build-release/<version>/site && python3 -m http.server 8000
#
# Publishing pushes the tag, creates the release as a draft with every asset and then publishes
# it; the published release starts the Pages workflow, which puts the release on
# https://haumacher.github.io/homecontrol/. A run that failed after the tag was pushed can be
# repeated: it reuses a draft release of the tag.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESP="${REPO}/platform/esp"
BUILD_NAME="build-esp32c6-release"
APP_BUILD="${ESP}/${BUILD_NAME}"
UPDATER_BUILD="${ESP}/updater/${BUILD_NAME}"
HOST_BUILD="${REPO}/build-release-host"
RELEASE_KEY_PUB="${ESP}/release-key.pub"
RELEASE_SECRET="${HC_RELEASE_SECRET:-${HOME}/.config/homecontrol/release-key.secret}"
IDF_EXPORT="${HC_IDF_EXPORT:-${HOME}/tools/esp/esp-idf-v5.5/export.sh}"
IDF_VERSION="v5.5.6"
GH_REPO="haumacher/homecontrol"
SITE_URL="https://haumacher.github.io/homecontrol"
# Where the site serves a release's files: the signed manifest names the image by this path
# (a leading / keeps the scheme and host of update.url, doc/ota.md "The manifest").
SITE_PATH="/homecontrol"

MODE="full"
case "${1:-}" in
	--stage) MODE="stage" ;;
	--dry-run) MODE="dry-run" ;;
	-h | --help) sed -n '3,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
	"") MODE="full" ;;
	*) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
esac

fail() {
	echo "ERROR: $*" >&2
	exit 1
}

# A precondition of publishing: an error for a release, a warning for --stage and --dry-run.
require() {
	if [[ "$MODE" == "full" ]]; then
		fail "$*"
	fi
	echo "WARNING: $* (staging anyway: --${MODE})" >&2
}

run() {
	if [[ "$MODE" == "dry-run" ]]; then
		printf '+ %s\n' "$*"
	else
		"$@"
	fi
}

cd "$REPO"

# ---- 1. Version, tag and tree ---------------------------------------------------------------

VERSION="$(sed -n 's/.*constexpr const char\* kVersion = "\([^"]*\)".*/\1/p' core/include/hc/version.h)"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] \
	|| fail "hc::kVersion \"${VERSION}\" is not a release version X.Y.Z[-pre] (no build metadata)"
TAG="v${VERSION}"
PRERELEASE=false
[[ "$VERSION" == *-* ]] && PRERELEASE=true
echo "release ${VERSION} (tag ${TAG}$([[ $PRERELEASE == true ]] && echo ', pre-release'))"

if [[ -n "$(git status --porcelain)" ]]; then
	git status --short >&2
	fail "the tree has uncommitted or untracked changes; a release is built from a commit"
fi

HEAD_TAGS="$(git tag --points-at HEAD --list 'v*')"
if [[ "$HEAD_TAGS" != "$TAG" ]]; then
	if [[ -z "$HEAD_TAGS" ]]; then
		require "HEAD carries no tag; tag it: git tag -a ${TAG} -m 'HomeControl ${VERSION}'"
	else
		require "HEAD carries the tag(s) $(echo $HEAD_TAGS), expected exactly ${TAG} (hc::kVersion)"
	fi
fi

git fetch --quiet --tags origin
UPSTREAM="$(git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null)" \
	|| fail "the branch has no upstream; release from master tracking origin/master"
BEHIND="$(git rev-list --count "HEAD..${UPSTREAM}")"
[[ "$BEHIND" -eq 0 ]] || fail "HEAD is ${BEHIND} commit(s) behind ${UPSTREAM}: pull, then move the tag"

if REMOTE_TAG="$(git ls-remote --exit-code --tags origin "refs/tags/${TAG}^{}" "refs/tags/${TAG}" 2>/dev/null)"; then
	if ! grep -q "^$(git rev-parse HEAD)" <<<"$REMOTE_TAG"; then
		fail "${TAG} is on origin at another commit"
	fi
fi
RELEASE_STATE="$(gh release view "$TAG" -R "$GH_REPO" --json isDraft --jq 'if .isDraft then "draft" else "published" end' 2>/dev/null || echo none)"
if [[ "$RELEASE_STATE" == "published" ]]; then
	require "the release ${TAG} is already published"
fi

# ---- 2. Tools --------------------------------------------------------------------------------

[[ -r "$RELEASE_SECRET" ]] || fail "no release key at ${RELEASE_SECRET} (HC_RELEASE_SECRET)"
python3 "${REPO}/scripts/sign.py" public "$RELEASE_SECRET" >/dev/null # needs python3-cryptography or PyNaCl
command -v gh >/dev/null || fail "the GitHub CLI gh is missing"
if [[ -z "${IDF_PATH:-}" ]]; then
	[[ -f "$IDF_EXPORT" ]] || fail "IDF_PATH is not set and ${IDF_EXPORT} does not exist (HC_IDF_EXPORT)"
	# shellcheck disable=SC1090
	source "$IDF_EXPORT" >/dev/null
fi
idf.py --version | grep -q "${IDF_VERSION}\b" || fail "the release needs ESP-IDF ${IDF_VERSION}, found $(idf.py --version)"

# ---- 3. Host tests ---------------------------------------------------------------------------

rm -rf "$HOST_BUILD"
cmake -S platform/posix -B "$HOST_BUILD" -G Ninja >/dev/null
cmake --build "$HOST_BUILD"
ctest --test-dir "$HOST_BUILD" --output-on-failure

# ---- 4. Firmware -----------------------------------------------------------------------------

# Wiped, so that every value comes from the sdkconfig defaults and no cached development option
# (a test menuconfig choice, HC_EXTRA_CA, another key or flash size) reaches the release.
rm -rf "$APP_BUILD" "$UPDATER_BUILD"
IDF_ARGS=(-D HC_FLASH_SIZE=4MB -D "HC_EXTRA_CA=")
idf.py -C "${ESP}/updater" -B "$UPDATER_BUILD" "${IDF_ARGS[@]}" set-target esp32c6
idf.py -C "${ESP}/updater" -B "$UPDATER_BUILD" "${IDF_ARGS[@]}" build
idf.py -C "$ESP" -B "$APP_BUILD" "${IDF_ARGS[@]}" -D "HC_RELEASE_KEY=${RELEASE_KEY_PUB}" set-target esp32c6
idf.py -C "$ESP" -B "$APP_BUILD" "${IDF_ARGS[@]}" -D "HC_RELEASE_KEY=${RELEASE_KEY_PUB}" build

grep -q '^CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE=y' "${APP_BUILD}/sdkconfig" \
	&& fail "the release build trusts extra CA certificates (HC_EXTRA_CA)"
grep -q '^CONFIG_HC_UPDATER_TEST_BUTTON_WINDOW=y' "${UPDATER_BUILD}/sdkconfig" \
	&& fail "the release updater is a test build (HC_UPDATER_TEST_BUTTON_WINDOW)"

# ---- 5. Stage --------------------------------------------------------------------------------

STAGE="${REPO}/build-release/${VERSION}"
ASSETS="${STAGE}/assets"
rm -rf "$STAGE"
mkdir -p "$ASSETS"

cp "${APP_BUILD}/bootloader/bootloader.bin" "${APP_BUILD}/partition_table/partition-table.bin" \
	"${APP_BUILD}/homecontrol.bin" "${APP_BUILD}/ota_data_ota0.bin" "${APP_BUILD}/storage.bin" \
	"${APP_BUILD}/homecontrol.elf" "${UPDATER_BUILD}/hc_updater.bin" "${UPDATER_BUILD}/hc_updater.elf" "$ASSETS/"

# The ESP Web Tools manifest from the template web/flasher/manifest.json (the local flasher's): the
# same parts and offsets, checked against the build's flash layout, as files beside it.
python3 - "${APP_BUILD}/flasher_args.json" "${REPO}/web/flasher/manifest.json" "$VERSION" "${ASSETS}/install.json" \
	"${ASSETS}/homecontrol.bin" <<'PY'
import json, os, struct, sys
flasher_args, template, version, out, app = sys.argv[1:]
flash = json.load(open(flasher_args))['flash_files']
by_name = {os.path.basename(path): int(offset, 16) for offset, path in flash.items()}
manifest = json.load(open(template))
parts = manifest['builds'][0]['parts']
names = [os.path.basename(p['path']) for p in parts]
for part, name in zip(parts, names):
	if by_name.get(name) != part['offset']:
		sys.exit('install.json: %s at %s, the build flashes it at %s' % (
			name, hex(part['offset']), hex(by_name[name]) if name in by_name else 'no offset'))
	part['path'] = name
missing = set(by_name) - set(names)
if missing:
	sys.exit('install.json lacks %s (web/flasher/manifest.json)' % ', '.join(sorted(missing)))
# The application must carry the release's version (hc::kVersion): the updater checks it against
# the manifest, and Improv reports it.
data = open(app, 'rb').read(512)
desc = 24 + 8
if data[0] != 0xE9 or struct.unpack_from('<I', data, desc)[0] != 0xABCD5432:
	sys.exit('homecontrol.bin has no app descriptor')
app_version = data[desc + 16:desc + 48].split(b'\0')[0].decode('ascii')
project = data[desc + 48:desc + 80].split(b'\0')[0].decode('ascii')
if project != 'homecontrol' or app_version != version:
	sys.exit('homecontrol.bin is %s %s, the release is homecontrol %s' % (project, app_version, version))
del manifest['_note']
manifest['version'] = version
with open(out, 'w') as f:
	json.dump(manifest, f, indent=2)
	f.write('\n')
PY

# The update manifest (doc/ota.md "The manifest"), signed over its exact bytes.
python3 - "${ASSETS}/homecontrol.bin" "$VERSION" "${SITE_PATH}/firmware/${VERSION}/homecontrol.bin" \
	"${ASSETS}/manifest.json" <<'PY'
import hashlib, json, sys
app, version, url, out = sys.argv[1:]
data = open(app, 'rb').read()
manifest = {'version': version, 'url': url, 'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)}
text = (json.dumps(manifest, indent=2) + '\n').encode('utf-8')
if len(text) > 1024:
	sys.exit('manifest.json is %d bytes, the device reads at most 1024' % len(text))
open(out, 'wb').write(text)
PY
python3 "${REPO}/scripts/sign.py" manifest "$RELEASE_SECRET" "${ASSETS}/manifest.json"

# The signature must verify against the key the firmware carries (a wrong secret file would
# publish a manifest every device refuses).
python3 - "$RELEASE_KEY_PUB" "${ASSETS}/manifest.json" "${ASSETS}/manifest.sig" <<'PY'
import sys
public = bytes.fromhex(open(sys.argv[1]).read().strip())
message = open(sys.argv[2], 'rb').read()
signature = bytes.fromhex(open(sys.argv[3]).read().strip())
try:
	from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
	from cryptography.exceptions import InvalidSignature
	try:
		Ed25519PublicKey.from_public_bytes(public).verify(signature, message)
	except InvalidSignature:
		sys.exit('manifest.sig does not verify against %s: the secret is not the release key' % sys.argv[1])
except ImportError:
	import nacl.signing, nacl.exceptions
	try:
		nacl.signing.VerifyKey(public).verify(message, signature)
	except nacl.exceptions.BadSignatureError:
		sys.exit('manifest.sig does not verify against %s: the secret is not the release key' % sys.argv[1])
print('manifest.sig verifies against %s' % sys.argv[1])
PY

# The language packs of the release (doc/i18n.md "Releasing"), flat for the release assets.
python3 "${REPO}/scripts/i18n-pack.py" manifest "${STAGE}/i18n-stage" --version "$VERSION"
mv "${STAGE}/i18n-stage/i18n/manifest.json" "${ASSETS}/i18n-manifest.json"
mv "${STAGE}/i18n-stage/i18n/"lang-*.json.gz "${ASSETS}/"
rm -r "${STAGE}/i18n-stage"

(cd "$ASSETS" && sha256sum -- * >SHA256SUMS)

# The site as the Pages workflow builds it, from this release alone.
python3 "${REPO}/scripts/pages-site.py" --out "${STAGE}/site" --release "$ASSETS"

echo
echo "staged ${STAGE}:"
ls -l "$ASSETS"
echo "site preview: cd ${STAGE}/site && python3 -m http.server 8000"

if [[ "$MODE" == "stage" ]]; then
	exit 0
fi

# ---- 6. Publish ------------------------------------------------------------------------------

NOTES="HomeControl ${VERSION} for the ESP32-C6 (4 MB flash).

- Install it on a new controller from the browser: ${SITE_URL}/
- Update an installed controller over the air: update source ${SITE_URL}/stable (Settings, Firmware update)
- Or upload \`homecontrol.bin\` on the controller's Settings page (Install firmware file).

The update manifest is signed with the release key $(python3 "${REPO}/scripts/sign.py" public "$RELEASE_SECRET" | cut -c67-) (\`platform/esp/release-key.pub\`)."

UPLOAD=("${ASSETS}"/*)
run git push origin "refs/tags/${TAG}"
if [[ "$RELEASE_STATE" == "none" ]]; then
	PRE=()
	[[ "$PRERELEASE" == true ]] && PRE=(--prerelease)
	run gh release create "$TAG" -R "$GH_REPO" --verify-tag --draft "${PRE[@]}" \
		--title "HomeControl ${VERSION}" --notes "$NOTES" --generate-notes "${UPLOAD[@]}"
else
	run gh release upload "$TAG" -R "$GH_REPO" --clobber "${UPLOAD[@]}"
fi
# Published last: the event starts the Pages workflow, which needs every asset.
run gh release edit "$TAG" -R "$GH_REPO" --draft=false

echo
echo "released ${VERSION}: https://github.com/${GH_REPO}/releases/tag/${TAG}"
echo "the Pages workflow publishes it at ${SITE_URL}/ (gh run list -R ${GH_REPO} --workflow pages.yml)"
