# Firmware

**[README](../README.md)** > **Firmware** · [Report an issue](../../../issues/new)

This document explains the firmware architecture of the T1N Smart
Lock: how it observes vehicle state, how it commands the central
locking system, the design decisions that shaped the
implementation, and the validation that supports the v1.0 release.

The firmware runs on a XIAO ESP32-C6 and exposes the device to
Matter over Thread. It builds on Espressif's
[esp-matter](https://github.com/espressif/esp-matter) SDK and the
upstream OpenThread implementation. Application-specific code
lives in `firmware/main/`.

For the hardware this firmware drives, see
[HARDWARE.md](HARDWARE.md). For building and flashing, see
[BUILDING.md](BUILDING.md). For installation, see
[INSTALL.md](INSTALL.md). For safety, limitations, and failure modes,
see [SAFETY.md](SAFETY.md). A full list of related documents is in
[Related documentation](#related-documentation) at the end.

## Contents

- [Design philosophy](#design-philosophy)
- [Hardware interfaces](#hardware-interfaces)
- [Module map](#module-map)
- [Threading model](#threading-model)
- [The CTM model](#the-ctm-model)
  - [Stock CTM behavior](#stock-ctm-behavior)
  - [What the firmware observes](#what-the-firmware-observes)
  - [What the firmware commands](#what-the-firmware-commands)
  - [Why this approach](#why-this-approach)
- [The doors module](#the-doors-module)
  - [LED classification](#led-classification)
  - [Lock state derivation](#lock-state-derivation)
  - [Door state derivation](#door-state-derivation)
  - [Live vs stability-gated reads](#live-vs-stability-gated-reads)
  - [NVS persistence](#nvs-persistence)
  - [Blackout windows](#blackout-windows)
- [Lock command flow](#lock-command-flow)
  - [Command cooldown](#command-cooldown)
  - [Door open rejection](#door-open-rejection)
  - [Pulse count decision](#pulse-count-decision)
  - [The asymmetric pulse rule](#the-asymmetric-pulse-rule)
  - [Unlock command flow](#unlock-command-flow)
- [The pulse subsystem](#the-pulse-subsystem)
  - [Pulse width and sequence timing](#pulse-width-and-sequence-timing)
  - [Atomic single-sequence guarantee](#atomic-single-sequence-guarantee)
  - [Inter-sequence cooldown](#inter-sequence-cooldown)
  - [Boot-time safety](#boot-time-safety)
- [State reporting](#state-reporting)
  - [Observation push mechanism](#observation-push-mechanism)
  - [Optimistic write synchronization](#optimistic-write-synchronization)
  - [Eventual consistency](#eventual-consistency)
- [Building from source](#building-from-source)
- [Validation matrix](#validation-matrix)
- [Known limitations](#known-limitations)
- [Related documentation](#related-documentation)

## Design philosophy

The firmware is built on one core idea: report observed reality,
not commanded state.

Most aftermarket smart locks fire a command and assume it worked.
The Matter ecosystem then shows whatever the command requested,
which is correct most of the time and silently wrong the rest. A
pulse that didn't engage, a key fob that unlocked the van from
outside, a door that someone left open; these conditions silently
diverge from the optimistic state in the app.

The T1N Smart Lock takes a different approach. The Sprinter's
center console has master door lock switch (MLS) LEDs that already encode
the truth about lock and door state while the CTM is awake. The
firmware reads those LEDs the same way a human does, classifies
the patterns, and reports observed reality back to Matter. The
optimistic write from a Lock command is treated as a hypothesis
to be verified against observation, not a fact to be trusted.

This single decision shapes most of the architecture below. See
[SAFETY.md](SAFETY.md#what-the-firmware-adds-beyond-the-factory-system)
for how observation-first design bounds what the device can do wrong.

## Hardware interfaces

The lock and sensor interface uses four GPIO pins on the
ESP32-C6, each owned by a dedicated module (the antenna, status
LED, and reset button modules own additional pins, documented in
their own sections):

| GPIO | Direction | Module | Purpose |
|------|-----------|--------|---------|
| GPIO1 | Output | `lock_pulse` | Master lock pulse output |
| GPIO2 | Input | `ctm_state` | CTM sleep-detection signal |
| GPIO18 | Input | `doors` | PAX side MLS LED state |
| GPIO20 | Input | `doors` | Driver side MLS LED state |

GPIO ownership is exclusive; no module touches a pin that's owned
by another module. This keeps the interaction model clean and
makes it possible to reason about hardware safety properties
module by module.

The hardware design itself is documented in
[HARDWARE.md](HARDWARE.md), including the schematic, BOM, and
PCB layout. This document focuses on what the firmware does with
those signals.

## Module map

The firmware is organized into nine application-level modules
plus the Matter/CHIP stack provided by esp-matter:

**`app_main.cpp`** is the entry point. Configures Matter endpoints
(one door lock, two contact sensors), calls module init functions
in dependency order, starts the Matter stack.

**`lock_pulse`** owns GPIO1. Provides a non-blocking interface to
fire 1-pulse or 2-pulse sequences on the master lock line. An
internal worker task handles timing.

**`ctm_state`** owns GPIO2. Counts edges on the LM393 output,
applies a 5-second sliding window and sticky classifier, reports
whether the CTM is currently awake or asleep.

**`doors`** owns GPIO18 and GPIO20. Samples the MLS LEDs,
classifies each as SOLID, OFF, BLINK, or UNKNOWN, derives lock
and door states per side, persists state to NVS, pushes state to
Matter when observation diverges from the last reported value.

**`door_lock`** is the Matter Door Lock cluster glue. Receives
Lock/Unlock commands, applies command cooldown and door open
rejection, requests pulse sequences from `lock_pulse`, syncs the
optimistic state with the doors module's push tracker.

It also exposes `door_lock_init()`, an empty function whose only
job is to be called once from `app_main`. The cluster command
handlers (`emberAfPluginDoorLockOnDoorLockCommand` and friends)
are weak symbols that esp-matter resolves at link time; without
at least one symbol in this translation unit being referenced
directly, the linker would dead-strip the whole object file and
the handlers would silently vanish. The empty init call exists
only to anchor the file in the binary.

**`door_lock_manager`** holds credential, user, and schedule
storage for the Matter Door Lock cluster. The `BoltLockMgr` class
inside is vendored Espressif example code, used unchanged.
`door_lock` delegates PIN validation, credential operations, and
schedule management to this module.

**`status_led`** owns the XIAO ESP32-C6's onboard LED. Indicates
BLE advertising, Thread network state, and Matter Identify. The
blink patterns are tabulated in
[INSTALL.md](INSTALL.md#status-led-reference).

**`reset_button`** handles the factory reset button input. Clears
Matter commissioning data and restarts the device.

**`antenna`** routes the radio to the external U.FL antenna at boot:
GPIO3 enables the XIAO's onboard RF switch and GPIO14 selects the
external antenna over the built-in ceramic chip. It runs before the
Matter stack starts so the radio comes up on the right antenna, and
has no runtime behavior after that. The external antenna is what
extends Thread range, keeping the lock reachable for direct local
control from an iPhone 15 Pro or newer model featuring a Thread
radio, with no hub, consistent with the local-first design.

Modules communicate through narrow, explicit interfaces. The
doors module subscribes to `ctm_state` events via callback; the
door_lock module calls `lock_pulse_request`, `ctm_state_is_awake`,
and several `doors_*` accessors. There is no shared mutable
state outside the modules' own atomic variables.

## Threading model

The firmware runs on FreeRTOS with the following task topology:

| Task | Priority | Period/Trigger | Module | Purpose |
|------|----------|---------------|--------|---------|
| `app_main` | (boot only) | once | `app_main` | Initialize subsystems, start Matter |
| Matter/CHIP | (managed) | event-driven | esp-matter | Matter protocol stack |
| `lock_pulse` | 10 | semaphore-driven | `lock_pulse` | Fire pulse sequences |
| `ctm_monitor` | 5 | 1000ms tick | `ctm_state` | Edge counting, classifier update |
| `doors_sample` | 5 | 50ms tick | `doors` | LED sampling and classification |
| `status_led` | 5 | 100ms tick | `status_led` | LED pattern updates |

Plus interrupts:

| ISR | Source | Module | Purpose |
|-----|--------|--------|---------|
| GPIO2 edge | LM393 transition | `ctm_state` | Timestamp ring buffer append |

**Priority rationale.** The Matter/CHIP task runs at its default
priority (managed by esp-matter). `lock_pulse` runs at priority
10, higher than background monitor tasks so pulse timing is not
preempted by routine sampling, but lower than the CHIP task so
Matter work is never delayed by a pulse sequence. Background
monitor tasks (`ctm_monitor`, `doors_sample`, `status_led`) all
run at priority 5, which is sufficient for their periodic work.

**Cross-task safety.** State shared between tasks uses
`std::atomic` (default sequentially consistent ordering), with
`volatile` for the ISR-shared edge-timestamp ring buffer and for
the status LED module's simple cross-task state flags. There are
no traditional locks in the application code; the Matter stack
manages its own synchronization internally. The doors module is
the only place where multiple tasks might race (the sampler task
updates state, the door_lock task reads classifier output for the
BLINK check), and that race is safe because the reads are
point-in-time snapshots without compound state.

**Callback contract.** Modules that accept callbacks
(`ctm_state_register_callback`, `lock_pulse_register_complete_cb`)
fire the callback from the module's own task context. Callbacks
must not block. Implementations either set a flag or post to a
queue and return immediately. Blocking a callback delays the
module's primary work for the entire system.

## The CTM model

The central timer module (CTM) is the Sprinter's body control
unit. It handles central locking, interior lighting, the alarm
system, key fob processing, and various other low-voltage
functions. The CTM is the only component the firmware
communicates with; the rest of the vehicle's electrical system is
untouched.

### Stock CTM behavior

The CTM operates in two states: awake and asleep.

**Awake** is the active state. The center console MLS LEDs are
illuminated according to lock and door state. The CTM accepts
master lock pulses on the WT_YL line, responds to key fob
commands, processes door switch inputs, and continues to drive
any active loads (interior lights, etc.). The WT_RD signal,
traditionally the passenger-side (front passenger, slider, and
rear cargo) lock-pulse line, carries a pulse train at roughly
89 Hz when awake. The firmware repurposes WT_RD as a read-only
sleep-detection signal and never drives it.

**Asleep** is a power-saving state. After approximately 15
minutes of inactivity (no door switch activity, no MLS press, no
key fob event, no ignition), the CTM transitions to sleep. The
MLS LEDs go dark. The CTM is still powered and listening but
consumes minimal current. The WT_RD signal carries a slower pulse
train at approximately 47 Hz; this is how the firmware detects
sleep state.

The 15-minute timeout is typical but not universal. Locking the
van from inside via the door handle latches appears to trigger a
faster transition to sleep in edge cases; the CTM treats this as
a "the user has secured the vehicle and is leaving" signal. Other
variations exist across model years and trim levels.

Wake transitions happen on any of: a key fob press, a master door lock
switch press, a door switch event (any door opening or closing),
the ignition being turned on, or a master lock pulse on WT_YL
(the firmware's only output, via GPIO1). A passenger-side pulse
on WT_RD also wakes the CTM, but the firmware only observes
WT_RD, it never drives it.

These triggers usually wake the CTM, but not always. After the module
has been asleep for an extended period, on the order of hours, a door
event that would normally wake it sometimes does not, apparently a
deeper power-conserving behavior. This has been observed in the field;
the threshold and exact conditions are not characterized. When it
happens, the firmware keeps reporting last-known state until some later
event does wake the CTM.

The CTM treats the master lock as a single group action. A pulse
on the WT_YL line toggles the lock state of all doors together;
there is no per-side control at this interface. Individual lock/unlock
via interior door handles works (mechanical override). These events
never appear on the WT_YL line; whether they update the MLS LEDs depends
on CTM state, as described under the doors module.

### What the firmware observes

The firmware passively reads three signals from the CTM:

**WT_RD pulse rate** (via LM393 on GPIO2). Edge counts over a
5-second window tell the firmware whether the CTM is awake (~448
edges) or asleep (~238 edges). The signal is continuous in both
states; the rate is what differs. See the `ctm_state` module for
the detection threshold and sticky classifier that filters
transients.

**Driver-side MLS LED** (via PC817 optocoupler on GPIO20). The
driver LED reflects driver side door state. The LED is solid on
when the driver door is closed and locked, off when closed and
unlocked, and blinking when open.

**PAX-side MLS LED** (via PC817 optocoupler on GPIO18). The
PAX side LED reflects the combined state of the front passenger
door, sliding cargo door, and rear cargo doors. All three are
wired to the same LED signal on the T1N, so the firmware
represents them as one logical group via a single Matter contact
sensor endpoint.

The firmware never writes to these signals. They are read-only
observations of the CTM's state.

### What the firmware commands

The firmware drives one signal:

**WT_YL master lock pulse** (via GPIO1). A pulse on WT_YL is what
the center console MLS button physically
produces when pressed. The CTM treats the firmware-driven pulse
identically to a button press. There is no protocol; the pulse is
a level-triggered request to toggle the master lock state.

Pulse duration is 500ms, matching the timing of a deliberate
button press. Pulse sequences (one or two pulses with a 500ms
gap) handle the two cases of "CTM awake, just toggle" and "CTM
asleep, wake then toggle."

### Why this approach

The decision to read LEDs and pulse a single line, rather than
talk to the CTM via CAN bus or replace the module entirely, is
deliberate. The safety consequences of this choice are detailed
in [SAFETY.md](SAFETY.md#what-the-firmware-adds-beyond-the-factory-system);
the design intent is summarized here.

**OEM-respectful.** No CAN bus injection, no module replacement,
no firmware modification of factory components. The CTM operates
exactly as Mercedes designed it. The smart lock is a peripheral
that observes and nudges; it is not a replacement for the
original system.

**Bounded blast radius.** A firmware crash, a PCB failure, a
botched OTA; none of these affect the factory locking. Remove
power from the device and the van behaves exactly as it did from
the factory. The factory key fob and interior key cylinders
continue to work in all cases.

**Passive observation.** The CTM doesn't know the firmware is
listening. There is no handshake, no authentication, no protocol
state that could get out of sync. The firmware reads the same
signals a human technician with a probe would read.

**Future maintenance.** The LEDs and pulse lines are
analog-domain signals, not protocol traffic that depends on a
manufacturer's continued support.

## The doors module

The doors module is the largest and most complex application
module. It samples the two MLS LEDs continuously, classifies
their patterns, derives lock and door states for each side,
persists those states across power loss, and pushes updates to
Matter when observation diverges from what the ecosystem
currently shows.

### LED classification

Each MLS LED encodes its side's state through one of four
observable patterns:

**SOLID** (steady on): the side is closed and locked. The LED
reads continuously on. (The PC817 pulls the GPIO low when the LED
is on; `led_read_logical_on` inverts this, so a lit LED is a
logical-high sample.)

**OFF** (no illumination): with CTM awake, the side is closed and
unlocked. With CTM asleep, the LED is simply dark and the state
is undetermined; sampling is suspended in this case.

**BLINK** (alternating): the side has at least one door open. The
LED toggles at roughly 1 Hz (~500ms half-period). Detection
requires temporal observation; a single GPIO read cannot
distinguish solid-on from the high phase of a blink cycle.

**UNKNOWN**: pre-init state, before any sample has been recorded.
The classifier returns UNKNOWN only while the history buffer is
empty.

The sampler task runs every 50ms, appending the raw on/off
reading to a 30-slot (1500ms) circular history. Each tick the
classifier walks the history newest-to-oldest, counting
transitions and measuring the length of the most recent
contiguous run, then evaluates in this order:

- **BLINK**: 2 or more transitions in the window. Two transitions
  means at least one full half-cycle is visible; a single
  transition could just be a one-time state change (OFF to SOLID)
  rather than a blink.
- **SOLID**: the newest contiguous run is high and at least
  `SOLID_CONFIRM_SAMPLES` (10 samples / 500ms) long.
- **OFF**: the newest contiguous run is low and at least
  `OFF_CONFIRM_SAMPLES` (4 samples / 200ms) long. The shorter
  confirm time reflects OFF being the safe default.
- **Otherwise**: the classifier holds its previous value (no
  change). This cleanly bridges the brief transient between SOLID
  and OFF.

UNKNOWN is not a steady-state output; it appears only at first
boot before any sample exists. The 1500ms window is longer than
one full blink period, so a blinking LED always shows the two or
more transitions needed to classify as BLINK.

### Lock state derivation

Lock state is binary at the Matter endpoint level: LOCKED or
UNLOCKED. The full state space includes UNKNOWN for the
pre-classifier window at first boot.

Per-side lock state is derived from the LED classifier:
- SOLID = side is locked
- OFF (with CTM awake) = side is unlocked
- OFF (with CTM asleep) = sampling suspended, last-known value
  held
- BLINK = side has a door open; treated as unlocked (0) and
  committed through the same stability gate as OFF (an open door
  means the side is not locked)
- UNKNOWN = no commitment yet

The vehicle-level lock state combines both sides: the van is
LOCKED only when both driver and PAX last-stable-locked values
are 1 (both sides solidly locked, no doors open). Any other
combination reports UNLOCKED. Once both sides have committed a
stable reading after first boot, UNKNOWN can never reappear at
the vehicle level.

Manual single door lock/unlock via the interior door handle
appears to be observed only when the CTM is awake and driving the
MLS LEDs (for example, just after a door switch event). When the
CTM is asleep, as it is after the van has sat idle, even with
someone inside, the LEDs stay dark, so the firmware cannot see
the change until the next observable lock event (MLS press, key
fob, door cycle, or a pulse from this module).

### Door state derivation

Door state is binary per side: OPEN or CLOSED. Derived from the
classifier output:
- BLINK = door open
- SOLID or OFF = door closed
- UNKNOWN = no commitment

The two Matter contact sensor endpoints (driver and PAX) report
this state. The PAX endpoint represents the combined state of
the front passenger door, sliding cargo door, and rear cargo
doors, since they share a single LED signal on the T1N.

### Live vs stability-gated reads

The doors module exposes two kinds of accessors for each state:

**Live classifier reads** (`doors_driver_classifier_is_blink`,
`doors_pax_classifier_is_blink`) return the current classifier
output. These reflect physical reality within one classifier
window (~1.5 seconds) but include transient states the classifier
hasn't yet stabilized on.

**Stability-gated reads** (`doors_driver_is_open`,
`doors_pax_is_open`, `doors_driver_is_locked`,
`doors_pax_is_locked`, `doors_vehicle_is_fully_locked`,
`doors_compute_lock_state`) return the committed stable value.
These require 10 seconds of consistent classifier output before
committing to a new value.

The 10-second stability gate filters out the CTM's pre-sleep LED
fade-down window (observed ~8 seconds between LED dim and the
CTM officially sleeping). Without the gate, the classifier would
push wrong states to Apple Home or Home Assistant as the LEDs
fade during the transition to sleep.

Cost: legitimate door open/close events lag in the Matter
ecosystem by up to 10 seconds. This is acceptable because users
are typically not watching the app at the moment they open or
close a door. The lock command pre-check uses the live read
instead, so a freshly-closed door is accepted within ~1.5 seconds
rather than the full 10-second gate.

### NVS persistence

Both lock state and door state are persisted to NVS per side.
Writes go through the stability gates: a classifier transition
must hold continuously before being committed to the in-RAM
`last_stable_*` field and written to NVS.

NVS persistence serves two purposes:

**Boot-time state recovery.** At boot, the firmware reads the
persisted state before the classifier has accumulated history.
Apple Home or Home Assistant sees the last-known state
immediately rather than UNKNOWN. After the classifier window
settles, persisted values are verified or updated against fresh
observation.

**Survival of CTM sleep with state changes.** If a door is opened
while the CTM is asleep, the LED is dark and the firmware cannot
observe the change. The persisted state holds the last-known
value until the CTM wakes and the gate commits to the new value.

NVS writes are bounded. Lock state changes maybe 2 to 10 times
per day in typical van use. Door state may change 20 to 40 times
per day from in-and-out events. Combined writes stay well under
NVS wear limits over a 10+ year lifetime.

### Blackout windows

LED-affecting events generate transient activity that the
classifier would otherwise interpret as fake state changes. All
blackouts use a 1500ms window (one full classifier window):

**CTM asleep to awake.** The LEDs power on with transient
activity as the CTM resumes driving them. The doors module
subscribes to `ctm_state` wake events and starts a blackout on
each wake.

**CTM awake to asleep.** The LEDs fade out with transient
activity. NVS already has the right value from continuous writes
before sleep, so the blackout protects against misclassifying
the fade-down.

**Boot.** The first sampling window after init; the classifier
hasn't accumulated history yet.

**Post-pulse** (exported but not currently called). After a lock
pulse sequence completes, the LEDs transition between states
with brief intermediate activity. A future `lock_attempt` module
will start a blackout here.

During blackout, the classifier still runs internally
(accumulating history) but no Matter pushes occur. After
blackout, the settled state is pushed once.

## Lock command flow

Lock and Unlock commands enter through Matter Door Lock cluster
callbacks (`emberAfPluginDoorLockOnDoorLockCommand`,
`emberAfPluginDoorLockOnDoorUnlockCommand`). Both flow through
the same internal logic with one key difference: only the Lock
command has a door open pre-check.

End to end, a Lock tap moves through these stages, each detailed
in the subsections below:

1. Command cooldown check (reject if within 2s of the last
   command)
2. Door open pre-check (reject if either side reads BLINK)
3. Pulse count decision (1 or 2 pulses, from CTM state and the
   asymmetric rule)
4. Enqueue the sequence via `lock_pulse_request` (non-blocking)
5. `BoltLockMgr` optimistic write of LOCKED to the Matter
   attribute
6. Sync the doors push tracker and open the 25s quiet window
7. Pulses fire asynchronously; observation later confirms the
   optimistic write or issues a correcting push

Unlock follows the same path without step 2.

**AutoRelock.** `app_main` sets the Door Lock cluster's
`AutoRelockTime` to 0, disabling auto-relock. This is deliberate:
auto-relock-while-loading would lock users out mid-task during
the long in-and-out activity typical of vanlife. esp-matter does
not persist this attribute, so it also resets to 0 on every
reboot, which, given the default-off choice, is a non-issue
rather than a bug.

### Command cooldown

A 2-second cooldown is enforced between consecutive Lock or
Unlock commands at the Matter cluster level. This debounces user
intent: mashed buttons, controller retries, double-tap behaviors
from various ecosystems.

The cooldown is shared between Lock and Unlock. Any command
suppresses subsequent commands of either type during the
cooldown window. This is intentional. A user who taps Lock then
immediately taps Unlock is probably confused or fighting an
automation; serializing the requests with a delay gives the
system time to actually respond to the first command.

The cooldown is separate from the hardware-level inter-sequence
cooldown in `lock_pulse` (300ms). Both apply, in series. The
command cooldown debounces user intent; the pulse cooldown
debounces hardware actuation.

### Door open rejection

For Lock commands only: if either side's live classifier reads
BLINK at command time, the command is rejected outright. No
pulse fires. The Matter ecosystem returns "No Response" to the
user immediately. This is the firmware's headline behavior beyond the
factory system; [SAFETY.md](SAFETY.md#what-the-firmware-adds-beyond-the-factory-system)
covers why it matters and how the stock CTM differs.

The check uses the live classifier rather than the
stability-gated value. A freshly-closed door is accepted within
~1.5 seconds instead of the full 10-second stability gate. The
inverse miss (door just opened, classifier hasn't updated yet)
is rare and caught by the observation push once the quiet
window expires (see [State reporting](#state-reporting)).

Unlock commands do not have the equivalent check. When a door is
open, the firmware already reports the van as unlocked through
the observation push, so Apple Home and Home Assistant show
UNLOCKED. The standard ecosystem toggle UI shows "Lock" as the
available action in that state; an Unlock tap with a door open
does not arise in normal use.

### Pulse count decision

If the door open check passes (or for Unlock, unconditionally),
the firmware enters `request_pulses_for_ctm_state`. This
function decides whether to fire 1 pulse or 2.

The decision uses two inputs:
- `ctm_state_is_awake()`: is the CTM currently awake?
- `doors_lock_state_needs_sync_pulse()`: are we in the specific
  asymmetric partial state that requires a sync pulse?

The matrix:

| CTM | Sync needed? | Pulses |
|-----|--------------|--------|
| Awake | No | 1 |
| Awake | Yes | 2 |
| Asleep | (irrelevant) | 2 |

When the CTM is asleep, the first pulse wakes it and the second
toggles the lock. When the CTM is awake but the lock state is in
the asymmetric partial state, the first pulse syncs both sides
to a known state and the second toggles to the target. Otherwise,
one pulse is enough.

### The asymmetric pulse rule

The T1N CTM treats the PAX side as authoritative for partial lock
states. When the van is in a partial state (one side locked, one
unlocked) and a single pulse is fired, the CTM syncs the driver
side to match PAX, not the other way around.

The implications:

**Driver SOLID + PAX OFF** (driver locked, PAX unlocked): a
single pulse would sync driver down to unlocked, opposite of user
intent. The firmware fires 2 pulses instead: pulse 1 syncs both
to unlocked, pulse 2 toggles both to locked. Net result: van
fully locked on one Lock command.

**Driver OFF + PAX SOLID** (driver unlocked, PAX locked): a
single pulse correctly syncs driver up to locked. Both sides end
up locked. Standard 1-pulse path.

**Both SOLID or both OFF**: no asymmetry, standard 1-pulse path.

**Either BLINK or UNKNOWN**: the asymmetric rule does not apply
(states aren't definitively in the partial pattern), default to
1-pulse path. The door open rejection has already filtered out
the BLINK case for Lock commands.

The `doors_lock_state_needs_sync_pulse` function encodes this
logic, returning true only for the driver-SOLID PAX-OFF case.

This rule was characterized through hardware testing. Earlier
firmware versions used a simpler always-1-pulse path and
exhibited the "tap Lock, get fully unlocked" failure mode in
partial states. The 2-pulse sync handles the asymmetry cleanly.

### Unlock command flow

Unlock commands follow the same path minus the door open check:

1. Command cooldown check
2. `request_pulses_for_ctm_state` (same pulse count logic, which
   internally calls `lock_pulse_request` to enqueue pulses)
3. Update `BoltLockMgr` in-memory state to UNLOCKED
4. Sync the doors push tracker with the optimistic write
5. Mark cooldown complete

The shared `request_pulses_for_ctm_state` means Unlock also
benefits from the asymmetric pulse rule. An Unlock command in
the driver-SOLID PAX-OFF state would fire 2 pulses (pulse 1 syncs
both to unlocked, pulse 2 toggles to locked, which is opposite
of Unlock intent).

This is intentional: the CTM is a toggle, not a state machine.
The firmware doesn't track "what direction does this pulse
move?" It only tracks "what sequence of pulses produces the
target state from current state?" For Unlock, the target is
UNLOCKED, and the sequence is whatever produces that from the
current observed state.

In practice, this means Unlock commands from partial states may
produce counterintuitive behavior. The Matter ecosystem will
correct via the observation push, so the user sees the truth
eventually. Future work could split the pulse logic into
direction-aware paths.

## The pulse subsystem

The `lock_pulse` module is the bottom layer of the lock command
stack. It owns GPIO1, fires pulse sequences with deterministic
timing, and provides safety guarantees against unintended
actuation.

### Pulse width and sequence timing

Three timing constants govern pulse behavior:

```
PULSE_WIDTH_MS       = 500
CTM_WAKE_DELAY_MS    = 500
SEQUENCE_COOLDOWN_MS = 300
```

**Pulse width** (500ms) is the duration GPIO1 is held HIGH for
each pulse. This matches the timing of a deliberate MLS button
press, ensuring the CTM treats the firmware pulse identically to
a human press.

**Wake delay** (500ms) is the gap between pulse 1 and pulse 2 in
a 2-pulse sequence. This matches the CTM's documented wake
latency: pulse 1 wakes the CTM, the wake delay gives the CTM
time to be ready to receive a lock command, and pulse 2 acts.

**Sequence cooldown** (300ms) is the minimum gap between the end
of one sequence and the start of the next. This is hardware-
level debounce; it prevents back-to-back sequences from cycling
the CTM faster than its electronics expect.

All three are compile-time constants. The firmware cannot
accidentally hold the line high indefinitely; `vTaskDelay`
provides deterministic timing, and the GPIO is driven LOW
immediately after the timed delay completes.

### Atomic single-sequence guarantee

The `lock_pulse_request` function uses an atomic
compare-and-exchange on a `s_busy` flag to ensure only one pulse
sequence runs at a time:

```cpp
bool expected = false;
if (!s_busy.compare_exchange_strong(expected, true)) {
    return LOCK_PULSE_RESULT_SUPPRESSED_BUSY;
}
```

This is the false-to-true transition. If `s_busy` is already
true, the compare-and-exchange fails and the request is rejected
with a logged warning. Subsequent requests during an active
sequence cannot interleave, interrupt, or stack.

The flag is cleared by the worker task after the sequence
completes, before the completion callback fires. A callback that
itself enqueues another sequence will succeed if the cooldown
allows.

### Inter-sequence cooldown

After a sequence completes, the next sequence cannot start until
`SEQUENCE_COOLDOWN_MS` (300ms) has elapsed. This is checked
before the busy flag claim, so a cooldown rejection doesn't
temporarily lock out other state:

```cpp
int64_t since_last_us = now_us - s_last_sequence_end_us.load();
if (since_last_us < (int64_t)SEQUENCE_COOLDOWN_MS * 1000) {
    return LOCK_PULSE_RESULT_SUPPRESSED_COOLDOWN;
}
```

This protects against rapid back-to-back actuation requests from
buggy controllers, malicious automation, or network retry storms.
The CTM expects time between actuations; the firmware enforces
that minimum.

### Boot-time safety

`lock_pulse_init` runs before `esp_matter::start` in `app_main`.
This ordering is important: the GPIO is configured and driven
LOW before any Matter code can possibly fire a callback that
could request a pulse.

The init sequence:
1. Configure GPIO1 as OUTPUT with internal pull-down enabled
2. Drive GPIO1 LOW explicitly (`gpio_set_level(GPIO, 0)`)
3. Create the work semaphore
4. Spawn the worker task

The pull-down enable plus the explicit LOW drive is redundant
safety. The line is never floating high during the brief window
where the ESP32 is configuring its peripherals but Matter hasn't
yet taken control. A floating line could otherwise cause a
spurious pulse during the boot sequence.

## State reporting

The doors module owns state reporting to Matter. The observation
push is the mechanism that makes "observed reality, not commanded
state" possible.

### Observation push mechanism

On each sampling tick (every 50ms), once past any active blackout
window, the doors module computes the current vehicle-level lock
state (and per-side door states) from the stability-gated values.
It compares this to the `last_pushed_*` tracker variables. If the
computed state differs from the last pushed state, it pushes the
new state to Matter and updates the tracker.

The push uses `DoorLockServer::SetLockState` for the lock
endpoint and the BooleanState cluster setter for the contact sensor
door endpoints. In esp-matter 1.5 the BooleanState cluster is
code driven: its `StateValue` is owned by the connectedhomeip
`BooleanStateCluster` object rather than the esp_matter attribute
store, so `esp_matter::attribute::update` returns
`ESP_ERR_NOT_SUPPORTED`. The push instead looks the cluster up in the
data-model provider registry
(`esp_matter::data_model::provider::get_instance().registry().Get`)
and calls `SetStateValue`, which writes the value, emits the
StateChange event, and marks the attribute dirty. Note that Matter's
BooleanState convention is inverted from intuition: `true` means
contact made (door closed),
so the push inverts the open state before writing it. Matter's
subscription machinery then propagates the state to subscribed
controllers (Apple Home, Home Assistant, etc.) within their
normal polling intervals.

### Optimistic write synchronization

When a Lock or Unlock command is received, the Matter Door Lock
cluster (via `BoltLockMgr`) optimistically writes the new state
to the LockState attribute. This is the "immediate feedback"
behavior that Matter expects: a successful command updates the
attribute immediately, before the device has physically actuated.

The doors module needs to know about this optimistic write,
otherwise its push tracker would think Matter still showed the
old state and would push a redundant update once observation
caught up.

The `doors_set_pushed_lock_state` function syncs the tracker
with the optimistic write. The Lock and Unlock command handlers
call this function after BoltLockMgr's state update. The flow:

1. User taps Lock
2. Command handler calls `request_pulses_for_ctm_state`
3. Command handler calls `BoltLockMgr().Lock(...)` which writes
   LOCKED to the Matter attribute
4. Command handler calls
   `doors_set_pushed_lock_state(LOCKED)` to sync the tracker
5. Pulses fire asynchronously
6. Observation eventually catches up: if reality matches LOCKED,
   no further push; if reality differs, the tracker mismatch
   triggers a correcting push to UNLOCKED

`doors_set_pushed_lock_state` also opens a lock-state push quiet
window (`LOCK_PUSH_QUIET_MS`, 25 seconds). During the window the
observation push skips lock-state updates (contact-sensor pushes
are unaffected). Without it, the stability gate's still-pre-pulse
value would push within one tick and cause a LOCKED to UNLOCKED
to LOCKED flicker in the app on every command. The 25-second
value covers the observed ~21-second command-to-stable-commit
latency (one pulse, CTM awake) plus margin; the 2-pulse wake
sequence adds about a second.

### Eventual consistency

The combination of optimistic writes, the observation push, and
the tracker sync provides eventual consistency: Matter converges
on reality after every change.

Sources of state change that the system handles:

**Firmware-initiated changes** (Lock/Unlock commands): optimistic
write happens immediately; the correcting push, if observation
later diverges, waits for the 25-second quiet window to expire
rather than the 10-second gate.

**External changes** (key fob press, MLS press, mechanical
unlock from inside): no optimistic write, observation pushes the
new state within 10 seconds of the LED transition stabilizing.

**Door state changes** (any door opened or closed): observation
pushes the new state within 10 seconds of the classifier
stabilizing on BLINK or non-BLINK.

The observation-driven window is dominated by the 10-second
stability gate. Faster reporting would require a shorter gate,
which would reintroduce false positives from the CTM's pre-sleep
LED fade-down. The 10-second tradeoff is calibrated against that
specific noise source.

## Building from source

Building the firmware from source and flashing it are covered in
[BUILDING.md](BUILDING.md). This section records only the resulting
image and its update story.

The v1.0 application image is about 1.54 MB. The partition layout
reserves 1.88 MB per application slot, so the binary uses roughly
82%. The build enables the Matter OTA requestor and uses a dual-slot
(A/B) flash layout, so the device is OTA-capable in principle. OTA is
untested in v1.0: no update has been validated through it, and the
reference workflow flashes over USB-C from source (see
[BUILDING.md](BUILDING.md#flash-your-build)). Treat OTA as present but
unsupported until a release validates it. Provider support also varies
by ecosystem (Home Assistant can serve Matter OTA, but not behind an
Apple Thread Border Router).

## Validation matrix

The v1.0 firmware was validated against eleven hardware scenarios on
a 2005 Dodge Sprinter 2500. Each test was performed manually with
serial logging attached, observing the actual behavior of the
CTM, the firmware, and the Matter ecosystem.

| # | Scenario | Expected | Result |
|---|----------|----------|--------|
| 1 | Apple Home, no PIN, CTM awake, both sides locked, send Unlock | Single pulse fires, van unlocks, Apple Home shows UNLOCKED | PASS |
| 2 | HA, correct PIN, CTM awake, both sides unlocked, send Lock | Single pulse fires, van locks, HA shows LOCKED | PASS |
| 3 | HA, wrong PIN, send Lock | Command rejected at cluster level, no pulse fires | PASS |
| 4 | CTM asleep, both sides locked, send Unlock | 2-pulse sequence fires (wake + act), van unlocks | PASS |
| 5 | Partial state (driver SOLID, PAX OFF), send Lock | 2-pulse sync sequence fires, van fully locks | PASS |
| 6 | Partial state (driver OFF, PAX SOLID), send Lock | Single pulse fires, van fully locks | PASS |
| 7 | Slider open, send Lock | Command rejected, no pulse fires, "No Response" in Apple Home | PASS |
| 8 | Driver open, send Lock | Command rejected, no pulse fires, "No Response" in Apple Home | PASS |
| 9 | Power cycle with NVS persisted state | Boot reports last-known state, classifier confirms within window | PASS |
| 10 | CTM wake from sleep, blackout window | No false state pushes during 1500ms window | PASS |
| 11 | Rapid double Lock taps | Second tap rejected by 2s command cooldown | PASS |

These tests cover the primary functional and edge-case behavior.
They do not cover every possible state combination; partial-state
testing in particular has more permutations than were exercised.
Real-world usage will surface scenarios not in this matrix; bug
reports with serial logs are welcomed via
[GitHub issues](../../../issues/new).

## Known limitations

**Single CTM tested.** The firmware was developed and validated
against a single 2005 Dodge Sprinter 2500. Other model years,
trims, and regional variants may have different CTM behavior.
Compatibility reports from other configurations are welcomed.

**CTM sleep with door state changes.** While the CTM is asleep,
the LEDs are dark and the firmware cannot observe state changes.
A door opening or closing during sleep is invisible until the
CTM wakes. The persisted state holds the last-known value across
this window. Lock commands during this window may proceed past
the door open check if the door was opened during sleep.

**Intermittent classifier wedge after reboot with doors held open.**
If the van is rebooted while one or more doors have been
held open for several minutes, the classifier can read not-BLINK
for ~10 seconds after the CTM wakes despite the LEDs visibly
blinking, and commit a false CLOSED. It self-heals on the next
physical door event, and the hardware path is otherwise sound
(the same setup classifies correctly after recovery). Tracked in
a code TODO (observed 2026-05-31); a fix is pending. Serial logs
from any reproduction are welcome via
[GitHub issues](../../../issues/new).

**Post-pulse verification not implemented.** The firmware does
not currently verify that a pulse actually achieved the
commanded state. The observation push catches divergence, but a
true post-pulse verification module that explicitly waits for the
expected state would be more robust. See
`lock_pulse_register_complete_cb` for the hook point.

**No per-side unlock.** The CTM treats the master lock as a
group; there is no per-side actuation via the WT_YL line.
Removing the per-side pulse wire (in favor of using that GPIO
for CTM state detection) was a deliberate v1.0 tradeoff. Users
cannot unlock only the cargo doors while the slider is open;
they must close all doors, unlock the van, then open whichever
doors they need.

**Unlock with door open fires a pulse.** Because the Unlock
handler has no door open check, an Unlock command with a door
open will fire a pulse. The user-facing impact is bounded: the
ecosystem UI typically does not present Unlock as available in
this state, so the scenario is rare in practice. Future versions
may add a symmetric check.

**Manual interior lock/unlock may be invisible.** Locking or
unlocking a single door via the interior handle latch is
reflected only while the CTM is awake and driving the MLS LEDs;
for instance, right after a door cycle. When the CTM is asleep
(the van has been idle long enough to sleep, even if occupied),
the LEDs are dark and the firmware cannot observe the change; the
ecosystem is not updated until the next observable lock event.

## Related documentation

- [README](../README.md) — project overview and quick start
- [HARDWARE.md](HARDWARE.md) — PCB design, BOM, and ordering
- [ENCLOSURE.md](ENCLOSURE.md) — 3D printed enclosure
- [BUILDING.md](BUILDING.md) — building and flashing the firmware
- [INSTALL.md](INSTALL.md) — commissioning and van installation
- [SAFETY.md](SAFETY.md) — electrical and operational safety
- [CERTIFICATION.md](CERTIFICATION.md) — Matter/Thread certification status
- [CONTRIBUTING.md](CONTRIBUTING.md) — how to contribute
- [LICENSING.md](LICENSING.md) — license terms