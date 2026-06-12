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

// doors.cpp
//
// LED state classifier + NVS-backed lock state + Matter integration.
// See doors.h for the full architecture documentation.

#include "doors.h"
#include "ctm_state.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <esp_matter.h>
#include <app/clusters/door-lock-server/door-lock-server.h>

#include <atomic>
#include <optional>
#include <string.h>

static const char *TAG = "doors";

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------

// PC817 optocoupler outputs. Internal pull-up required since PC817
// is open-collector. GPIO HIGH = LED off, GPIO LOW = LED on.
// (Logical inversion handled in led_read_logical_on() below.)
#define DRIVER_LED_GPIO  GPIO_NUM_20
#define PAX_LED_GPIO     GPIO_NUM_18

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

// Sampling task tick interval. Fast enough to catch the high/low phase
// of a 1Hz blink (~500ms half-period) with multiple samples per phase.
static constexpr uint32_t SAMPLE_TICK_MS = 50;

// Rolling history window for blink/solid/off classification. Must be
// longer than one full blink period to guarantee at least one edge
// is visible when the LED is blinking.
static constexpr uint32_t CLASSIFIER_WINDOW_MS = 1500;

// Number of history slots = window / tick.
static constexpr int CLASSIFIER_HISTORY_SIZE =
    (CLASSIFIER_WINDOW_MS / SAMPLE_TICK_MS);  // 30 samples

// Confirmed SOLID requires this much continuous high time in the
// recent window (no LOW samples). Lower values risk false SOLID
// classifications during the high phase of a blink cycle.
static constexpr uint32_t SOLID_CONFIRM_MS = 500;
static constexpr int SOLID_CONFIRM_SAMPLES =
    (SOLID_CONFIRM_MS / SAMPLE_TICK_MS);  // 10 samples

// Confirmed OFF requires this much continuous low time. Shorter than
// SOLID because OFF is the default safe state.
static constexpr uint32_t OFF_CONFIRM_MS = 200;
static constexpr int OFF_CONFIRM_SAMPLES =
    (OFF_CONFIRM_MS / SAMPLE_TICK_MS);  // 4 samples

// Blackout window after any LED-affecting event. Equal to one full
// classifier window, so the classifier has time to fully resettle
// before new state is reported.
//
// TODO: characterize on van. If awake->asleep and asleep->awake
// transitions differ meaningfully, split into separate constants.
static constexpr uint32_t LED_BLACKOUT_MS = 1500;

// NVS stability gate. A classifier state transition (SOLID/OFF) must
// hold for at least this long before being committed to NVS. This
// filters out transient LED readings during the CTM going-to-sleep
// fade-down window (observed ~8 seconds between LED dip and the
// CTM officially flipping to ASLEEP). Without this gate, the
// classifier writes Unlocked to NVS just before sleep, and the
// firmware then reports Unlocked indefinitely until the next CTM
// wake event re-observes the actual LED state.
//
// 10 seconds is well above the observed pre-sleep window and well
// under the CTM's 15-minute auto-sleep timeout, so legitimate lock
// events (which keep the CTM awake) always have time to commit.
static constexpr uint32_t NVS_STABILITY_MS = 10000;

// Door-state stability gate. A classifier transition between BLINK
// (door open) and OFF/SOLID (door closed) must hold for at least
// this long before being committed to last_stable_open. Same shape
// as NVS_STABILITY_MS for lock state, but separately named so it
// can be tuned independently in a future firmware update without
// affecting lock-state commit behavior.
//
// Purpose: filter out the CTM's pre-sleep LED fade-down window
// which would otherwise cause a false "door closed" push when a
// door is open and the CTM transitions to sleep (LED fades,
// classifier reads OFF, push fires, state is wrong until next
// CTM wake).
//
// 10 seconds is well above the observed ~8-second pre-sleep fade
// window. The latency on legitimate door events is acceptable
// because the user isn't usually looking at the app the instant
// they open or close a door. Tighten later if testing shows the
// fade window is consistently shorter.
static constexpr uint32_t DOOR_STATE_STABILITY_MS = 10000;

// Sampling task config.
static constexpr uint32_t TASK_STACK_BYTES = 4096;
static constexpr UBaseType_t TASK_PRIORITY = 5;  // Same as ctm_monitor

// ---------------------------------------------------------------------------
// NVS keys
// ---------------------------------------------------------------------------

#define NVS_NAMESPACE      "doors"
#define NVS_KEY_DRV_LOCKED "drv_lk"  // int8: -1 unknown, 0 unlocked, 1 locked
#define NVS_KEY_PAX_LOCKED "pax_lk"  // int8: -1 unknown, 0 unlocked, 1 locked
#define NVS_KEY_DRV_OPEN   "drv_op"  // int8: -1 unknown, 0 closed,   1 open
#define NVS_KEY_PAX_OPEN   "pax_op"  // int8: -1 unknown, 0 closed,   1 open

// ---------------------------------------------------------------------------
// Classifier types
// ---------------------------------------------------------------------------

