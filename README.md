# T1N Smart Lock

The T1N Smart Lock is an open source Matter over Thread device designed
for the 2005 Dodge Sprinter 2500 (T1N chassis). It adds what the van never
had: lock and unlock from your iPhone, lock state you can see at a glance,
and a sensor for any door left ajar. It does this without driving the lock
motors directly or replacing the factory module. The factory keys and key fob
keep working exactly as before.

The factory service manual identifies the wires, but does not describe reading
the lock-state LEDs, using the central timer module (CTM) rail to tell when the
van is asleep, or pulsing the locks once or twice depending on state. That approach
is the idea behind this project. Every signal and timing it depends on was worked out
by hand, with a multimeter at the back of the master door lock switch and a long stretch
of testing in the van. The manual gives you the pinout; it does not give you the design.

This device reports observed reality, not commanded state. So your app reflects what
the van actually did, not just what it was told to do.

<p align="center">
  <a href="docs/media/t1n-smart-lock-demo.mp4">
    <img src="docs/media/t1n-smart-lock-enclosure-top.jpeg" alt="T1N Smart Lock in its 3D printed enclosure, top view" height="360">
  </a>
  <br>
  <sub>Click to watch a short demo: opening/closing driver door, then locking and unlocking, with the master lock switch LEDs and Apple Home following each change.</sub>
</p>

It is built primarily for offgrid and vanlife use. It can run from either the
van's starter battery or a house/leisure battery; see
[INSTALL.md](docs/INSTALL.md#choosing-a-12v-source) for the tradeoffs.
It tolerates a noisy electrical environment including engine-on
conditions, and is a low-power Matter device so the power budget stays
small. It is local-first: it pairs and operates over Thread directly
from an iPhone 15 Pro or newer model featuring a Thread radio, and also
works through any compatible Thread Border Router for use in Apple Home,
Home Assistant, Google Home, or Alexa.

## How it works

The board is a small carrier for a Seeed Studio XIAO ESP32-C6, which
provides the Thread radio and runs the firmware. A transistor pulses
the factory master lock line, two optocouplers read the center console
switch LEDs without loading them, and a comparator turns the CTM sense
line into clean edges for sleep detection. The only connection to the
van is five wires tapped at the back of the master door lock switch,
landed on the board's five-position terminal block.

In your Matter app the device appears as a door lock plus two contact
sensors, one for the driver side and one for the passenger, cargo, and
rear doors. For the full architecture see [FIRMWARE.md](docs/FIRMWARE.md).

<p align="center">
  <a href="docs/media/t1n-smart-lock-home-with-doors.png">
    <img src="docs/media/t1n-smart-lock-home-with-doors.png" alt="The T1N Smart Lock in Apple Home, shown as a lock and two door sensors" height="360">
  </a>
  <a href="docs/media/t1n-smart-lock-ha.png">
    <img src="docs/media/t1n-smart-lock-ha.png" alt="The same lock paired in Home Assistant" height="360">
  </a>
  <br>
  <sub>The same lock in Apple Home and Home Assistant. Matter is multi-admin, so it pairs to more than one ecosystem at once (see <a href="docs/INSTALL.md#commission-the-device">Commission the device</a>).</sub>
</p>

## Documentation

Everything is in [`docs/`](docs/). A typical path is to read SAFETY
first, build the board and enclosure, flash the firmware, then
commission and install it in the van.

| Document | What's in it |
|---|---|
| [FIRMWARE.md](docs/FIRMWARE.md) | firmware architecture and behavior |
| [HARDWARE.md](docs/HARDWARE.md) | PCB design, BOM, and ordering |
| [ENCLOSURE.md](docs/ENCLOSURE.md) | 3D printed enclosure |
| [BUILDING.md](docs/BUILDING.md) | building and flashing the firmware |
| [INSTALL.md](docs/INSTALL.md) | commissioning and van installation |
| [SAFETY.md](docs/SAFETY.md) | electrical and operational safety |
| [CERTIFICATION.md](docs/CERTIFICATION.md) | Matter/Thread certification status |
| [CONTRIBUTING.md](docs/CONTRIBUTING.md) | how to contribute |
| [LICENSING.md](docs/LICENSING.md) | license terms |

## Repository layout

| Path | Contents |
|---|---|
| [`firmware/`](firmware/) | C++ application on ESP-IDF (C/C++), source in `main/` |
| [`hardware/`](hardware/) | KiCad project, schematic, and gerbers |
| [`enclosure/`](enclosure/) | 3D printed enclosure model files |
| [`docs/`](docs/) | all project documentation |

## Status and compatibility

This is v1.0 of a personal project, developed and
tested by a single maintainer. It is an uncertified Matter device that
uses public test credentials, which is normal for a self-built device
and explained in [CERTIFICATION.md](docs/CERTIFICATION.md). Response
times on issues vary.

The firmware was developed and verified against one vehicle.
Compatibility with any other year, market, or trim is unconfirmed. If
you run it on a different T1N, a compatibility report is welcome. The
list below will grow as verified reports come in.

| Vehicle | Market | Status | Notes |
|---|---|---|---|
| 2005 Dodge Sprinter 2500 | North America | Verified | Reference build |

Firmware, PCB, and enclosure version history, and which revisions are
validated to work together, are recorded in [CHANGELOG.md](CHANGELOG.md).

## Safety

Installing this device modifies your vehicle's electrical system and
interfaces with the factory locking system. Wiring errors can damage
the CTM or the board. Read [SAFETY.md](docs/SAFETY.md) before building
or installing anything.

## Other projects from Automatous

| Project | What it is |
|---|---|
| [shelly-1-gen4-matter-thread](https://github.com/automatous-io/shelly-1-gen4-matter-thread) | open source Matter over Thread firmware for the Shelly 1 Gen4 |

## Licensing

This repository is multi-licensed. The firmware in `firmware/` is
[Apache 2.0](firmware/LICENSE.txt). The hardware in `hardware/` and the
enclosure in `enclosure/` are both CC BY 4.0
([hardware](hardware/LICENSE.txt), [enclosure](enclosure/LICENSE.txt)).
Both are open source. Use,
modification, sharing, and commercial use are all allowed, with
attribution. See [LICENSING.md](docs/LICENSING.md) for the plain
language version.
