//
// Copyright 2025-2026 AUTOMATOUS.IO
// Portions derived from Espressif esp-matter examples,
// originally released into the public domain / CC0.
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

// door_lock.cpp
//
// Matter Door Lock cluster glue. Receives Matter lock/unlock commands,
// reads CTM sleep state, requests pulse sequences from lock_pulse, and
// delegates credential/user/schedule storage to BoltLockMgr (Espressif
// example boilerplate in door_lock_manager.cpp).
//
// GPIO ownership lives in dedicated modules:
//   GPIO1  (lock pulse output)                   -> lock_pulse.cpp
//   GPIO2  (CTM sleep detect)                    -> ctm_state.cpp
//   GPIO18 (master door lock switch pax LED)     -> doors.cpp
//   GPIO20 (master door lock switch driver LED)  -> doors.cpp
//
// This file owns no hardware directly.
//
// lock_pulse_request() is non-blocking, so the Matter callback returns
// to Apple Home / HA optimistically (well under 1ms). The actual GPIO
// pulse fires asynchronously on lock_pulse_task. State verification of
// the result is currently not implemented (see TODO below).
//
// Two layers of cooldown protect the hardware:
//   - COMMAND_COOLDOWN_MS (2000ms): debounces user intent at the
//     Matter cluster level (mashed buttons, controller retries,
//     double-tap behaviors).
//   - 300ms in lock_pulse.cpp: hardware-level minimum spacing.
//
// TODO: post-pulse verification. lock_pulse already exposes a
// completion callback hook (lock_pulse_register_complete_cb). A
// future lock_attempt module would snapshot pre-pulse vehicle state,
// wait for the completion callback plus blackout window, read the
// post-pulse state from doors.cpp, and update the Matter lock state
// attribute to the actual result instead of optimistically.

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "door_lock_manager.h"
#include <lib/core/DataModelTypes.h>
#include <app/clusters/door-lock-server/door-lock-server.h>
#include "lock_pulse.h"
#include "ctm_state.h"
#include "doors.h"

#include <atomic>

static const char *TAG = "door_lock";

// Command-level cooldown. Minimum interval between consecutive
// Lock/Unlock commands at the Matter cluster level. This is the
// user-intent debounce layer; hardware protection lives in
// lock_pulse.cpp.
#define COMMAND_COOLDOWN_MS  2000

// Last command timestamp (microseconds since boot). Shared between
// Lock and Unlock: any command suppresses subsequent commands of
// either type during the cooldown window.
//
// Initialized to a value safely in the past so the first command
// after boot is never blocked by cooldown. Cannot use INT64_MIN
// because (now_us - INT64_MIN) overflows.
static std::atomic<int64_t> s_last_command_end_us{
    -(int64_t)COMMAND_COOLDOWN_MS * 1000 * 10};

static bool command_cooldown_check(const char *cmd_name)
{
    int64_t now_us = esp_timer_get_time();
    int64_t last_end_us = s_last_command_end_us.load();
    int64_t since_last_us = now_us - last_end_us;

    if (since_last_us < (int64_t)COMMAND_COOLDOWN_MS * 1000) {
        ESP_LOGW(TAG, "%s command rejected: cooldown active "
                 "(%lldms since last, need %dms).",
                 cmd_name, since_last_us / 1000, COMMAND_COOLDOWN_MS);
        return false;
    }
    return true;
}

static void command_cooldown_mark_complete(void)
{
    s_last_command_end_us.store(esp_timer_get_time());
}