typedef enum {
    LED_CLASS_UNKNOWN = 0,  // Classifier hasn't accumulated enough history yet
    LED_CLASS_OFF,          // Confirmed LOW for OFF_CONFIRM_SAMPLES
    LED_CLASS_SOLID,        // Confirmed HIGH for SOLID_CONFIRM_SAMPLES
    LED_CLASS_BLINK,        // Edges seen in CLASSIFIER_WINDOW_MS
} led_class_t;

// Kept for ad-hoc debug logging during development. Marked unused
// to silence the compiler warning when no log lines reference it.
__attribute__((unused))
static const char *led_class_str(led_class_t c)
{
    switch (c) {
        case LED_CLASS_OFF:   return "OFF";
        case LED_CLASS_SOLID: return "SOLID";
        case LED_CLASS_BLINK: return "BLINK";
        default:              return "UNKNOWN";
    }
}

// Per-LED state. Two instances: driver, pax.
struct led_state {
    // Circular buffer of raw samples. 1 = LED on, 0 = LED off.
    // Newest sample is at (history_head - 1); oldest is at
    // history_head (about to be overwritten). The classifier walks
    // backwards from head to traverse newest-to-oldest.
    uint8_t history[CLASSIFIER_HISTORY_SIZE];
    int history_count;  // 0..CLASSIFIER_HISTORY_SIZE (capped at max)
    int history_head;   // Next write index (wraps)

    // Current classifier output.
    led_class_t classifier;

    // Last confirmed stable state for NVS persistence. Updates only
    // on transitions to SOLID or OFF (not BLINK or UNKNOWN). The
    // value of last_stable_locked is what gets written to NVS.
    //
    //   -1 = unknown (no confirmed reading yet, post-boot)
    //    0 = last confirmed state was OFF (unlocked)
    //    1 = last confirmed state was SOLID (locked)
    int8_t last_stable_locked;

    // NVS stability gate state. When the classifier produces a
    // SOLID/OFF value that differs from last_stable_locked, the new
    // value enters a pending state. Only after the pending value has
    // held continuously for NVS_STABILITY_MS does it get committed
    // to last_stable_locked + NVS. If the classifier flips back
    // before the gate expires, the pending state is reset.
    //
    //   -1 = no pending change
    //    0 = pending OFF
    //    1 = pending SOLID
    int8_t pending_stable;
    int64_t pending_since_us;

    // Last confirmed stable door-open state. Mirrors
    // last_stable_locked but for the OPEN <-> CLOSED axis derived
    // from BLINK vs SOLID/OFF classification. Persisted to NVS so
    // a reboot with CTM still asleep reports the last-known door
    // state instead of UNKNOWN.
    //
    //   -1 = unknown (no confirmed reading yet, no NVS history)
    //    0 = last confirmed state was CLOSED (SOLID or OFF)
    //    1 = last confirmed state was OPEN (BLINK)
    int8_t last_stable_open;

    // Door-state stability gate. Mirrors pending_stable but for
    // the OPEN <-> CLOSED axis. Only after the pending value has
    // held continuously for DOOR_STATE_STABILITY_MS does it get
    // committed to last_stable_open + NVS. If the classifier
    // flips back before the gate expires, the pending state is
    // reset.
    //
    //   -1 = no pending change
    //    0 = pending CLOSED
    //    1 = pending OPEN
    int8_t pending_open;
    int64_t pending_open_since_us;
};

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static struct led_state s_driver;
static struct led_state s_pax;

// Sampling task synchronization
static TaskHandle_t s_task_handle = nullptr;
static SemaphoreHandle_t s_wake_sem = nullptr;

// Blackout window: timestamp (us) until which Matter pushes are
// suppressed. doors_init() sets this to (now + LED_BLACKOUT_MS) at
// startup, giving the classifier one full window to settle before
// pushing.
static std::atomic<int64_t> s_blackout_until_us{0};

// CTM awake state. Read by sampling task, written by ctm callback.
static std::atomic<bool> s_ctm_awake{false};

// Most recently pushed Matter values, for change detection.
// Initialized to sentinel values that don't match any real state,
// so the first push after sampling resumes always fires.
//
//   Contact sensors: -1 = never pushed yet, 0 = pushed Closed, 1 = pushed Open
//   Lock state:      0xFE = never pushed yet (DlLockState valid values are 0-3)
//
// Without these sentinels, the default values (false for bool,
// 0 for uint8_t) happen to match common real states ("Closed" for
// contact sensor when both LEDs are SOLID at boot, "NotFullyLocked"
// for lock state). That coincidence would cause the first push to
// be skipped, and Apple Home would stay at the Matter cluster
// default ("Open" for contact sensors, undefined for lock) instead
// of getting the real state.
static std::atomic<int8_t>  s_last_pushed_driver_open{-1};
static std::atomic<int8_t>  s_last_pushed_pax_open{-1};
static std::atomic<uint8_t> s_last_pushed_lock_state{0xFE};

