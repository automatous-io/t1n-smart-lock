# Licensing

**[README](../README.md)** > **Licensing** · [Report an issue](../../../issues/new)

The T1N Smart Lock is a personal project, shared so others can
build their own, and it is fully open source. The firmware carries
a software license and the physical design files carry a design
license, because the two kinds of files call for different license
families. Commercial use is allowed for every part, with
attribution, and you never need to ask permission.

## Firmware

The firmware in `firmware/` is licensed under
[Apache 2.0](../firmware/LICENSE.txt), a permissive open source
license approved by the Open Source Initiative. You can use it for
any purpose including commercial, modify it, and include it in
larger works whether open or closed, as long as you keep the
copyright notice and license text, state significant changes, and
include the NOTICE file if one is present.

The firmware started from Espressif's
[esp-matter](https://github.com/espressif/esp-matter) door lock
example. Those example files were released by Espressif into the
public domain under CC0, which places no conditions on reuse, so
the Apache 2.0 license here is a choice rather than something
inherited. One file, `firmware/main/common_macros.h`, is a genuine
Espressif source file that stays under Apache 2.0 with its original
notice. The firmware also builds against the esp-matter SDK and
other Espressif components, fetched at build time, which remain
under their own licenses, Apache 2.0 in most cases.

Apache 2.0 includes a patent grant, but it is a license from
contributors covering their own contributions, with a defensive
termination clause. It does not shield users from patent claims by
unrelated third parties.

## Hardware and enclosure

The PCB design files in `hardware/` (KiCad schematic, layout,
gerbers) and the 3D printed enclosure files in `enclosure/` (STL,
STEP) are licensed under
[Creative Commons Attribution 4.0](../hardware/LICENSE.txt), the
open, attribution-only Creative Commons license. You can build the
lock for yourself, modify the PCB or enclosure, share your changes,
order boards from a fabricator like JLCPCB, print enclosures, and
sell any of it, including assembled units and commercial products.
The only conditions are that you credit AUTOMATOUS.IO with a link back
to this repository and indicate where you changed the design.

CC BY 4.0 is not a software license, but it applies cleanly to
design files and creative work, and it keeps the physical design as
open as the firmware.

## Attribution

The standard attribution string for the firmware, the hardware, or
the enclosure is:

T1N Smart Lock by AUTOMATOUS.IO
https://github.com/automatous-io/t1n-smart-lock

Include it in any redistribution of the source files, in the README
of a fork or derivative project, in documentation that describes
work built on the project, and in promotional material for
derivative works.

## Related documentation

- [README](../README.md) — project overview and quick start
- [FIRMWARE.md](FIRMWARE.md) — firmware architecture and behavior
- [HARDWARE.md](HARDWARE.md) — PCB design, BOM, and ordering
- [ENCLOSURE.md](ENCLOSURE.md) — 3D printed enclosure
- [BUILDING.md](BUILDING.md) — building and flashing the firmware
- [INSTALL.md](INSTALL.md) — commissioning and van installation
- [SAFETY.md](SAFETY.md) — electrical and operational safety
- [CERTIFICATION.md](CERTIFICATION.md) — Matter/Thread certification status
- [CONTRIBUTING.md](CONTRIBUTING.md) — how to contribute
