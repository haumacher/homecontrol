#!/bin/sh
# Copyright (c) 2026 Bernhard Haumacher
#
# Builds the Debian package homecontrol (doc/linux.md) with the version hc::kVersion in Debian
# syntax (debian/hc-version), for the distribution this runs on (VERSION_CODENAME of
# /etc/os-release). debian/changelog is generated here and not versioned; further arguments go to
# dpkg-buildpackage (default -b: the binary package only). The package lands in dist/ as
# homecontrol_<version>_<codename>_<arch>.deb: the codename in the file name keeps the builds of
# both distributions apart on a GitHub release and in the apt pool.
#
#   debian/build-deb.sh [dpkg-buildpackage args]
#   debian/build-deb.sh --changelog-only
set -eu

cd "$(dirname "$0")/.."

VERSION=$(debian/hc-version)
CODENAME=$(. /etc/os-release && printf '%s' "${VERSION_CODENAME:-unstable}")
MAINTAINER=$(sed -n 's/^Maintainer: //p' debian/control)
# The commit date, not the build date: the same commit builds the same package.
DATE=$(git log -1 --format=%cD 2>/dev/null || date -R)
DESCRIBE=$(git describe --always --dirty 2>/dev/null || echo "$VERSION")

cat > debian/changelog <<END
homecontrol (${VERSION}) ${CODENAME}; urgency=medium

  * HomeControl ${DESCRIBE}, built as a Linux service for ${CODENAME}.

 -- ${MAINTAINER}  ${DATE}
END
echo "homecontrol ${VERSION} (${DESCRIBE}) for ${CODENAME}"
if [ "${1:-}" = --changelog-only ]; then exit 0; fi

if [ $# -eq 0 ]; then set -- -b; fi
dpkg-buildpackage -us -uc "$@"

mkdir -p dist
ARCH=$(dpkg-architecture -qDEB_HOST_ARCH)
for f in ../homecontrol_"${VERSION}"_"${ARCH}".deb ../homecontrol-dbgsym_"${VERSION}"_"${ARCH}".deb; do
    [ -e "$f" ] || continue
    base=$(basename "$f" "_${ARCH}.deb")
    mv -f "$f" "dist/${base}_${CODENAME}_${ARCH}.deb"
done
rm -f ../homecontrol_"${VERSION}"_*.buildinfo ../homecontrol_"${VERSION}"_*.changes
ls -1 dist/homecontrol_"${VERSION}"_"${CODENAME}"_"${ARCH}".deb