// Suppresses observation-based lock-state pushes for a window after
// each Lock/Unlock command. BoltLockMgr writes the Matter LockState
// attribute optimistically the moment a command is accepted; if we
// pushed observation-based state in the same tick, the gate's still-
// pre-pulse value would overwrite the optimistic write and Apple
// Home would flicker LOCKED -> UNLOCKED -> LOCKED. The window ends
// after enough time for the pulse + classifier settle + 10s
// stability gate to commit a fresh observation. After the window,
// either the next push agrees with the optimistic write (no push
// fires, no flicker), or it disagrees and a correction push fires
// (the bug case: locking with a door open).
//
// Set to 25 seconds: observed gate latency from command receipt to
// stable commit is ~21 seconds (CTM awake, 1 pulse). 2-pulse wake
// sequence adds ~1 second. 25 leaves margin without making the
// correction-push window noticeably long.
static std::atomic<int64_t> s_lock_push_quiet_until_us{0};
#define LOCK_PUSH_QUIET_MS  25000

// Initialization complete flag.
static std::atomic<bool> s_initialized{false};

// Matter endpoint IDs (defined in app_main.cpp).
extern uint16_t door_lock_endpoint_id;
extern uint16_t driver_door_endpoint_id;
extern uint16_t pax_door_endpoint_id;

// ---------------------------------------------------------------------------
// GPIO sampling
// ---------------------------------------------------------------------------

// Read the LED "on" state, accounting for PC817 inversion.
// PC817 pulls GPIO LOW when LED is on (current flows through opto),
// HIGH when LED is off (no current, pull-up wins).
static inline uint8_t led_read_logical_on(gpio_num_t gpio)
{
    return (gpio_get_level(gpio) == 0) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// History buffer management
// ---------------------------------------------------------------------------

static void history_clear(struct led_state *s)
{
    memset(s->history, 0, sizeof(s->history));
    s->history_count = 0;
    s->history_head = 0;
}

static void history_append(struct led_state *s, uint8_t sample)
{
    s->history[s->history_head] = sample;
    s->history_head = (s->history_head + 1) % CLASSIFIER_HISTORY_SIZE;
    if (s->history_count < CLASSIFIER_HISTORY_SIZE) {
        s->history_count++;
    }
}

// ---------------------------------------------------------------------------
// Classifier (derives BLINK / SOLID / OFF from history buffer)
// ---------------------------------------------------------------------------

static led_class_t classify(const struct led_state *s)
{
    if (s->history_count == 0) {
        return LED_CLASS_UNKNOWN;
    }

    // Walk history backwards (newest first), count transitions and
    // tail run lengths.
    int high_count = 0;
    int transitions = 0;
    int newest_run_value = -1;  // -1 = not started
    int newest_run_length = 0;

    int idx = (s->history_head - 1 + CLASSIFIER_HISTORY_SIZE) % CLASSIFIER_HISTORY_SIZE;
    int prev_sample = -1;

    for (int i = 0; i < s->history_count; i++) {
        uint8_t sample = s->history[idx];

        if (sample) high_count++;

        if (prev_sample >= 0 && sample != prev_sample) {
            transitions++;
        }

        // Track the most recent contiguous run for SOLID/OFF detection.
        if (newest_run_value < 0) {
            newest_run_value = sample;
            newest_run_length = 1;
        } else if (sample == newest_run_value) {
            newest_run_length++;
        }
        // Once the run breaks, stop counting (newest_run_length frozen).

        prev_sample = sample;
        idx = (idx - 1 + CLASSIFIER_HISTORY_SIZE) % CLASSIFIER_HISTORY_SIZE;
    }

    // BLINK: at least 2 transitions = at least one full half-cycle
    // visible. One transition alone could be a state change (off to
    // solid) rather than a blink.
    if (transitions >= 2) {
        return LED_CLASS_BLINK;
    }

    // SOLID: most recent N samples are all HIGH.
    if (newest_run_value == 1 && newest_run_length >= SOLID_CONFIRM_SAMPLES) {
        return LED_CLASS_SOLID;
    }

    // OFF: most recent N samples are all LOW.
    if (newest_run_value == 0 && newest_run_length >= OFF_CONFIRM_SAMPLES) {
        return LED_CLASS_OFF;
    }

    // Not enough recent stable samples; classifier is still settling.
    // Return previous value (don't update). This handles the
    // transition between SOLID and OFF cleanly by holding the last
    // confirmed state during transient periods.
    return s->classifier;
}

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

static esp_err_t nvs_load_last_locked(int8_t *driver_out, int8_t *pax_out)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // First boot, no namespace yet. Treat as no history.
        *driver_out = -1;
        *pax_out = -1;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s.", esp_err_to_name(err));
        return err;
    }

    int8_t drv = -1, pax = -1;
    err = nvs_get_i8(handle, NVS_KEY_DRV_LOCKED, &drv);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_i8(drv) failed: %s.", esp_err_to_name(err));
    }
    err = nvs_get_i8(handle, NVS_KEY_PAX_LOCKED, &pax);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_i8(pax) failed: %s.", esp_err_to_name(err));
    }

    nvs_close(handle);
    *driver_out = drv;
    *pax_out = pax;
    return ESP_OK;
}

