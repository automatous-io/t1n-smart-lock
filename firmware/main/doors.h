//
// Copyright 2025-2026 AUTOMATOUS.IO
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

// doors.h
//
// Door state detection via PC817 optocouplers monitoring the two LEDs
// on the center console master door lock switch. The driver LED
// tracks the driver door; the PAX LED tracks the front passenger
// door, sliding cargo door, and rear cargo doors (all three wired
// to a single LED signal on the T1N). PAX is borrowed from
// aviation usage (passengers, undifferentiated).
//
// Hardware:
//   GPIO20: driver LED sense, via U1 (PC817).
//   GPIO18: PAX LED sense, via U2 (PC817).
//
// ----- LED state encoding (per door) -----
// Each LED encodes its door's state through one of four observable
// patterns:
//
//   Solid on:        door closed AND locked
//   Off (CTM awake): door closed AND unlocked
//   Off (CTM asleep): undetermined; sampling is suspended so the
//                    classifier holds its last value from before sleep
//   Blinking:        door open
//
// Blinking requires temporal observation. A single GPIO read cannot
// distinguish solid-on from the high phase of a blink cycle.
// Detection uses a 1500ms sampling window with edge counting.
//
// ----- Lock state model -----
//
// Binary: Locked or Unlocked. The lock reports Locked only when both
// driver and PAX last_stable_locked values are 1. Anything else
// reports Unlocked. At first boot, before either side has committed
// a stable reading, the state is Unknown and no Matter push happens
// until a real value is available.
//
// Manual single-door locks via the interior door handle produce no
// LED indication on this van. Such events are invisible to firmware
// until the next observable lock event (MLS press, key fob, or a
// pulse from this module).
//
// ----- Door state model -----
//
// Binary: Open or Closed. Derived from the classifier output where
// BLINK means open and SOLID/OFF means closed. State transitions
// go through the same shape of stability gate
// (DOOR_STATE_STABILITY_MS, 10 seconds) and same NVS persistence
// pattern as lock state.
//
// The gate filters out the CTM's pre-sleep LED fade-down window
// the same way it does for lock state. Without it, opening a door
// and walking away (so the CTM eventually sleeps) would push a
// false CLOSED event as the LED fades from BLINK down to OFF, and
// Apple Home / HA would show the door as closed for as long as
// the CTM remained asleep.
//
// Cost: legitimate door open/close events lag in Apple Home / HA
// by up to DOOR_STATE_STABILITY_MS. Acceptable tradeoff because
// users are typically not watching the app at the moment they
// open or close a door.
//
// Limitation: a door opened or closed while the CTM is asleep is
// invisible to the firmware until the next CTM wake. The LEDs are
// powered by the CTM and go dark during sleep, so there's nothing
// to observe. The firmware reports the value last seen before
// sleep until the next wake.
//
// ----- NVS persistence -----
//
// Both lock state and door-open state are persisted to NVS, per
// door. Writes go through their respective stability gates
// (NVS_STABILITY_MS for lock, DOOR_STATE_STABILITY_MS for door;
// both 10 seconds): a classifier transition must hold continuously
// before being committed to the in-RAM last_stable_* field and
// written to NVS. The gates filter out transient LED readings
// during the CTM's pre-sleep fade-down window (observed ~8
// seconds between LED dim and the CTM officially sleeping).
// Without the gates, the classifier would write Unlocked / Closed
// to NVS just before sleep and the firmware would report those
// wrong states indefinitely until the next CTM wake re-observed
// reality.
//
// NVS persistence lets the firmware report last-known state at
// boot when the CTM is still asleep. After CTM wake and the next
// gate commit, persisted values are verified or updated against
// fresh observation: if reality matches the persisted value, no
// transition runs and Matter sees the loaded value immediately
// after the classifier window settles; if reality differs, the
// gate runs and the new value commits after 10 seconds.
//
// NVS writes are bounded: lock state changes maybe 2-10 times per
// day in typical van use; door state maybe 20-40 times per day
// from in-and-out events. Combined writes stay well under NVS
// wear limits over a 10+ year lifetime.
//
// ----- Sampling lifecycle -----
//
// The sampling task is suspended when CTM is asleep (LEDs are dark,
// no useful info to gather). It resumes when CTM transitions awake,
// after a 1500ms blackout window to let LED transition noise settle.
//
// The module subscribes to ctm_state_register_callback() for
// wake/sleep transitions. On CTM wake the LED history is cleared
// (stale data from before sleep), the blackout window starts, and
// the sampling task is unblocked.
//
// ----- Blackout windows -----
//
// LED-affecting events generate transient noise that the classifier
// would interpret as fake state changes. All blackouts use the same
// 1500ms window (one full classifier window):
//
//   - CTM asleep -> awake: LEDs power on with transient activity.
//   - CTM awake -> asleep: LEDs power off with transient activity.
//     NVS already has the right value from continuous writes.
//   - Boot: first sampling window after init; classifier hasn't
//     accumulated history yet.
//   - Post-pulse: doors_start_blackout() is exported for this use
//     but nothing currently calls it. A future lock_attempt module
//     will call it after a pulse sequence completes.
//
// During blackout, the classifier still runs internally (accumulating
// history) but no Matter pushes occur. After blackout, settled state
// is pushed once.

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Lock state as reported to Matter. Values 0-3 match the upstream
// Matter DlLockState enum so they can be cast directly for
// DoorLockServer::SetLockState. This module only ever returns
// LOCKED, UNLOCKED, or UNKNOWN; NOT_FULLY_LOCKED and UNLATCHED are
// defined for enum completeness but are not produced by this code.
typedef enum {
    DOORS_LOCK_STATE_NOT_FULLY_LOCKED = 0,
    DOORS_LOCK_STATE_LOCKED           = 1,
    DOORS_LOCK_STATE_UNLOCKED         = 2,
    DOORS_LOCK_STATE_UNLATCHED        = 3,
    DOORS_LOCK_STATE_UNKNOWN          = 0xFF,
} doors_lock_state_t;

