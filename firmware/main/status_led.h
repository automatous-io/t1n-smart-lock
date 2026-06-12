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

// status_led.h
//
// Visual status indicator on the XIAO ESP32-C6 onboard LED (GPIO15, active LOW).
// Patterns indicate Matter commissioning and Thread network status.
//
//   OFF                Pre-init or unused.
//   BLE_ADVERTISING    200ms/200ms. Uncommissioned, advertising for pairing.
//   THREAD_CONNECTING  500ms on, 1500ms off. Commissioned, attaching to Thread.
//   THREAD_CONNECTED   Solid. Operational.
//   FAULT              100ms/100ms. Reserved, currently unused.
//   IDENTIFY           100ms on, 900ms off. Matter Identify cluster active.
//
// IDENTIFY saves the prior state on entry and restores it on exit, so it
// works correctly regardless of what state preceded it.
//
// Implementation: dedicated FreeRTOS task at 100ms tick. State changes
// take effect on the next tick (up to 100ms latency). All public
// functions are thread-safe.

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_STATE_OFF,
    LED_STATE_BLE_ADVERTISING,
    LED_STATE_THREAD_CONNECTING,
    LED_STATE_THREAD_CONNECTED,
    LED_STATE_FAULT,
    LED_STATE_IDENTIFY,
} led_state_t;

// Configure GPIO15, spawn the LED task. Call early in app_main so the
// LED can indicate progress during the rest of boot.
esp_err_t status_led_init(void);

// Change the LED state. No-op if already at this state.
void status_led_set_state(led_state_t state);

// Enter Identify mode, saving the current state for restoration.
// Called from app_identification_cb on Matter Identify START.
// Repeated calls while active extend the duration without losing the
// original saved state.
void status_led_start_identify(void);

// Exit Identify mode, restoring the state saved by start_identify.
// Called from app_identification_cb on Matter Identify STOP.
void status_led_stop_identify(void);

#ifdef __cplusplus
}
#endif