static void nvs_save_last_locked(const char *key, int8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(rw) failed: %s.", esp_err_to_name(err));
        return;
    }
    err = nvs_set_i8(handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i8(%s, %d) failed: %s.",
                 key, value, esp_err_to_name(err));
    } else {
        err = nvs_commit(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit failed: %s.", esp_err_to_name(err));
        }
    }
    nvs_close(handle);
}

static esp_err_t nvs_load_last_open(int8_t *driver_out, int8_t *pax_out)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // First boot, no namespace yet. Treat as no history.
        *driver_out = -1;
        *pax_out = -1;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s.", esp_err_to_name(err));
        return err;
    }

    int8_t drv = -1, pax = -1;
    err = nvs_get_i8(handle, NVS_KEY_DRV_OPEN, &drv);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_i8(drv_op) failed: %s.", esp_err_to_name(err));
    }
    err = nvs_get_i8(handle, NVS_KEY_PAX_OPEN, &pax);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_i8(pax_op) failed: %s.", esp_err_to_name(err));
    }

    nvs_close(handle);
    *driver_out = drv;
    *pax_out = pax;
    return ESP_OK;
}

static void nvs_save_last_open(const char *key, int8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(rw) failed: %s.", esp_err_to_name(err));
        return;
    }
    err = nvs_set_i8(handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i8(%s, %d) failed: %s.",
                 key, value, esp_err_to_name(err));
    } else {
        err = nvs_commit(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit failed: %s.", esp_err_to_name(err));
        }
    }
    nvs_close(handle);
}

// ---------------------------------------------------------------------------
// Matter pushers (bounce work to CHIP task via PlatformMgr)
// ---------------------------------------------------------------------------

// Helper context for pushing BooleanState::StateValue updates to
// Matter on the CHIP task.
//
struct contact_push_ctx {
    uint16_t endpoint_id;
    bool value;
};

static void push_contact_sensor_chip_task(intptr_t arg)
{
    using namespace chip::app::Clusters;

    contact_push_ctx *ctx = (contact_push_ctx *)arg;

    // 1.4.2~2: BooleanState::StateValue is created with
    // ATTRIBUTE_FLAG_NONE (a normal attribute), so the standard
    // esp_matter attribute API updates it. The cluster-setter API
    // (BooleanStateCluster::SetStateValue) is a v1.5+ addition not
    // present in this registry release.
    esp_matter_attr_val_t val = esp_matter_bool(ctx->value);
    esp_err_t err = esp_matter::attribute::update(
        ctx->endpoint_id,
        BooleanState::Id,
        BooleanState::Attributes::StateValue::Id,
        &val);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "BooleanState pushed: endpoint=%u value=%d.",
                 ctx->endpoint_id, ctx->value ? 1 : 0);
    } else {
        ESP_LOGW(TAG, "BooleanState update failed: endpoint=%u value=%d err=%s.",
                 ctx->endpoint_id, ctx->value ? 1 : 0, esp_err_to_name(err));
    }
    delete ctx;
}

static void push_contact_sensor(uint16_t endpoint_id, bool is_open)
{
    // Matter ContactSensor device type uses BooleanState cluster.
    // Per Matter spec (validated empirically against Apple Home on van):
    //   StateValue = true  -> contact made (door CLOSED)
    //   StateValue = false -> no contact   (door OPEN)
    //
    // So invert is_open before pushing: closed door pushes true.
    bool state_value = !is_open;

    contact_push_ctx *ctx = new contact_push_ctx{
        endpoint_id,
        state_value,
    };

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        push_contact_sensor_chip_task, (intptr_t)ctx);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "ScheduleWork failed for contact sensor endpoint %u.", endpoint_id);
        delete ctx;
    }
}

struct lock_push_ctx {
    uint16_t endpoint_id;
    uint8_t lock_state;  // doors_lock_state_t value (excluding UNKNOWN)
};

static void push_lock_state_chip_task(intptr_t arg)
{
    using namespace chip::app::Clusters;

    lock_push_ctx *ctx = (lock_push_ctx *)arg;

    // SetLockState in this esp-matter version takes a plain
    // DlLockState, not Nullable. UNKNOWN is filtered out at the
    // caller in push_lock_state() below.
    DoorLock::DlLockState state =
        static_cast<DoorLock::DlLockState>(ctx->lock_state);

    DoorLockServer::Instance().SetLockState(ctx->endpoint_id, state);
    delete ctx;
}