// Configures GPIO18 and GPIO20 as inputs with pull-ups, loads
// last-known state from NVS, registers a callback with ctm_state,
// and spawns the sampling task (starts suspended; resumes when CTM
// transitions awake). Must be called AFTER nvs_flash_init() and
// ctm_state_init(). Returns ESP_OK on success.
esp_err_t doors_init(void);

// Returns true if the driver door's last committed stable state is
// OPEN. Reads from the stability-gated value (last_stable_open),
// not the live classifier. Returns false when that value is OPEN's
// opposite or uninitialized: CLOSED, or before any stable reading
// has been committed (first boot with no NVS history). During the
// CTM-asleep window the last committed value is held, so this
// reports the door's state as last observed before sleep.
bool doors_driver_is_open(void);

// Same semantics as doors_driver_is_open(), for the PAX side.
bool doors_pax_is_open(void);

// Returns true if the driver LED classifier currently reads BLINK
// (door physically open). Unlike doors_driver_is_open, this reads
// the live classifier output rather than the gated stable value,
// so it tracks reality within one classifier window (~1.5 seconds)
// rather than the 10-second stability gate. Intended for low-
// latency command pre-checks where a freshly-closed door should
// not be rejected for the full gate window. Returns false when
// the classifier reads SOLID, OFF, or UNKNOWN.
bool doors_driver_classifier_is_blink(void);

// Same semantics as doors_driver_classifier_is_blink(), PAX side.
bool doors_pax_classifier_is_blink(void);

// Returns true when the current LED classifier state requires an
// extra sync pulse before the Lock command can actually lock the
// van. The Sprinter CTM treats the PAX-group lock state as
// authoritative: a single lock pulse from partial state syncs the
// driver side to match PAX, not the other way around.
//
// Concretely:
//   driver SOLID + PAX OFF   -> 1 pulse would UNLOCK both (CTM
//                               syncs driver down to PAX state).
//                               This function returns TRUE so the
//                               command emits 2 pulses: pulse 1
//                               unlocks both, pulse 2 locks both.
//   driver OFF + PAX SOLID   -> 1 pulse correctly LOCKS both (CTM
//                               syncs driver up to PAX state).
//                               This function returns FALSE so the
//                               command uses the standard single
//                               pulse path.
//
// Returns false in all non-partial states (both SOLID, both OFF)
// and conservative defaults (BLINK or UNKNOWN on either side).
bool doors_lock_state_needs_sync_pulse(void);

// Returns true if the driver door's last committed stable state is
// LOCKED. Reads from the stability-gated value (last_stable_locked),
// not the live classifier. Returns false before any stable reading
// has been committed (first boot only).
bool doors_driver_is_locked(void);

// Same semantics as doors_driver_is_locked(), for the PAX side.
bool doors_pax_is_locked(void);

// Returns true when both doors_driver_is_locked() and
// doors_pax_is_locked() are true. Reads the gated stable values, so
// changes only after the stability gate commits.
bool doors_vehicle_is_fully_locked(void);

// Returns the binary lock state computed from both sides'
// last_stable_locked values: LOCKED when both are 1, UNLOCKED
// otherwise. Returns UNKNOWN only at first boot when at least one
// side has not yet committed a stable reading. Once both sides have
// committed, UNKNOWN can never reappear.
doors_lock_state_t doors_compute_lock_state(void);

// Updates the doors module's tracking of what lock state was last
// pushed to Matter. Call from any code path that writes the Matter
// LockState attribute through a mechanism other than
// push_state_if_changed (for example, BoltLockMgr's optimistic
// update on Lock/Unlock commands). Keeping the tracker in sync with
// external writes prevents spurious pushes when observation agrees
// with the external write, and lets a correction push fire when
// observation later disagrees.
void doors_set_pushed_lock_state(doors_lock_state_t state);

// Suppresses Matter pushes for LED_BLACKOUT_MS (1500ms). Currently
// exported but not called from anywhere; reserved for a future
// lock_attempt module which will trigger blackout after each pulse
// sequence. Multiple sources sharing a single blackout-until
// timestamp is intentional: the latest call extends the window.
void doors_start_blackout(void);

#ifdef __cplusplus
}
#endif