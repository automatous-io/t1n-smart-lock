# Safety

**[README](../README.md)** > **Safety** · [Report an issue](../../../issues/new)

> **⚠️ Read before installing.** Installing this device modifies
> your vehicle's electrical system and interfaces with the
> factory locking system. Understand the limitations and accept
> the risks before proceeding.

This document covers what the T1N Smart Lock adds beyond the
factory system, its built-in safeguards, what it does not protect
against, what it requires from the user, known limitations, failure
modes, and disclaimers.

## What the firmware adds beyond the factory system

The firmware adds one behavior the factory module
doesn't have:

**Lock commands are rejected when a door is observed open.** The
firmware watches the master door lock switch (MLS) LEDs on the center console.
When any door is open, the LED for that side blinks while the
central timer module (CTM) is awake. If the firmware sees a blinking LED at the moment a Lock
command arrives, it rejects the command outright. No pulse fires. Apple
Home or Home Assistant returns "No Response" immediately, while
the user is still at the van and can close the offending door.

This check depends on the MLS LEDs being observable, so it functions
while the CTM is awake; when the CTM is asleep the LEDs are dark and
the open-door rejection cannot apply.

This isn't a personal-safety feature. It serves two practical purposes:

- **Honest state in the ecosystem.** Without the block, Apple
  Home or Home Assistant would briefly show LOCKED (the
  optimistic write) before the observation push corrects to
  UNLOCKED about 10 seconds later. That flicker is bad UX,
  especially in vanlife scenarios where users tap Lock and walk
  away. Rejecting at the command level gives immediate,
  accurate feedback.
- **Hardware preservation.** The T1N's master lock cycles all
  doors as a group. Firing a lock pulse while a door is open
  would ask 20-year-old actuators on closed doors to engage
  unnecessarily. Suppressing the command spares some wear,
  though the same actuators can still be cycled manually via
  the center console switch.

Unlock commands do not have the equivalent check by design.
When a door is open, the firmware already reports the van as
unlocked through the observation push, so Apple Home and Home
Assistant show UNLOCKED. The standard ecosystem toggle UI
shows "Lock" as the available action in that state, so the
scenario of a user tapping Unlock with a door open does not
arise in normal use.

More generally, the firmware reports observed reality rather
than commanded state. If any command does not produce the
expected outcome (for example, an unobserved physical
interference), the doors module sees the divergence and
corrects the reported state in Apple Home or Home Assistant
within about 10 seconds.

The factory T1N central locking will cycle the locks with the
slider or driver door wide open. The factory system never checks
door state before locking; the firmware adds that check so the
reported state stays honest and the closed-door actuators aren't
cycled needlessly.

## Built-in safeguards

