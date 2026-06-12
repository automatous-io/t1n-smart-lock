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

// ctm_state.h
//
// CTM (Mercedes Sprinter T1N Central Timer Module) sleep state detection.
//
// SIGNAL MODEL (validated 2026-05-24):
// The CTM signals its power state via a frequency-modulated pulse train
// on the WT_RD wire. Frequency encodes state:
//   - Asleep: ~47 Hz (~238 transitions per 5 seconds)
//   - Awake:  ~89 Hz (~448 transitions per 5 seconds)
//
// An LM393 comparator on the PCB converts the analog pulse train to
// clean digital edges on GPIO2. This module counts those edges in a
// 5-second sliding window and compares to a threshold (350) to
// determine state. A 5-second sticky classifier prevents transient
// noise from causing false state flips.
//
// Detection has ~100 transitions of margin in both directions. Lock
// pulse fires generate a brief WT_RD activity spike (~800-865
// transitions per 5s for ~3-4 seconds) but this does not cause false
// transitions because the spike goes deeper into the awake region.
//
// HARDWARE:
//   GPIO2 is the LM393 comparator output. Edges occur continuously in
//   both states; reading instantaneous GPIO level is meaningless.
//   Internal pull-up is required because LM393 is open-collector.
//
// THREADING:
//   - ISR runs on edge from GPIO2, appends timestamp to ring buffer.
//     IRAM-attributed, no logging.
//   - Dedicated FreeRTOS task `ctm_monitor` ticks every 1000ms, counts
//     edges in the last 5 seconds, applies threshold + sticky window,
//     updates atomic state.
//   - State callbacks (if registered) fire from the ctm_monitor task
//     context. Callbacks MUST NOT block.
//   - ctm_state_is_awake() is safe to call from any task at any time.
//
// BOOT BEHAVIOR:
//   Initial state = ASLEEP (safe default for door_lock; uses longer
//   double-pulse sequence until proven awake). After 5+ seconds of
//   observation, official state reflects reality.

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// State change callback. Fires from ctm_monitor task context when the
// official (sticky-filtered) state changes. The new state is passed as
// the argument: true = awake, false = asleep.
//
// MUST NOT BLOCK. Implementations should set a flag or post to a queue
// and return immediately. Blocking the ctm_monitor task delays state
// detection for the entire system.
typedef void (*ctm_state_changed_cb_t)(bool now_awake);

// Initialize the CTM state detector.
//
// Configures GPIO2 as input with internal pull-up, attaches ISR for
// edge counting, spawns the ctm_monitor task. Must be called once at
// startup before any other ctm_state_* function.
//
// Returns ESP_OK on success, error code otherwise. On failure, callers
// should treat all subsequent ctm_state_is_awake() calls as returning
// false (asleep; safe default).
esp_err_t ctm_state_init(void);

// Returns true if the CTM is currently believed to be awake.
//
// "Currently believed" means the sticky-filtered official state, which
// requires 5 seconds of consistent raw state before changing. This
// makes the state robust against transients but adds up to 5 seconds
// of detection latency for genuine state changes.
//
// Safe to call from any task. Returns false (asleep) before init or on
// init failure; this is a conservative default that causes door_lock
// to use the longer double-pulse sequence, which works regardless of
// actual CTM state.
bool ctm_state_is_awake(void);

// Register a callback to be invoked when the CTM state changes.
//
// Only one callback can be registered at a time. Calling this again
// replaces the previous callback. Pass NULL to clear.
//
// Callback fires from ctm_monitor task context; MUST NOT BLOCK.
void ctm_state_register_callback(ctm_state_changed_cb_t cb);

#ifdef __cplusplus
}
#endif