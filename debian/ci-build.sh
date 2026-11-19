#!/bin/sh
# Copyright (c) 2026 Bernhard Haumacher
#
# Builds the binary package inside a clean Debian container, as the release workflow does
# (.github/workflows/release.yml, job "deb"), from the repository root:
#
#   docker run --rm -v "$PWD":/src -w /src debian:trixie debian/ci-build.sh [--nocheck]
#
# Runs as root in the container: installs the build dependencies of debian/control and builds with
# debian/build-deb.sh, the host tests included (--nocheck leaves them out: the emulated armhf build).
# The package lands in dist/; the files the build leaves in the checkout are given back to the
# checkout's owner.
set -eu

cd "$(dirname "$0")/.."

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends git ca-certificates dpkg-dev >/dev/null

# The checkout belongs to the host user, not to root in the container. In the system's config:
# debhelper runs the build with a HOME of its own, where the build id's git describe would not see it.
git config --system --add safe.directory '*'

debian/build-deb.sh --changelog-only
apt-get build-dep -y -qq ./ >/dev/null

if [ "${1:-}" = --nocheck ]; then
    export DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-} nocheck"
fi
status=0
debian/build-deb.sh -b || status=$?
owner=$(stat -c %u:%g .)
chown -R "$owner" dist debian web/ui/dist/lang build-deb 2>/dev/null || true
exit $status