Beyond the open-door check, the firmware is designed to prevent
unintended lock actuations. These are features the device gives
you. How each one is implemented is documented in
[FIRMWARE.md](FIRMWARE.md#the-pulse-subsystem).

**No spurious actuation at power-on.** During the brief window
between power-up and the firmware taking control, the lock line is
held in its safe, inactive state. Booting or rebooting the device
never trips the locks.

**Commands cannot double-fire.** Only one lock sequence runs at a
time. Overlapping or repeated commands from the Matter ecosystem
are rejected until the active sequence completes, so a burst of
taps or an automation loop cannot stack actuations.

**Rapid cycling is rate-limited.** A 300ms cooldown is enforced
between one lock sequence and the next, so buggy controllers,
malicious automation, or network retry storms cannot cycle the
locks faster than the CTM expects.

**The lock line cannot stick on.** Each pulse is a fixed 500ms and
the line returns to its inactive state immediately afterward. The
firmware cannot accidentally hold the lock line energized
indefinitely.

**State survives power loss.** After a reboot or power loss, the
device restores its last known lock and door state instead of
defaulting to "unknown" or "unlocked." That last known state is
what Apple Home or Home Assistant shows on boot. If nothing
changed during the outage, it is accurate right away; if a door
or lock did change, the device corrects the reported state once
the CTM wakes and the LEDs are observable again.

## What this device does NOT protect against

This is a smart locking retrofit. It does not protect against:

- **Forced entry.** Breaking a window, prying a door, or cutting
  through the body bypasses the locks entirely.
- **Key fob attacks.** Relay attacks, replay attacks, and other
  attacks against the factory key fob system are unaffected by
  this device. The factory immobilizer and remote keyless entry
  remain whatever they were before.
- **Theft of the entire vehicle.** A smart lock secures doors;
  it does not prevent hotwiring, tow away theft, or other
  whole vehicle theft methods.
- **Network-based attacks on Matter.** The Matter protocol has
  its own security model. This device inherits whatever security
  properties Matter and Thread provide; it does not add
  additional protections at the protocol layer.
- **Loss of remote control during network outages.** If your
  Thread Border Router, WiFi network, or internet connection
  is down, remote control is unavailable. Manual key access and
  the factory key fob continue to work normally.
- **Hardware failure of the factory locking system.** The smart
  lock interfaces with the factory central timer module. If the CTM
  or door actuators fail, the smart lock cannot compensate.

## What this device requires from the user

For safe operation:

- **Proper wiring.** The PCB connects to the Sprinter's central
  locking system via the five J3 terminals. Incorrect wiring can
  damage the CTM, the PCB, or both. Verify connections against
  [HARDWARE.md](HARDWARE.md#j3-terminal-wiring) before applying power.
- **An appropriately specified 12V power source.** The board is a
  constant load, drawing about 0.3W (~0.027A at 12V) whether or not
  the van is running. Choosing a source that suits how you use the
  van is your responsibility. The risk to avoid is draining a battery
  you rely on to start the engine. The reference build runs from a
  dedicated house / leisure battery (100Ah). Powering from the starter
  battery should be fine if you drive regularly, but to be safe use a
  separate house battery or add a low-voltage cutoff. See
  [Choosing a 12V source](INSTALL.md#choosing-a-12v-source) for the
  tradeoffs.
- **Overcurrent protection on the 12V power feed.** This applies only
  to the 12V line feeding the buck converter, not the five J3 signal
  taps, which are low-current CTM lines that never carry 12V. The 12V
  feed adds wiring that can chafe or short against the chassis, so put
  a small inline fuse (on the order of 1A, sized to the feed wire) in
  the 12V feed between the tap and the buck input, as close to the tap
  as practical. See
  [Powering the board](INSTALL.md#powering-the-board) for placement.
- **CTM compatibility verification.** The firmware was developed
  and tested against a single 2005 Dodge Sprinter 2500 CTM.
  Other model years, trims, and regional variants may have
  different CTM behavior. Test thoroughly before relying on
  the device.
- **A reliable Thread Border Router on the network.** The device
  requires a Thread Border Router within range of the van. Two
  control modes are possible depending on your setup:

  *Local control (at the van):* an iPhone 15 Pro or newer model
  featuring a Thread radio can serve as the Border Router
  while you're within Thread range of the van. This works without any
  other infrastructure and is sufficient if you only need to control
  the lock when you're physically near the vehicle.

  *Remote control (over the internet):* Requires a stationary
  Border Router at the van's typical parking location, connected
  to your WiFi network. A HomePod mini, Apple TV 4K, Nest Hub,
  Home Assistant with SkyConnect, or similar provides this. With
  this setup, Apple Home or Home Assistant, etc., can control the
  device from anywhere with an internet connection.

  For van use specifically, the stationary setup is strongly
  recommended if remote control matters to you. Local only control
  via iPhone is fine for commissioning and short-range use.
- **Factory key fob and physical key access.** Always keep the
  factory key fob accessible and physical keys available. Never
  rely solely on the smart lock to access the van.
- **Testing before depending on it.** Before treating the device
  as a primary locking control, exercise all lock and unlock
  scenarios manually. Confirm the center console LEDs match what the
  firmware reports.

## Year and regional caveats

The T1N (first generation Sprinter) was sold across multiple
years and markets:

- **Europe:** 1995 to 2006, as Mercedes-Benz Sprinter and
  Mercedes-Benz Transporter T1N
- **North America:** 2002 to 2006, as Freightliner Sprinter
  (from 2002) and Dodge Sprinter (from 2003)

This firmware was developed and tested on a single configuration:

- **2005 Dodge Sprinter 2500**, North American market

Compatibility with any other configuration is unverified.

Potential compatibility concerns:

- **Other T1N model years.** The CTM is broadly similar across
  model years but specific behaviors (sleep timing, LED blink
  patterns, wake response) may vary. Earlier production years
  (1995 to 2001) are particularly unverified and may differ.
- **European Mercedes-Benz Sprinter T1N.** Sold as Mercedes-Benz
  rather than Dodge. CTM hardware, wiring, and center console layout
  may differ from the North American Dodge variant.
- **Freightliner badged North American T1N.** Sold in North
  America alongside Dodge. Likely the same CTM as the Dodge
  variant but unverified.
- **NCV3 (2006 to 2018) and VS30 (2019+) Sprinters.** Newer
  Sprinter generations with different CTM protocols. This
  firmware is not designed or verified for these platforms.

If you're installing on anything other than a 2005 Dodge T1N,
expect to validate the CTM behavior yourself before relying on
the device. Hardware compatibility reports from other variants
are welcome via GitHub issues; the more reports come in, the
better the documentation can become.

## Failure modes

What happens when things go wrong:

- **ESP32 boot or firmware crash.** The device cannot send lock
  commands. The factory key fob and physical keys continue to
  work normally. The Matter ecosystem will show the device as
  unresponsive after a timeout.
- **Thread network loss.** The device retains its last known
  state in non-volatile storage and continues to read the CTM
  LEDs locally, but cannot be controlled remotely until the
  Thread network is restored.
- **Power loss to the device.** The device stops functioning.
  The factory locking system continues to work normally. On
  power restoration, the device boots fresh and re-attaches to
  the Thread network.
- **CTM sleep.** When the CTM enters sleep mode, the center console
  LEDs go dark and the device cannot observe state changes. It
  reports the last known state from before sleep until the CTM
  wakes back up. This is by design; the alternative is reporting
  "unknown," which is less useful for users who want to see
  consistent state.
- **CTM sleep with door state changes.** While the CTM is asleep,
  the firmware cannot observe LED changes and relies on last known
  state. Door state changes during sleep (a door opening or closing)
  are invisible until the CTM wakes. Commands issued during this window
  may not reflect actual conditions until observation resumes.
- **Firmware update interruption.** If a firmware update is
  interrupted, the device may fail to boot. Recovery requires
  physical access to the USB-C port to reflash the firmware.
  See [BUILDING.md](BUILDING.md#flash-your-build) for the reflash procedure.
- **PCB or component failure.** If the device fails entirely,
  remove power, disconnect from the CTM, and the factory
  locking system returns to its original behavior.

## Modifications and your van

Installing the T1N Smart Lock modifies your vehicle's electrical
system. Be aware:

- The factory warranty on T1N Sprinters has long expired by now,
  but any aftermarket extended warranty you may have could be
  affected by electrical modifications.
- Your vehicle insurance may have terms regarding aftermarket
  electrical modifications. Check with your insurance provider
  if you're unsure.
- Installation involves connecting to live vehicle electrical
  circuits. Inexperienced installers should consult a qualified
  technician.

## Disclaimer

This device is provided as-is, without warranty of any kind. By
building, installing, or using the T1N Smart Lock, you accept
all risks associated with modifying your vehicle and relying on
a smart locking retrofit.

AUTOMATOUS.IO is not liable for:

- Damage to your vehicle, its electrical system, or the factory
  CTM during installation or operation
- Loss, theft, or damage resulting from device malfunction,
  network outages, or any other failure of the device to
  perform as expected
- Personal injury or property damage arising from use of the
  device
- Insurance disputes, warranty disputes, or any other legal
  consequences of installing this device in your vehicle

This is an open source hardware project. It is not a certified
commercial product and carries no implicit guarantees of safety,
reliability, or fitness for any particular purpose. Whether you
build it from publicly available designs or order components, you
are responsible for the assembled device's installation and operation
in your vehicle.

If you're not comfortable with these terms, do not build or
install this device.

## Related documentation

- [README](../README.md) — project overview and quick start
- [FIRMWARE.md](FIRMWARE.md) — firmware architecture and behavior
- [HARDWARE.md](HARDWARE.md) — PCB design, BOM, and ordering
- [ENCLOSURE.md](ENCLOSURE.md) — 3D printed enclosure
- [BUILDING.md](BUILDING.md) — building and flashing the firmware
- [INSTALL.md](INSTALL.md) — commissioning and van installation
- [CERTIFICATION.md](CERTIFICATION.md) — Matter/Thread certification status
- [CONTRIBUTING.md](CONTRIBUTING.md) — how to contribute
- [LICENSING.md](LICENSING.md) — license terms