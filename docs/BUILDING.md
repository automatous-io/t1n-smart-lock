# Building from source

**[README](../README.md)** > **Building** · [Report an issue](../../../issues/new)

This guide covers building the T1N Smart Lock firmware from
source and flashing it to the board.

For what the firmware actually does once built, see
[FIRMWARE.md](FIRMWARE.md).

## Contents

- [Requirements](#requirements)
- [Repository structure](#repository-structure)
- [Setup](#setup)
- [Build](#build)
- [Build output](#build-output)
- [Flash your build](#flash-your-build)
- [Reproducing this exact build](#reproducing-this-exact-build)

## Requirements

- **ESP-IDF v5.4.4**. Espressif's IoT development framework, and
  the only SDK you install yourself. The esp_matter `1.5`
  release targets ESP-IDF 5.4.1; this project pins 5.4.4, the
  final 5.4.x patch release, which stays on the version line
  esp_matter expects and includes the 5.4.x bug fixes released
  after 5.4.1.
- **esp_matter, from the component registry**. Espressif's Matter
  SDK is pulled in automatically as a managed component, pinned to
  `1.5` in `main/idf_component.yml`. You do not clone or
  install esp-matter separately; the first build fetches it into
  `managed_components/`.
- **macOS or Linux**. The Windows build host is not tested.

## Repository structure

The firmware is a single ESP-IDF project rooted at `firmware/`:

```
firmware/
├── main/                 # application source (module map in FIRMWARE.md)
│   └── idf_component.yml  # registry component pins (esp_matter, button)
└── sdkconfig.defaults     # Thread/Matter build defaults for the C6
```

All build commands below run from inside `firmware/`.

## Setup

ESP-IDF is the only SDK to install yourself. Follow Espressif's
official guide for v5.4.4 and the ESP32-C6 target:

- [ESP-IDF Get Started (v5.4.4, ESP32-C6)](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32c6/get-started/index.html)

esp_matter and the other managed components are fetched from the
component registry on the first build, so there is nothing else to
install.

## Build

Clone the repository and build:

```bash
git clone https://github.com/automatous-io/t1n-smart-lock.git
cd t1n-smart-lock/firmware

# Activate the ESP-IDF environment. The export.sh path depends on
# where ESP-IDF was installed. Manual install:
. $HOME/esp/esp-idf/export.sh
# VSCode ESP-IDF extension installs under .espressif, e.g.:
# . $HOME/.espressif/v5.4.4/esp-idf/export.sh

idf.py set-target esp32c6
idf.py build
```

Run these in the same shell where you sourced `export.sh`;
ESP-IDF won't be on your PATH otherwise.

`sdkconfig.defaults` is applied automatically by `set-target`, so
no `-DSDKCONFIG_DEFAULTS` flag is needed. The first build resolves
the registry components pinned in `main/idf_component.yml` and
downloads them into `managed_components/`; this needs network
access and runs only once. After the target is set, plain
`idf.py build` rebuilds incrementally.

A successful build ends with `Project build complete.` followed by
the path to `t1n_smart_lock.bin` and the `idf.py flash` command
hint. If the build stops with an error instead, the most common
cause is an ESP-IDF version mismatch (see
[Requirements](#requirements)).

## Build output

`idf.py build` produces the application image and its supporting
binaries in `build/`:

- `build/t1n_smart_lock.bin`, the application image
- `build/bootloader/bootloader.bin`, the second-stage bootloader
- `build/partition_table/partition-table.bin`, the partition table

## Flash your build

With the board connected over USB-C:

**The first time you flash a given board, erase it first.** Erasing
clears the whole flash, including the `nvs` partition, so the board
starts from a known-empty state. A XIAO may arrive with factory demo
firmware, and a board you have used before can carry leftover NVS data,
Matter commissioning records, or Thread credentials from whatever ran
on it previously. Wiping all of it lets the device boot uncommissioned
and pair cleanly. This build
compiles its commissionable data and attestation certificates into the
firmware (test setup parameters and the example DAC provider), so
nothing needs re-provisioning after the erase and the pairing QR is
unchanged.

```bash
idf.py -p <PORT> erase-flash
idf.py -p <PORT> flash monitor
```

Replace `<PORT>` with the board's USB serial port:

- **macOS:** `/dev/cu.usbmodem*` (e.g. `/dev/cu.usbmodem101`)
- **Linux:** `/dev/ttyACM*` or `/dev/ttyUSB*`
- **Windows:** `COM*` (untested build host)

`idf.py flash` writes the bootloader, partition table, and app
image to their correct offsets automatically; there's no manual
merge step. The ESP-IDF VSCode extension's flash button does the
same thing if you prefer a GUI. Exit the serial monitor with
`Ctrl-]`.

**For later firmware updates, flash without erasing.** Plain
`idf.py -p <PORT> flash monitor` rewrites the bootloader, partition
table, and app image but leaves the `nvs` data partition intact, so the
device keeps its Matter fabric and Thread credentials and does not need
re-pairing after an update. To return a commissioned device to a fresh
state without a full erase, use the factory reset button (see
[INSTALL.md](INSTALL.md#factory-reset)) or erase only the `nvs` partition.

The fastest at-a-glance check is the onboard status LED: it blinks
while the device is pairing or attaching to Thread, and goes solid
once it's connected and operational. A solid LED is the healthy
resting state. See the [Status LED reference](INSTALL.md#status-led-reference)
in INSTALL.md for the full pattern table.

In the serial monitor, a healthy boot shows these in order:

- `Project name: t1n_smart_lock` and the expected `App version`,
  confirming your build booted rather than a stale image.
- Each module reporting ready (`lock_pulse`, `ctm_state`, and
  `doors: Doors module ready`), then
  `Door lock cluster initialized`.
- `Server initialization complete`.

A good flash leaves the board booting uncommissioned and advertising
over BLE, shown by a rapid blink on the status LED. That is the
expected resting state for a fresh board. Commissioning it to your
Matter ecosystem and wiring it into the van are covered in
[INSTALL.md](INSTALL.md).

## Reproducing this exact build

The Requirements above are enough to build working firmware. The
registry method also makes the exact build reproducible without any
manual SDK checkout. Three things pin it:

- **ESP-IDF v5.4.4**, the version this release was built with.
- **`main/idf_component.yml`**, which pins `espressif/esp_matter`
  to `1.5` and `espressif/button` to `^4`.
- **`firmware/dependencies.lock`**, which records the exact
  resolved version and content hash of every managed component,
  including esp_matter and its transitive dependencies.

To reproduce the release binary, build with ESP-IDF 5.4.4 against
the committed `dependencies.lock`; the component manager restores
the same versions from the registry. Delete `managed_components/`
and rebuild if you want to force a clean re-resolution against the
lock file.

## Related documentation

- [README](../README.md) — project overview and quick start
- [FIRMWARE.md](FIRMWARE.md) — firmware architecture and behavior
- [HARDWARE.md](HARDWARE.md) — PCB design, BOM, and ordering
- [ENCLOSURE.md](ENCLOSURE.md) — 3D printed enclosure
- [INSTALL.md](INSTALL.md) — commissioning and van installation
- [SAFETY.md](SAFETY.md) — electrical and operational safety
- [CERTIFICATION.md](CERTIFICATION.md) — Matter/Thread certification status
- [CONTRIBUTING.md](CONTRIBUTING.md) — how to contribute
- [LICENSING.md](LICENSING.md) — license terms