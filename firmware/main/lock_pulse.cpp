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

// lock_pulse.cpp

#include "lock_pulse.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <atomic>

static const char *TAG = "lock_pulse";

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------

// GPIO1 drives Q1's base via R1. Q1 collector pulls WT_YL (master
// lock pulse line) to ground when conducting. GPIO1 HIGH asserts.
#define LOCK_PULSE_GPIO  GPIO_NUM_1

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

static constexpr uint32_t PULSE_WIDTH_MS = 500;

// Pulse 1 wakes the CTM; the CTM needs this delay before pulse 2 is
// processed as a lock action.
static constexpr uint32_t CTM_WAKE_DELAY_MS = 500;

// Minimum gap between sequences (not within a sequence; the 500ms
// intra-sequence gap is separate and not affected).
static constexpr uint32_t SEQUENCE_COOLDOWN_MS = 300;

// ---------------------------------------------------------------------------
// Task config
// ---------------------------------------------------------------------------

static constexpr uint32_t TASK_STACK_BYTES = 3072;

// Higher than background monitor tasks so pulse timing isn't
// preempted, lower than the CHIP task so Matter work is never
// delayed.
static constexpr UBaseType_t TASK_PRIORITY = 10;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static TaskHandle_t s_task_handle = nullptr;

// Signaled by request(), waited on by task.
static SemaphoreHandle_t s_work_sem = nullptr;

// Pending sequence count (1 or 2). Set under s_busy guard.
static std::atomic<int> s_pending_count{0};

// True between request() acceptance and task completion.
static std::atomic<bool> s_busy{false};

// End timestamp of the last completed sequence, for cooldown checks.
// Initialized to "long ago"; cannot use INT64_MIN because the
// (now_us - last_end_us) subtraction would overflow.
static std::atomic<int64_t> s_last_sequence_end_us{
    -(int64_t)SEQUENCE_COOLDOWN_MS * 1000 * 10};

static std::atomic<bool> s_initialized{false};

// Optional completion callback, fires after the final pulse.
static std::atomic<lock_pulse_complete_cb_t> s_complete_cb{nullptr};

// ---------------------------------------------------------------------------
// Worker task: executes pulse sequences
// ---------------------------------------------------------------------------

static void fire_single_pulse(void)
{
    gpio_set_level(LOCK_PULSE_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(PULSE_WIDTH_MS));
    gpio_set_level(LOCK_PULSE_GPIO, 0);
}

static void lock_pulse_task(void *arg)
{
    ESP_LOGI(TAG, "lock_pulse_task started (priority %u).", (unsigned)TASK_PRIORITY);

    while (true) {
        // Dormant until request() signals.
        if (xSemaphoreTake(s_work_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int count = s_pending_count.load();
        if (count < 1 || count > 2) {
            ESP_LOGE(TAG, "Invalid pending count %d. Skipping.", count);
            s_busy.store(false);
            continue;
        }

        ESP_LOGI(TAG, "Sequence started (%d pulse%s).",
                 count, count == 1 ? "" : "s");

        ESP_LOGI(TAG, "Pulse 1 firing on GPIO%d (HIGH for %lums).",
                 LOCK_PULSE_GPIO, PULSE_WIDTH_MS);
        fire_single_pulse();
        ESP_LOGI(TAG, "Pulse 1 complete.");

        if (count == 2) {
            ESP_LOGI(TAG, "Wake delay: %lums.", CTM_WAKE_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(CTM_WAKE_DELAY_MS));

            ESP_LOGI(TAG, "Pulse 2 firing on GPIO%d (HIGH for %lums).",
                     LOCK_PULSE_GPIO, PULSE_WIDTH_MS);
            fire_single_pulse();
            ESP_LOGI(TAG, "Pulse 2 complete.");
        }

        s_last_sequence_end_us.store(esp_timer_get_time());

        // Clear busy before the callback so a callback that enqueues
        // another sequence works cleanly.
        s_busy.store(false);

        ESP_LOGI(TAG, "Sequence complete.");

        lock_pulse_complete_cb_t cb = s_complete_cb.load();
        if (cb != nullptr) {
            cb();
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t lock_pulse_init(void)
{
    ESP_LOGI(TAG, "Initializing lock pulse on GPIO%d.", LOCK_PULSE_GPIO);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LOCK_PULSE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s.", esp_err_to_name(ret));
        return ret;
    }

    // Pull-down plus drive-LOW: redundant safety so the line is never
    // floating high during early boot.
    gpio_set_level(LOCK_PULSE_GPIO, 0);

    s_work_sem = xSemaphoreCreateBinary();
    if (s_work_sem == nullptr) {
        ESP_LOGE(TAG, "Failed to create work semaphore.");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ret = xTaskCreate(
        lock_pulse_task,
        "lock_pulse",
        TASK_STACK_BYTES,
        nullptr,
        TASK_PRIORITY,
        &s_task_handle
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(lock_pulse) failed.");
        vSemaphoreDelete(s_work_sem);
        s_work_sem = nullptr;
        return ESP_ERR_NO_MEM;
    }

    s_initialized.store(true);
    ESP_LOGI(TAG, "Lock pulse ready (GPIO%d driven LOW, cooldown %lums, "
             "pulse %lums, wake delay %lums).",
             LOCK_PULSE_GPIO, SEQUENCE_COOLDOWN_MS,
             PULSE_WIDTH_MS, CTM_WAKE_DELAY_MS);
    return ESP_OK;
}

lock_pulse_result_t lock_pulse_request(int count)
{
    if (!s_initialized.load()) {
        ESP_LOGW(TAG, "Request received before init. Rejecting.");
        return LOCK_PULSE_RESULT_SUPPRESSED_BUSY;
    }

    if (count < 1 || count > 2) {
        ESP_LOGW(TAG, "Invalid count %d. Must be 1 or 2.", count);
        return LOCK_PULSE_RESULT_INVALID_COUNT;
    }

    // Cooldown check before touching the busy flag, so a cooldown
    // rejection doesn't temporarily lock out other state.
    int64_t now_us = esp_timer_get_time();
    int64_t last_end_us = s_last_sequence_end_us.load();
    int64_t since_last_us = now_us - last_end_us;
    if (since_last_us < (int64_t)SEQUENCE_COOLDOWN_MS * 1000) {
        ESP_LOGW(TAG, "Request suppressed: cooldown active "
                 "(%lldms since last, need %lums).",
                 since_last_us / 1000, SEQUENCE_COOLDOWN_MS);
        return LOCK_PULSE_RESULT_SUPPRESSED_COOLDOWN;
    }

    // Atomic claim of the busy flag. Returns true only on the
    // false-to-true transition.
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "Request suppressed: sequence still in progress.");
        return LOCK_PULSE_RESULT_SUPPRESSED_BUSY;
    }

    s_pending_count.store(count);
    xSemaphoreGive(s_work_sem);

    ESP_LOGI(TAG, "Request enqueued (%d pulse%s).",
             count, count == 1 ? "" : "s");
    return LOCK_PULSE_RESULT_ENQUEUED;
}

void lock_pulse_register_complete_cb(lock_pulse_complete_cb_t cb)
{
    s_complete_cb.store(cb);
}