static void push_lock_state(uint16_t endpoint_id, doors_lock_state_t state)
{
    // The Matter SetLockState API in this esp-matter version doesn't
    // accept a Nullable, so Unknown can't be pushed as null. Skip
    // the push entirely when state is UNKNOWN; Matter holds whatever
    // value was last set (the safest behavior).
    //
    // This only matters at first-boot with CTM asleep and no NVS
    // history. Once any real state has been observed and persisted,
    // there is always something concrete to push.
    if (state == DOORS_LOCK_STATE_UNKNOWN) {
        ESP_LOGI(TAG, "Lock state UNKNOWN. Skipping Matter push (no NVS history yet).");
        return;
    }

    lock_push_ctx *ctx = new lock_push_ctx{
        endpoint_id,
        static_cast<uint8_t>(state),
    };

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        push_lock_state_chip_task, (intptr_t)ctx);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "ScheduleWork failed for lock endpoint %u.", endpoint_id);
        delete ctx;
    }
}

// ---------------------------------------------------------------------------
// State changes (runs on sampling task, decides what to push)
// ---------------------------------------------------------------------------

static void update_last_stable_and_nvs(struct led_state *s, const char *key)
{
    int8_t new_stable;
    if (s->classifier == LED_CLASS_SOLID) {
        new_stable = 1;
    } else if (s->classifier == LED_CLASS_OFF) {
        new_stable = 0;
    } else if (s->classifier == LED_CLASS_BLINK) {
        // BLINK means the door is open. The LED only returns to
        // SOLID after the door is closed AND a lock event fires
        // (MLS press, key fob, or our pulse). Treat BLINK the same
        // as OFF for last_stable_locked, running through the gate
        // so pre-sleep transients don't fire false commits.
        new_stable = 0;
    } else {
        // UNKNOWN. Classifier hasn't accumulated enough history.
        // Clear any pending stability gate but don't change
        // last_stable_locked.
        s->pending_stable = -1;
        return;
    }

    if (s->last_stable_locked == new_stable) {
        // Already at this stable state; nothing to do. Clear any
        // pending gate in case the classifier had flapped briefly.
        s->pending_stable = -1;
        return;
    }

    // Classifier wants to change last_stable_locked. Run it through
    // the stability gate.
    int64_t now_us = esp_timer_get_time();
    if (s->pending_stable != new_stable) {
        // New pending change. Start the gate timer.
        s->pending_stable = new_stable;
        s->pending_since_us = now_us;
        return;
    }

    // Pending state matches; check if gate has expired.
    int64_t held_ms = (now_us - s->pending_since_us) / 1000;
    if (held_ms < NVS_STABILITY_MS) {
        return;
    }

    // Gate expired; commit.
    ESP_LOGI(TAG, "Lock state committed for %s: %d -> %d (stable %lldms).",
             key, s->last_stable_locked, new_stable, held_ms);
    s->last_stable_locked = new_stable;
    s->pending_stable = -1;
    nvs_save_last_locked(key, new_stable);
}

static void update_last_stable_open_and_nvs(struct led_state *s, const char *key)
{
    int8_t new_stable;
    if (s->classifier == LED_CLASS_BLINK) {
        new_stable = 1;  // OPEN
    } else if (s->classifier == LED_CLASS_SOLID || s->classifier == LED_CLASS_OFF) {
        new_stable = 0;  // CLOSED (both lock and unlock close states map here)
    } else {
        // UNKNOWN. Classifier hasn't accumulated enough history.
        // Clear any pending stability gate but don't change
        // last_stable_open.
        s->pending_open = -1;
        return;
    }

    if (s->last_stable_open == new_stable) {
        // Already at this stable state; nothing to do. Clear any
        // pending gate in case the classifier had flapped briefly.
        s->pending_open = -1;
        return;
    }

    // Classifier wants to change last_stable_open. Run it through
    // the stability gate.
    int64_t now_us = esp_timer_get_time();
    if (s->pending_open != new_stable) {
        // New pending change. Start the gate timer.
        s->pending_open = new_stable;
        s->pending_open_since_us = now_us;
        return;
    }

    // Pending state matches; check if gate has expired.
    int64_t held_ms = (now_us - s->pending_open_since_us) / 1000;
    if (held_ms < DOOR_STATE_STABILITY_MS) {
        return;
    }

    // Gate expired; commit.
    ESP_LOGI(TAG, "Door open state committed for %s: %d -> %d (stable %lldms).",
             key, s->last_stable_open, new_stable, held_ms);
    s->last_stable_open = new_stable;
    s->pending_open = -1;
    nvs_save_last_open(key, new_stable);
}