// Request a pulse sequence sized for the current CTM state and
// lock-state asymmetry. See the comment in the function body for the
// pulse-count rules. Returns immediately; pulses fire on
// lock_pulse_task.
static void request_pulses_for_ctm_state(void)
{
    // Pulse count depends on CTM state and current lock-state asymmetry:
    //
    //   CTM asleep                       -> 2 pulses (pulse 1 wakes
    //                                       the CTM, pulse 2 toggles
    //                                       the lock)
    //   CTM awake, driver LOCKED,
    //     PAX UNLOCKED                   -> 2 pulses (pulse 1 syncs
    //                                       both to unlocked, pulse 2
    //                                       locks both)
    //   CTM awake, all other states      -> 1 pulse  (standard toggle)
    //
    // Hardware-characterized rule for the Sprinter CTM: from partial
    // state, a single lock pulse syncs the driver side to match PAX.
    // This is great when PAX is locked (driver follows up to LOCKED)
    // but wrong when PAX is unlocked (driver follows down to UNLOCKED,
    // opposite of user intent). Only the second case needs the extra
    // sync pulse first. See doors_lock_state_needs_sync_pulse for the
    // detection.
    //
    // Live classifier readings are used so the decision matches
    // current physical reality rather than the 10-second-lagged
    // stability gate. The user typically taps Lock right after an
    // exit pattern that produced the partial state, so the live
    // classifier is the right signal.
    bool ctm_asleep = !ctm_state_is_awake();
    bool needs_sync = doors_lock_state_needs_sync_pulse();
    int pulse_count = (ctm_asleep || needs_sync) ? 2 : 1;

    if (ctm_asleep) {
        ESP_LOGI(TAG, "CTM is asleep. Requesting 2-pulse wake+lock sequence.");
    } else if (needs_sync) {
        ESP_LOGI(TAG, "Driver locked, PAX unlocked. Requesting 2-pulse sync+lock sequence.");
    } else {
        ESP_LOGI(TAG, "CTM is awake. Requesting single lock pulse.");
    }

    lock_pulse_result_t result = lock_pulse_request(pulse_count);
    switch (result) {
        case LOCK_PULSE_RESULT_ENQUEUED:
            // Expected path; pulses will fire asynchronously.
            break;
        case LOCK_PULSE_RESULT_SUPPRESSED_COOLDOWN:
            ESP_LOGW(TAG, "Lock pulse cooldown active. Pulse not fired.");
            break;
        case LOCK_PULSE_RESULT_SUPPRESSED_BUSY:
            ESP_LOGW(TAG, "Lock pulse busy. Pulse not fired.");
            break;
        case LOCK_PULSE_RESULT_INVALID_COUNT:
            ESP_LOGE(TAG, "Lock pulse rejected count. This indicates a logic error.");
            break;
    }
    // Matter reports success regardless. See TODO at top of file
    // for post-pulse verification work.
}

// ---------------------------------------------------------------------------
// Linker anchor
// ---------------------------------------------------------------------------

// Empty function body. The Matter Door Lock cluster callbacks below
// are referenced by esp_matter via weak symbols; without at least
// one symbol in this translation unit being called directly from
// app_main, the linker would dead-strip the entire .obj. app_main
// calls door_lock_init() to keep this file in the binary.
extern "C" void door_lock_init(void)
{
}

// ---------------------------------------------------------------------------
// Cluster init
// ---------------------------------------------------------------------------

// Initialize lock server and lock state.
//
// PIN behavior:
//   Apple Home uses fabric identity, no PIN required.
//   TODO: confirm Home Assistant PIN requirement. Earlier observation
//   suggests HA won't show the lock without a PIN configured.
void emberAfDoorLockClusterInitCallback(EndpointId endpoint)
{
    DoorLockServer::Instance().InitServer(endpoint);
    BoltLockMgr().InitLockState();
    ESP_LOGI(TAG, "Door lock cluster initialized.");
}

// ---------------------------------------------------------------------------
// Lock command
// ---------------------------------------------------------------------------

