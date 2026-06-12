# Changelog

All notable changes to the T1N Smart Lock are recorded here. The
project ships three independently revised artifacts: the firmware, the
PCB, and the enclosure. Each can move on its own version line, so the
compatibility table below records which revisions are validated to work
together. Build a tagged commit to reproduce the exact combination in a
given row.

This project is distributed as source. There are no prebuilt binaries
or GitHub Releases. Firmware is built and flashed from source as
described in [docs/BUILDING.md](docs/BUILDING.md), and the managed component pins recorded
here are part of what defines a release.

## Compatibility

| Firmware | PCB | Enclosure | ESP-IDF | esp_matter | Validated on |
|---|---|---|---|---|---|
| 1.0.0 | v1.0 | v1.0 | 5.4.4 | 1.4.2~2 | 2005 Dodge Sprinter 2500 |

## 1.0.0 - 2026-06-12

First public release. Firmware 1.0.0, PCB v1.0, enclosure v1.0.

Open source observation-based Matter over Thread control of the factory
central locking, validated against a single vehicle. The firmware reads the
master door lock switch LEDs through optocouplers, senses the CTM rail
to detect sleep, and issues a CTM-aware lock command of one or two
pulses. It reports observed reality rather than commanded state. Full
behavior is documented in [docs/FIRMWARE.md](docs/FIRMWARE.md); this entry records the
release baseline rather than restating the feature set.

Known limitations/issues at release, with detail in [docs/FIRMWARE.md](docs/FIRMWARE.md):

- The classifier can commit a false CLOSED after a reboot with one or
  more doors held open for several minutes. It self-heals on the next
  physical door event. Observed 2026-05-31.
- OTA is present in the build but untested and unsupported in v1.0.
  Updates are flashed over USB-C from source.
- No per-side unlock. The CTM treats the master lock as a group, so all
  doors cycle together.
- An Unlock command with a door open still fires a pulse, since only the
  Lock path has the door-open check.
- Validated on one 2005 Dodge Sprinter 2500 only. Other years, markets,
  and trims are unverified.