static void push_state_if_changed(void)
{
    // Contact sensors: read from the gated stable value
    // (last_stable_open) rather than the live classifier, so the
    // push survives the CTM's pre-sleep LED fade without a false
    // CLOSED. See DOOR_STATE_STABILITY_MS for the fade-window
    // rationale.
    //
    // A value of -1 means no stable reading has committed yet
    // (first boot with no NVS history). Skip the push in that
    // case; the Matter cluster default carries until there is
    // something real to report.
    int8_t driver_open = s_driver.last_stable_open;
    int8_t pax_open    = s_pax.last_stable_open;

    int8_t prev_driver = s_last_pushed_driver_open.load();
    int8_t prev_pax    = s_last_pushed_pax_open.load();

    if (driver_open >= 0 && driver_open != prev_driver) {
        ESP_LOGI(TAG, "Driver door: %s.", driver_open ? "OPEN" : "CLOSED");
        push_contact_sensor(driver_door_endpoint_id, driver_open != 0);
        s_last_pushed_driver_open.store(driver_open);
    }
    if (pax_open >= 0 && pax_open != prev_pax) {
        ESP_LOGI(TAG, "PAX door: %s.", pax_open ? "OPEN" : "CLOSED");
        push_contact_sensor(pax_door_endpoint_id, pax_open != 0);
        s_last_pushed_pax_open.store(pax_open);
    }

    // Lock-state push quiet window: skip lock-state pushes for a
    // window after each command. See s_lock_push_quiet_until_us for
    // the full rationale. Contact-sensor pushes above are unaffected;
    // door state is independent of the lock command path.
    int64_t now_us = esp_timer_get_time();
    int64_t quiet_until = s_lock_push_quiet_until_us.load();
    if (now_us < quiet_until) {
        return;
    }

    // Lock state: computed from full LED + CTM + NVS picture.
    doors_lock_state_t lock_state = doors_compute_lock_state();
    uint8_t prev_lock = s_last_pushed_lock_state.load();

    if ((uint8_t)lock_state != prev_lock) {
        const char *name = "?";
        switch (lock_state) {
            case DOORS_LOCK_STATE_LOCKED:           name = "Locked"; break;
            case DOORS_LOCK_STATE_UNLOCKED:         name = "Unlocked"; break;
            case DOORS_LOCK_STATE_NOT_FULLY_LOCKED: name = "NotFullyLocked"; break;
            case DOORS_LOCK_STATE_UNKNOWN:          name = "Unknown"; break;
            default: break;
        }
        ESP_LOGI(TAG, "Lock state: %s.", name);
        push_lock_state(door_lock_endpoint_id, lock_state);
        s_last_pushed_lock_state.store((uint8_t)lock_state);
    }
}

// ---------------------------------------------------------------------------
// CTM transition callback
// ---------------------------------------------------------------------------

static void on_ctm_transition(bool now_awake)
{
    // Runs from ctm_monitor task context. MUST NOT BLOCK.
    if (now_awake) {
        ESP_LOGI(TAG, "CTM is awake. Clearing LED history, starting blackout, and resuming sampling.");
        history_clear(&s_driver);
        history_clear(&s_pax);
        s_driver.classifier = LED_CLASS_UNKNOWN;
        s_pax.classifier    = LED_CLASS_UNKNOWN;
        // Clear any pending stability gates from before sleep.
        // Pre-sleep transient readings should not influence
        // post-wake commits for either the lock-state gate or
        // the door-state gate.
        s_driver.pending_stable = -1;
        s_pax.pending_stable    = -1;
        s_driver.pending_open   = -1;
        s_pax.pending_open      = -1;
    } else {
        ESP_LOGI(TAG, "CTM is asleep. Suspending sampling. NVS already current.");
    }

    s_ctm_awake.store(now_awake);
    s_blackout_until_us.store(esp_timer_get_time() + (int64_t)LED_BLACKOUT_MS * 1000);

    if (now_awake) {
        // Wake the sampling task (no-op if already running)
        xSemaphoreGive(s_wake_sem);
    }

    // On asleep transition, no Matter push is issued: LEDs going dark
    // doesn't mean state changed. NVS still holds the correct state
    // and lock state is reported from NVS now. The sampling task
    // will see s_ctm_awake=false on its next iteration and suspend
    // itself.
}

// ---------------------------------------------------------------------------
// Sampling task
// ---------------------------------------------------------------------------