bool emberAfPluginDoorLockOnDoorLockCommand(chip::EndpointId endpointId,
                                            const Nullable<chip::FabricIndex> &fabricIdx,
                                            const Nullable<chip::NodeId> &nodeId,
                                            const Optional<ByteSpan> &pinCode,
                                            OperationErrorEnum &err)
{
    ESP_LOGI(TAG, "Lock command received on endpoint %d.", endpointId);

    if (!command_cooldown_check("Lock")) {
        err = OperationErrorEnum::kUnspecified;
        return false;
    }

    // Reject Lock commands when a door is observed open. The CTM
    // physically cannot engage the lock on an open door, and
    // without this check Apple Home would briefly show LOCKED
    // (BoltLockMgr's optimistic write) before correcting back to
    // UNLOCKED when the quiet window expires and the gate observes
    // the divergence. That delayed correction is bad UX in van-
    // life scenarios where users load the van, tap Lock, and walk
    // away before the correction fires. Rejecting here gives
    // immediate "lock failed" feedback while the user is still at
    // the van and can close the offending door.
    //
    // Uses the live classifier accessors rather than the gated
    // last_stable_open, so a door that was just closed is accepted
    // within ~1.5 seconds instead of the full 10-second stability
    // gate. The inverse miss (door just opened, classifier hasn't
    // updated yet) falls through to the quiet window in doors.cpp,
    // which catches the divergence and corrects Apple Home to
    // UNLOCKED after the window expires.
    //
    // Unlock has no equivalent pre-check: unlocking with a door
    // open is harmless and the user may legitimately want to
    // ensure the lock is disengaged regardless of door position.
    if (doors_driver_classifier_is_blink() || doors_pax_classifier_is_blink()) {
        ESP_LOGW(TAG, "Lock command rejected: a door is open.");
        err = OperationErrorEnum::kUnspecified;
        return false;
    }

    // Enqueue pulse sequence (non-blocking, returns immediately).
    request_pulses_for_ctm_state();

    // Update BoltLockMgr in-memory state. Doesn't fire hardware; this
    // updates the cluster's internal state which becomes the reported
    // attribute value.
    bool result = BoltLockMgr().Lock(endpointId, pinCode, err);

    // BoltLockMgr's setLockState writes the Matter LockState
    // attribute directly via DoorLockServer::SetLockState, bypassing
    // the doors module's push tracking. Sync the tracker so the next
    // push_state_if_changed compares against the actual attribute
    // value: no spurious push fires when observation agrees, and a
    // correction push fires when observation disagrees. The latter
    // is what restores honesty if, for example, a door was open and
    // the pulse couldn't fully engage the lock.
    if (result) {
        doors_set_pushed_lock_state(DOORS_LOCK_STATE_LOCKED);
    }

    command_cooldown_mark_complete();
    return result;
}

// ---------------------------------------------------------------------------
// Unlock command
// ---------------------------------------------------------------------------

bool emberAfPluginDoorLockOnDoorUnlockCommand(chip::EndpointId endpointId,
                                              const Nullable<chip::FabricIndex> &fabricIdx,
                                              const Nullable<chip::NodeId> &nodeId,
                                              const Optional<ByteSpan> &pinCode,
                                              OperationErrorEnum &err)
{
    ESP_LOGI(TAG, "Unlock command received on endpoint %d.", endpointId);

    if (!command_cooldown_check("Unlock")) {
        err = OperationErrorEnum::kUnspecified;
        return false;
    }

    // Enqueue pulse sequence (non-blocking, returns immediately).
    request_pulses_for_ctm_state();

    // Update BoltLockMgr in-memory state.
    bool result = BoltLockMgr().Unlock(endpointId, pinCode, err);

    // Sync the doors push tracker with BoltLockMgr's optimistic
    // write. See the matching comment in the Lock command handler
    // above.
    if (result) {
        doors_set_pushed_lock_state(DOORS_LOCK_STATE_UNLOCKED);
    }

    command_cooldown_mark_complete();
    return result;
}

// ---------------------------------------------------------------------------
// Credential / user / schedule passthrough (unchanged from example)
// ---------------------------------------------------------------------------

bool emberAfPluginDoorLockGetCredential(chip::EndpointId endpointId, uint16_t credentialIndex,
                                        CredentialTypeEnum credentialType,
                                        EmberAfPluginDoorLockCredentialInfo &credential)
{
    return BoltLockMgr().GetCredential(endpointId, credentialIndex, credentialType, credential);
}

