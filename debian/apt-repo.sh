#!/bin/sh
# Copyright (c) 2026 Bernhard Haumacher
#
# Builds the signed apt repository of the controller (doc/linux.md "The apt repository"), served by
# GitHub Pages under https://haumacher.github.io/homecontrol/apt/ beside the OTA channels:
#
#   debian/apt-repo.sh <site dir> <package dir>
#
# <package dir> holds the packages of the releases on the site (.github/workflows/pages.yml downloads
# those of the newest releases from GitHub, the releases being the source of truth), named as
# debian/build-deb.sh names them: homecontrol_<version>_<codename>_<arch>.deb. The distributions are
# the codenames (bookworm, trixie), the components the update channels: "beta" holds every package,
# "stable" those of releases (a version without "~", i.e. hc::kVersion without a pre-release part),
# so beta never lags behind stable, as with the firmware's channels. The pool keeps each file once.
#
# The indexes are written with apt-ftparchive and signed with the GnuPG key $APT_SIGNING_KEY_ID
# (default: the only secret key of the keyring), its passphrase in $APT_SIGNING_PASSPHRASE; the public
# key is published as homecontrol-archive-keyring.gpg (and .asc). index.html, the install page, comes
# from debian/apt-index.html.
set -eu

SITE=${1:-}
PACKAGES=${2:-}
if [ -z "$SITE" ] || [ ! -d "$PACKAGES" ]; then
    echo "usage: $0 <site dir> <package dir>" >&2
    exit 2
fi
TEMPLATE=$(cd "$(dirname "$0")" && pwd)/apt-index.html
PACKAGES=$(cd "$PACKAGES" && pwd)
URL=${APT_REPO_URL:-https://haumacher.github.io/homecontrol/apt}
SUITES="bookworm trixie"
ARCHS="amd64 arm64 armhf"
COMPONENTS="stable beta"

rm -rf "$SITE"
mkdir -p "$SITE"
cd "$SITE"
count=0
for deb in "$PACKAGES"/homecontrol_*.deb; do
    [ -e "$deb" ] || continue
    name=$(basename "$deb")
    rest=${name#homecontrol_}
    version=${rest%%_*}
    rest=${rest#*_}
    suite=${rest%%_*}
    arch=${rest#*_}
    arch=${arch%.deb}
    case " $SUITES " in *" $suite "*) ;; *) echo "$name: no distribution $suite, skipped" >&2; continue ;; esac
    case " $ARCHS " in *" $arch "*) ;; *) echo "$name: no architecture $arch, skipped" >&2; continue ;; esac
    if [ "$(dpkg-deb -f "$deb" Version)" != "$version" ] || [ "$(dpkg-deb -f "$deb" Architecture)" != "$arch" ]; then
        echo "$0: $name: its control file says $(dpkg-deb -f "$deb" Version) $(dpkg-deb -f "$deb" Architecture)" >&2
        exit 1
    fi
    mkdir -p "pool/$suite"
    cp "$deb" "pool/$suite/"
    for component in $COMPONENTS; do
        if [ "$component" = stable ]; then
            case "$version" in *~*) continue ;; esac
        fi
        mkdir -p "lists/$suite/$component"
        echo "pool/$suite/$name" >> "lists/$suite/$component/files"
    done
    count=$((count + 1))
done
if [ "$count" -eq 0 ]; then
    echo "$0: no package in $PACKAGES" >&2
    exit 1
fi

KEY=${APT_SIGNING_KEY_ID:-$(gpg --batch --list-secret-keys --with-colons | awk -F: '/^sec/ { print $5; exit }')}
if [ -z "$KEY" ]; then
    echo "$0: no signing key in the keyring" >&2
    exit 1
fi
sign() {
    printf '%s' "${APT_SIGNING_PASSPHRASE:-}" | gpg --batch --yes --pinentry-mode loopback --passphrase-fd 0 \
        --local-user "$KEY" "$@"
}

for suite in $SUITES; do
    for component in $COMPONENTS; do
        for arch in $ARCHS; do
            index="dists/$suite/$component/binary-$arch"
            mkdir -p "$index"
            : > "$index/Packages"
            if [ -s "lists/$suite/$component/files" ]; then
                for deb in $(cat "lists/$suite/$component/files"); do
                    case "$deb" in *_"$arch".deb) apt-ftparchive packages "$deb" >> "$index/Packages" ;; esac
                done
            fi
            gzip -9 -n -k -f "$index/Packages"
        done
    done
    apt-ftparchive \
        -o APT::FTPArchive::Release::Origin=HomeControl \
        -o "APT::FTPArchive::Release::Label=HomeControl" \
        -o APT::FTPArchive::Release::Suite="$suite" \
        -o APT::FTPArchive::Release::Codename="$suite" \
        -o "APT::FTPArchive::Release::Architectures=$ARCHS" \
        -o "APT::FTPArchive::Release::Components=$COMPONENTS" \
        -o "APT::FTPArchive::Release::Description=HomeControl, the smart-home controller as a Linux service" \
        release "dists/$suite" > "dists/$suite/Release.tmp"
    mv "dists/$suite/Release.tmp" "dists/$suite/Release"
    sign --clearsign -o "dists/$suite/InRelease" "dists/$suite/Release"
    sign --armor --detach-sign -o "dists/$suite/Release.gpg" "dists/$suite/Release"
done
rm -rf lists

gpg --batch --yes --export "$KEY" > homecontrol-archive-keyring.gpg
gpg --batch --yes --armor --export "$KEY" > homecontrol-archive-keyring.asc
FINGERPRINT=$(gpg --batch --with-colons --fingerprint "$KEY" | awk -F: '/^fpr/ { print $10; exit }')

# The install page: the newest version per distribution and channel, with its architectures.
table=$(mktemp)
for suite in $SUITES; do
    for component in $COMPONENTS; do
        versions=$(cat dists/$suite/$component/binary-*/Packages | sed -n 's/^Version: //p' | sort -u)
        newest=""
        for v in $versions; do
            if [ -z "$newest" ] || dpkg --compare-versions "$v" gt "$newest"; then
                newest=$v
            fi
        done
        if [ -z "$newest" ]; then
            printf '<tr><td>%s</td><td>%s</td><td colspan="2">none yet</td></tr>\n' "$suite" "$component"
            continue
        fi
        archs=""
        for arch in $ARCHS; do
            if grep -qx "Version: $newest" "dists/$suite/$component/binary-$arch/Packages"; then
                archs="$archs $arch"
            fi
        done
        printf '<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>\n' "$suite" "$component" "$newest" "${archs# }"
    done
done > "$table"
# The example names "stable" once it has a release, "beta" until then.
channel=beta
for index in dists/*/stable/binary-*/Packages; do
    if [ -s "$index" ]; then
        channel=stable
    fi
done
sed -e "s|@SITE_URL@|$URL|g" -e "s|@CHANNEL@|$channel|g" -e "s|@FINGERPRINT@|$FINGERPRINT|g" \
    -e "/@PACKAGES@/{r $table" -e 'd;}' "$TEMPLATE" > index.html
rm -f "$table"
echo "apt repository: $count package(s) in $SITE, signed by $FINGERPRINT"