// TODO: Intermittent classifier wedge after reboot with doors held
// open for several minutes. Classifier reads not-BLINK for ~10s
// after CTM wake despite LEDs visibly blinking, commits false CLOSED.
// Self-heals on next physical door event. Hardware path is fine
// (same setup classifies correctly after recovery). Likely
// history_clear() or first-classify state issue. Repro: open both
// doors, hold 5+ min, reboot. Observed 2026-05-31.
static void doors_sampling_task(void *arg)
{
    ESP_LOGI(TAG, "doors_sampling_task started.");

    while (true) {
        // If CTM is asleep, wait for wake signal.
        if (!s_ctm_awake.load()) {
            xSemaphoreTake(s_wake_sem, portMAX_DELAY);
            // s_ctm_awake should now be true (set by callback before
            // semaphore was given). Continue to sampling loop.
            ESP_LOGI(TAG, "Sampling resumed (CTM awake).");
        }

        // Sample both LEDs
        uint8_t drv = led_read_logical_on(DRIVER_LED_GPIO);
        uint8_t pax = led_read_logical_on(PAX_LED_GPIO);

        history_append(&s_driver, drv);
        history_append(&s_pax, pax);

        // Run classifiers
        s_driver.classifier = classify(&s_driver);
        s_pax.classifier    = classify(&s_pax);

        // Update last_stable_locked + NVS if confirmed stable state
        update_last_stable_and_nvs(&s_driver, NVS_KEY_DRV_LOCKED);
        update_last_stable_and_nvs(&s_pax,    NVS_KEY_PAX_LOCKED);

        // Same pattern for last_stable_open + NVS, with the
        // door-state gate.
        update_last_stable_open_and_nvs(&s_driver, NVS_KEY_DRV_OPEN);
        update_last_stable_open_and_nvs(&s_pax,    NVS_KEY_PAX_OPEN);

        // Push to Matter only if past blackout window
        int64_t now_us = esp_timer_get_time();
        int64_t blackout_until = s_blackout_until_us.load();
        if (now_us >= blackout_until) {
            push_state_if_changed();
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_TICK_MS));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t doors_init(void)
{
    ESP_LOGI(TAG, "Initializing doors module (DRV=GPIO%d, PAX=GPIO%d).",
             DRIVER_LED_GPIO, PAX_LED_GPIO);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DRIVER_LED_GPIO) | (1ULL << PAX_LED_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s.", esp_err_to_name(ret));
        return ret;
    }

    // Initialize per-LED state.
    history_clear(&s_driver);
    history_clear(&s_pax);
    s_driver.classifier = LED_CLASS_UNKNOWN;
    s_pax.classifier    = LED_CLASS_UNKNOWN;
    s_driver.pending_stable = -1;
    s_pax.pending_stable    = -1;
    s_driver.pending_since_us = 0;
    s_pax.pending_since_us    = 0;
    s_driver.pending_open = -1;
    s_pax.pending_open    = -1;
    s_driver.pending_open_since_us = 0;
    s_pax.pending_open_since_us    = 0;

    // Load last-known locked state from NVS.
    int8_t drv_lk = -1, pax_lk = -1;
    nvs_load_last_locked(&drv_lk, &pax_lk);
    s_driver.last_stable_locked = drv_lk;
    s_pax.last_stable_locked    = pax_lk;
    ESP_LOGI(TAG, "Loaded lock state from NVS: drv=%d, pax=%d.", drv_lk, pax_lk);

    // Load last-known door open state from NVS. Same pattern as
    // lock state above. Stays -1 (unknown) if the NVS read fails
    // or the namespace has no door-open keys yet (first boot
    // after this feature was added). The first stable observation
    // after CTM wake will then commit the true value through the
    // door-state gate.
    int8_t drv_op = -1, pax_op = -1;
    nvs_load_last_open(&drv_op, &pax_op);
    s_driver.last_stable_open = drv_op;
    s_pax.last_stable_open    = pax_op;
    ESP_LOGI(TAG, "Loaded door open state from NVS: drv=%d, pax=%d.", drv_op, pax_op);

    // Initial blackout; give classifier one window to settle after boot.
    s_blackout_until_us.store(esp_timer_get_time() + (int64_t)LED_BLACKOUT_MS * 1000);

    // Wake semaphore starts taken/empty.
    s_wake_sem = xSemaphoreCreateBinary();
    if (s_wake_sem == nullptr) {
        ESP_LOGE(TAG, "Failed to create wake semaphore.");
        return ESP_ERR_NO_MEM;
    }

    // Spawn sampling task. Initial state assumes CTM asleep (safe
    // default matching ctm_state init). Task will block on semaphore
    // until ctm_state callback fires with awake=true.
    s_ctm_awake.store(false);

    BaseType_t task_ret = xTaskCreate(
        doors_sampling_task,
        "doors_sample",
        TASK_STACK_BYTES,
        nullptr,
        TASK_PRIORITY,
        &s_task_handle
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(doors_sample) failed.");
        vSemaphoreDelete(s_wake_sem);
        s_wake_sem = nullptr;
        return ESP_ERR_NO_MEM;
    }

    // Subscribe to CTM state changes
    ctm_state_register_callback(on_ctm_transition);

    s_initialized.store(true);
    ESP_LOGI(TAG, "Doors module ready. Sampling suspended until CTM is awake.");
    return ESP_OK;
}

bool doors_driver_is_open(void)
{
    // Use stability-gated value so this matches what gets pushed
    // to Matter. Returns false if unknown (conservative; same
    // pattern as doors_driver_is_locked).
    return (s_driver.last_stable_open == 1);
}

bool doors_pax_is_open(void)
{
    return (s_pax.last_stable_open == 1);
}

bool doors_driver_classifier_is_blink(void)
{
    // Live classifier output, not the gated stable value. Used by
    // the Lock command pre-check so a freshly-closed door is
    // accepted within ~1.5 seconds rather than the 10-second
    // stability gate.
    return (s_driver.classifier == LED_CLASS_BLINK);
}

