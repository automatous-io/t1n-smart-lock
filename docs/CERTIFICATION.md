# Matter and Thread Certification

**[README](../README.md)** > **Certification** · [Report an issue](../../../issues/new)

This document explains the Matter and Thread certification status
of the T1N Smart Lock and what "uncertified Matter device" means
in practice.

## Certification status

This firmware is **not Matter Certified** and **not Thread Certified**.

It implements the Matter protocol via Espressif's
[esp-matter](https://github.com/espressif/esp-matter) SDK and
Thread via [OpenThread](https://openthread.io/), but has not been
submitted to the Connectivity Standards Alliance (CSA) or Thread
Group for certification.

## Why not certified

Matter and Thread certification are designed for commercial
products manufactured and distributed at scale. The process
requires paid membership in the certifying organization, per-product
certification fees, and testing through an authorized lab. Together
these run into thousands of dollars per year plus a per-device cost.

That cost structure is built for companies shipping volume products,
not for an open source design that individuals build for
themselves. The license allows selling a commercial version of the
hardware without asking permission (see
[LICENSING.md](LICENSING.md#hardware-and-enclosure)), so anyone who
did would take on certification as part of that.

## What this means for users

The device appears in Matter ecosystems as an uncertified Matter
device. Most ecosystems show a one time warning during
commissioning:

- **Apple Home:** "Uncertified Accessory" prompt with "Add Anyway"
- **Google Home:** "This device hasn't been certified" with an
  "Add anyway" option
- **Amazon Alexa:** Similar uncertified device prompt
- **Home Assistant:** May or may not prompt depending on version

This is normal and expected behavior. Tapping "Add Anyway" or the
equivalent option proceeds with commissioning. Once commissioned,
the device functions identically to a certified Matter device
within your fabric.

## Test credentials

The firmware uses the publicly available ESP-Matter test VID/PID.
These are standard development credentials published by the CSA for
non-commercial use during firmware development.

Because every unit built from this firmware uses the same test
credentials, they all share the same commissioning setup code and
QR. That is fine for personal use, but it means the setup code is
not a secret, and if you build more than one, commission them one
at a time to avoid ambiguity during pairing.

Once a device is commissioned to your Matter fabric, the setup
code is no longer used for authentication. Your Matter ecosystem
manages credentials going forward.

Test credentials are appropriate for personal use, development,
and open source distribution. They are not appropriate for any
commercial product, which would require obtaining a real VID/PID
from the CSA and certifying the device.

## Trademarks

- **Matter** is a trademark of the Connectivity Standards Alliance.
- **Thread** is a trademark of the Thread Group.

This project is not affiliated with, endorsed by, or sponsored by
either organization.

## Related documentation

- [README](../README.md) — project overview and quick start
- [FIRMWARE.md](FIRMWARE.md) — firmware architecture and behavior
- [HARDWARE.md](HARDWARE.md) — PCB design, BOM, and ordering
- [ENCLOSURE.md](ENCLOSURE.md) — 3D printed enclosure
- [BUILDING.md](BUILDING.md) — building and flashing the firmware
- [INSTALL.md](INSTALL.md) — commissioning and van installation
- [SAFETY.md](SAFETY.md) — electrical and operational safety
- [CONTRIBUTING.md](CONTRIBUTING.md) — how to contribute
- [LICENSING.md](LICENSING.md) — license terms