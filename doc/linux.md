# HomeControl on Linux (the Debian package)

The controller runs as a Linux service on a Debian, Raspberry Pi OS or Ubuntu machine: the POSIX
build of the same core as the ESP32-C6 firmware, with the same web UI, API, KNXnet/IP tunnel, rules,
schedules, MQTT, external values, history and E-Home connection. Zigbee needs the controller's
radio and is not available (homecontrol #95).

## Installation from the apt repository

The release workflow builds the package `homecontrol` for each release tag `vX.Y.Z` and publishes it
in the signed apt repository https://haumacher.github.io/homecontrol/apt/ (its page shows the
commands and the published versions):

| System | Distribution |
| --- | --- |
| Debian 12, Raspberry Pi OS Bookworm, Ubuntu 22.04 | `bookworm` |
| Debian 13, Raspberry Pi OS Trixie, Ubuntu 24.04 and newer | `trixie` |

Architectures amd64, arm64 and armhf. The component is the channel, as for the firmware's update
channels: `stable` holds the releases, `beta` the pre-releases as well.

```sh
sudo curl -fsSLo /usr/share/keyrings/homecontrol-archive-keyring.gpg \
  https://haumacher.github.io/homecontrol/apt/homecontrol-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/homecontrol-archive-keyring.gpg] https://haumacher.github.io/homecontrol/apt trixie stable" \
  | sudo tee /etc/apt/sources.list.d/homecontrol.list
sudo apt update
sudo apt install homecontrol
```

The installation creates the system user `homecontrol`, enables and starts the service: open
`http://<host>:8081/` and set the controller's password (the onboarding, as on a new device). The
package version is the firmware version (`hc::kVersion`, a pre-release `1.5.0-beta.1` as
`1.5.0~beta.1`): `dpkg-query -W homecontrol` and `version` in `/api/status` agree.

## Updates

The package manager updates the controller: `sudo apt update && sudo apt upgrade`. The upgrade
restarts the service; the settings, the runtime image, the stored values and the history stay. There
is no update channel in the Settings page (the apt component takes its place), the update status says
`available: false`, and the language packs come with the package instead of from the update server.

## Port and address

The web UI and the API listen on port 8081, the WebSocket of the live values on the port above
(8082), on every IPv4 address. Not 8080: E-Home's Debian package runs Tomcat there, and both fit on
one server. Not 80: a port below 1024 needs a privilege the service does not have.

`/etc/default/homecontrol` holds both settings, commented out with their defaults:

```sh
HOMECONTROL_HTTP_PORT=8081      # 0 switches the web server off
HOMECONTROL_HTTP_BIND=127.0.0.1 # a numeric IPv4 or IPv6 address, e.g. behind a reverse proxy
```

After a change: `sudo systemctl restart homecontrol`. A value that is not a port or not a numeric
address stops the start with the reason in the journal. Run by hand, `--http <port>` and
`--bind <address>` win over the environment (`homecontrol --help`).

## Data

Everything the controller writes lives in `/var/lib/homecontrol` (mode 0750, the user
`homecontrol`): the settings file `homecontrol.settings` (doc/network.md; the password hash and the
tokens included), the runtime image `homecontrol.image.bin`, the blob store `homecontrol.state/`
(persisted values, history, doc/persistence.md) and the run marker `homecontrol.run`
(doc/diagnostics.md). The unit passes `--settings /var/lib/homecontrol/homecontrol.settings`; the
other files are named after it. The web UI and its language packs are read from
`/usr/share/homecontrol/www`.

`sudo apt remove homecontrol` stops the service and keeps `/var/lib/homecontrol` (a new installation
continues with it); `sudo apt purge homecontrol` deletes it. The user stays, as Debian recommends.

## The service

The unit `homecontrol.service` runs `/usr/bin/homecontrol` as `homecontrol` without any capability,
with a read-only system (`ProtectSystem=strict`, writable only `/var/lib/homecontrol`),
`NoNewPrivileges`, private `/tmp` and devices, and `Restart=always`: a restart or a factory reset
from the Settings page (or the API) ends the process, and systemd starts it again within 2 s (the
reset erased the settings, the image and the stored state before: the onboarding follows).

```sh
systemctl status homecontrol
journalctl -u homecontrol -b      # since the boot
journalctl -u homecontrol -f      # follow
```

## E-Home

The controller is connected to E-Home from its Settings page with the one-time code of the project's
tab "Controller" (doc/ehome.md), as a device: it needs no network search. An E-Home behind HTTPS is
reached over `wss://`; its certificate is checked against the system's trust store
(`ca-certificates`), so an E-Home with a certificate of a private CA needs that CA in
`/usr/local/share/ca-certificates/` and `sudo update-ca-certificates`. KNX goes through a KNXnet/IP
gateway (setting `knx.gateway`, doc/knx.md).

## What Linux does not offer

- **Zigbee**: no coordinator. An image with Zigbee bindings loads; its Zigbee datapoints keep no
  value, a command or read to one is refused (409 `dp.refused`, the reason in the journal), and the
  Zigbee API answers "not available". Everything else of the image runs.
- **mDNS**: the controller is not announced as `_homecontrol._tcp`, and its network name
  (`net.hostname`) is not editable: a package on an existing server does not rename it.
- **Self-update and the firmware upload**: the package manager's job (above).
- **WiFi, the status LED, Improv, the core dump**: the device's hardware. The host's network is
  used; the settings `wifi.ps` and `led.quiet` are still listed but have no effect, and the System
  page shows the figures it has no source for as not available.

## Building the package

From the repository root, in a clean Debian container as the release workflow does:

```sh
docker run --rm -v "$PWD":/src -w /src debian:trixie debian/ci-build.sh      # or debian:bookworm
```

`debian/ci-build.sh` installs the build dependencies of `debian/control` and runs
`debian/build-deb.sh`, which writes `debian/changelog` with the version of `core/include/hc/version.h`
(`debian/hc-version`) and builds the binary package with the distribution's hardened flags
(`-O2`, fortify: they find warnings the Debug host build does not, so a change of the core is
checked here too) and the host tests (`--nocheck` leaves them out, as the emulated armhf build does).
The package lands in `dist/homecontrol_<version>_<codename>_<arch>.deb`.

## The apt repository

The release workflow (`.github/workflows/release.yml`, job `deb`) builds the six packages and
attaches them to the GitHub release. The Pages workflow (`pages.yml`) builds the site from the newest
five releases and, with their packages, the repository under `/apt` (`debian/apt-repo.sh`, the page
from `debian/apt-index.html`): one distribution per codename, the components `stable` (releases) and
`beta` (all), signed with the key of the repository secrets `APT_SIGNING_KEY` (the ASCII-armored
secret key, or its base64) and `APT_SIGNING_PASSPHRASE`. Without the key the site goes out without
`/apt` and the workflow warns; the OTA channels never wait for it. A dry run of the release workflow
(started by hand) builds the packages and an apt repository preview signed with a key of the run.
