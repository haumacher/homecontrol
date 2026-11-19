# HomeControl

An ESP32-C6 based controller for your smart home: the runtime that executes schedules, rules and
visualization for a KNX installation and a Zigbee network, engineered with
[E-Home](https://github.com/haumacher/ehome).

- **Install it**: the web flasher at https://haumacher.github.io/homecontrol/ (a browser with Web Serial,
  an ESP32-C6 board by USB); releases at https://github.com/haumacher/homecontrol/releases.
- **On a Linux server** (Debian, Raspberry Pi OS, Ubuntu; amd64, arm64, armhf): the package
  `homecontrol` from the apt repository https://haumacher.github.io/homecontrol/apt/, everything but
  Zigbee — `doc/linux.md`.
- `ROADMAP.md` — where the project is going and the decisions taken.
- `CLAUDE.md` — build facts and conventions.
- `doc/hardware.md` — boards and wiring.
- Issues: https://github.com/haumacher/homecontrol/issues