bool doors_pax_classifier_is_blink(void)
{
    return (s_pax.classifier == LED_CLASS_BLINK);
}

bool doors_lock_state_needs_sync_pulse(void)
{
    // Live classifier output, not the gated stable value. Used by
    // the Lock command path to decide whether the standard single
    // pulse would achieve user intent or whether an extra sync
    // pulse is required first. The check needs to reflect current
    // physical reality, not state from 10 seconds ago, because
    // partial state often results from a user action (interior
    // handle pull, MLS press) immediately before they tap Lock in
    // Apple Home.
    //
    // Hardware-characterized rule: the Sprinter CTM treats the
    // PAX-group lock state as authoritative. A single lock pulse
    // syncs the driver side to match PAX:
    //
    //   driver SOLID + PAX OFF   -> 1 pulse unlocks driver (bad,
    //                               opposite of user intent). Return
    //                               TRUE so the command emits 2
    //                               pulses, with pulse 1 syncing
    //                               both to OFF and pulse 2 locking
    //                               both.
    //   driver OFF + PAX SOLID   -> 1 pulse locks driver (good,
    //                               matches user intent). Return
    //                               FALSE; single pulse works.
    //
    // UNKNOWN or BLINK on either side returns FALSE (conservative;
    // BLINK should have been caught by the door-open pre-check).
    return (s_driver.classifier == LED_CLASS_SOLID) &&
           (s_pax.classifier    == LED_CLASS_OFF);
}

bool doors_driver_is_locked(void)
{
    // Use stability-gated value so this matches doors_compute_lock_state.
    // Returns false if unknown (conservative; pre-first-commit).
    return (s_driver.last_stable_locked == 1);
}

bool doors_pax_is_locked(void)
{
    return (s_pax.last_stable_locked == 1);
}

bool doors_vehicle_is_fully_locked(void)
{
    return (doors_driver_is_locked() && doors_pax_is_locked());
}

// Binary lock state model: Locked or Unlocked.
//
// LOCKED requires both driver and PAX last_stable_locked == 1
// (committed via the NVS stability gate). Anything else is
// UNLOCKED. If a door opens, this reports Unlocked.
//
// Reading from last_stable_locked instead of the raw classifier
// means the Matter push and the NVS write share the same stability
// gate, which suppresses the spurious Unlocked push the CTM's
// pre-sleep LED fade would otherwise cause at sleep. See
// NVS_STABILITY_MS for the fade-window rationale. The cost is that
// legitimate lock/unlock events take up to NVS_STABILITY_MS to
// appear in Apple Home / HA; the CTM stays awake long after lock
// events, so the gate always has time to expire on real changes.
//
// UNKNOWN is only returned at first boot before either side has
// ever committed a stable reading. After the first CTM wake +
// stability gate expiration, last_stable_locked has real data and
// UNKNOWN never appears again. push_lock_state() skips Matter
// pushes for UNKNOWN so the Matter cluster default value carries
// until there's something real to report.
//
// Manual single-door lock/unlock via the interior handle is only
// observable while the CTM is awake and driving the MLS LEDs;
// once the CTM sleeps the LEDs are dark and the change is
// invisible until the next observable lock event.
doors_lock_state_t doors_compute_lock_state(void)
{
    int8_t drv_lk = s_driver.last_stable_locked;
    int8_t pax_lk = s_pax.last_stable_locked;

    if (drv_lk < 0 || pax_lk < 0) {
        // First boot, no committed history yet. Genuinely unknown.
        return DOORS_LOCK_STATE_UNKNOWN;
    }

    return (drv_lk == 1 && pax_lk == 1) ? DOORS_LOCK_STATE_LOCKED
                                        : DOORS_LOCK_STATE_UNLOCKED;
}

void doors_set_pushed_lock_state(doors_lock_state_t state)
{
    // Called from outside the sampling task (e.g. door_lock.cpp's
    // Matter command handlers) when something other than
    // push_state_if_changed writes the Matter LockState attribute.
    // The atomic store keeps the tracker consistent with the
    // attribute's actual value so the next sampling tick's
    // change-detection compares against truth.
    s_last_pushed_lock_state.store((uint8_t)state);

    // Start the lock-state push quiet window. push_state_if_changed
    // will skip its lock-state push for the next LOCK_PUSH_QUIET_MS,
    // letting the stability gate commit a fresh observation before
    // the next push fires. Without this, the gate's pre-commit value
    // would push within ~50ms, causing a LOCKED -> UNLOCKED ->
    // LOCKED flicker in Apple Home on every command.
    int64_t until = esp_timer_get_time() + (int64_t)LOCK_PUSH_QUIET_MS * 1000;
    s_lock_push_quiet_until_us.store(until);
}

void doors_start_blackout(void)
{
    int64_t until = esp_timer_get_time() + (int64_t)LED_BLACKOUT_MS * 1000;
    s_blackout_until_us.store(until);
    ESP_LOGI(TAG, "Blackout started for %lums.", LED_BLACKOUT_MS);
}