bool emberAfPluginDoorLockSetCredential(chip::EndpointId endpointId, uint16_t credentialIndex,
                                        chip::FabricIndex creator, chip::FabricIndex modifier,
                                        DlCredentialStatus credentialStatus,
                                        CredentialTypeEnum credentialType,
                                        const chip::ByteSpan &credentialData)
{
    return BoltLockMgr().SetCredential(endpointId, credentialIndex, creator, modifier,
                                       credentialStatus, credentialType, credentialData);
}

bool emberAfPluginDoorLockGetUser(chip::EndpointId endpointId, uint16_t userIndex,
                                  EmberAfPluginDoorLockUserInfo &user)
{
    return BoltLockMgr().GetUser(endpointId, userIndex, user);
}

bool emberAfPluginDoorLockSetUser(chip::EndpointId endpointId, uint16_t userIndex,
                                  chip::FabricIndex creator, chip::FabricIndex modifier,
                                  const chip::CharSpan &userName, uint32_t uniqueId,
                                  UserStatusEnum userStatus, UserTypeEnum usertype,
                                  CredentialRuleEnum credentialRule,
                                  const CredentialStruct *credentials, size_t totalCredentials)
{
    return BoltLockMgr().SetUser(endpointId, userIndex, creator, modifier, userName, uniqueId,
                                 userStatus, usertype, credentialRule, credentials, totalCredentials);
}

DlStatus emberAfPluginDoorLockGetSchedule(chip::EndpointId endpointId, uint8_t weekdayIndex,
                                          uint16_t userIndex,
                                          EmberAfPluginDoorLockWeekDaySchedule &schedule)
{
    return BoltLockMgr().GetWeekdaySchedule(endpointId, weekdayIndex, userIndex, schedule);
}

DlStatus emberAfPluginDoorLockGetSchedule(chip::EndpointId endpointId, uint8_t yearDayIndex,
                                          uint16_t userIndex,
                                          EmberAfPluginDoorLockYearDaySchedule &schedule)
{
    return BoltLockMgr().GetYeardaySchedule(endpointId, yearDayIndex, userIndex, schedule);
}

DlStatus emberAfPluginDoorLockGetSchedule(chip::EndpointId endpointId, uint8_t holidayIndex,
                                          EmberAfPluginDoorLockHolidaySchedule &holidaySchedule)
{
    return BoltLockMgr().GetHolidaySchedule(endpointId, holidayIndex, holidaySchedule);
}

DlStatus emberAfPluginDoorLockSetSchedule(chip::EndpointId endpointId, uint8_t weekdayIndex,
                                          uint16_t userIndex, DlScheduleStatus status,
                                          DaysMaskMap daysMask, uint8_t startHour,
                                          uint8_t startMinute, uint8_t endHour, uint8_t endMinute)
{
    return BoltLockMgr().SetWeekdaySchedule(endpointId, weekdayIndex, userIndex, status,
                                            daysMask, startHour, startMinute, endHour, endMinute);
}

DlStatus emberAfPluginDoorLockSetSchedule(chip::EndpointId endpointId, uint8_t yearDayIndex,
                                          uint16_t userIndex, DlScheduleStatus status,
                                          uint32_t localStartTime, uint32_t localEndTime)
{
    return BoltLockMgr().SetYeardaySchedule(endpointId, yearDayIndex, userIndex, status,
                                            localStartTime, localEndTime);
}

DlStatus emberAfPluginDoorLockSetSchedule(chip::EndpointId endpointId, uint8_t holidayIndex,
                                          DlScheduleStatus status, uint32_t localStartTime,
                                          uint32_t localEndTime, OperatingModeEnum operatingMode)
{
    return BoltLockMgr().SetHolidaySchedule(endpointId, holidayIndex, status, localStartTime,
                                            localEndTime, operatingMode);
}

void emberAfPluginDoorLockOnAutoRelock(chip::EndpointId endpointId)
{
    ESP_LOGI(TAG, "Auto-relock triggered on endpoint %d.", endpointId);
}