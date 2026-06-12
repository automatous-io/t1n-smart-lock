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

// lock_pulse.h
//
// Drives GPIO1 to pulse the T1N master lock line (WT_YL). Each pulse
// triggers the CTM's lock/unlock function. Same pulse for both lock
// and unlock; the CTM toggles state.
//
// lock_pulse_init() drives GPIO1 LOW and must run before
// esp_matter::start() so the line is in a known-safe state before
// any other subsystem can touch it.
//
// A pulse sequence is either:
//   1 pulse  (CTM awake): one 500ms pulse, done.
//   2 pulses (CTM asleep): wake pulse, 500ms gap, action pulse.
//
// Cooldown: 300ms minimum between sequences. The intra-sequence
// 500ms gap is fixed and separate from the cooldown.
//
// lock_pulse_request() enqueues a sequence and returns immediately;
// all GPIO timing happens on lock_pulse_task. Optionally callers can
// register a completion callback that fires when a sequence finishes,
// used for post-pulse LED state verification.

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOCK_PULSE_RESULT_ENQUEUED,
    LOCK_PULSE_RESULT_SUPPRESSED_COOLDOWN,  // <300ms since last sequence
    LOCK_PULSE_RESULT_SUPPRESSED_BUSY,      // sequence still running
    LOCK_PULSE_RESULT_INVALID_COUNT,        // count not 1 or 2
} lock_pulse_result_t;

// Fires from lock_pulse_task after the final pulse in a sequence
// completes, before the 300ms cooldown closes. MUST NOT BLOCK.
// Defer real work via PlatformMgr::ScheduleWork() or another task.
typedef void (*lock_pulse_complete_cb_t)(void);

// Must run before esp_matter::start().
esp_err_t lock_pulse_init(void);

// count must be 1 (CTM awake) or 2 (CTM asleep). Non-blocking;
// returns within microseconds. Safe to call from any task.
lock_pulse_result_t lock_pulse_request(int count);

// NULL to clear. Only one callback at a time; replaces previous.
// Safe to call from any task.
void lock_pulse_register_complete_cb(lock_pulse_complete_cb_t cb);

#ifdef __cplusplus
}
#endif