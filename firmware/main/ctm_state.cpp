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

// ctm_state.cpp

#include "ctm_state.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>

static const char *TAG = "ctm_state";

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------

// GPIO2: LM393 comparator output monitoring the WT_RD pulse train.
// Edges occur continuously in both CTM states; the rate is what
// differs. The instantaneous level is meaningless on its own.
#define CTM_GPIO  GPIO_NUM_2

// ---------------------------------------------------------------------------
// Detection parameters (validated 2026-05-24)
// ---------------------------------------------------------------------------

// Asleep ~238 edges/5s, awake ~448 edges/5s. Asleep sits 112 below
// the 350 threshold and awake 98 above it, safely outside the ±2
// measured variance.
static constexpr int CTM_AWAKE_THRESHOLD_5S = 350;

// 5-second sliding window matches the threshold tuning above.
static constexpr uint32_t CTM_WINDOW_MS = 5000;

// Raw state must hold this long before the official state flips.
// Defends against transients like the lock pulse spike.
static constexpr uint32_t CTM_STICKY_MS = 5000;

// Monitor task evaluation interval.
static constexpr uint32_t CTM_TICK_MS = 1000;

// At 89 Hz awake rate, 2048 entries hold ~23s of edge history. More
// than the 5s window needs, with cushion for the lock pulse spike
// (which can briefly hit ~170 Hz).
static constexpr int CTM_RING_SIZE = 2048;

// Task config
static constexpr uint32_t CTM_TASK_STACK_BYTES = 4096;
static constexpr UBaseType_t CTM_TASK_PRIORITY = 5;  // Matches status_led

// ---------------------------------------------------------------------------
// State (shared between ISR and monitor task)
// ---------------------------------------------------------------------------

// Edge timestamp ring buffer. ISR appends, monitor task reads a
// snapshot. edge_head is owned by the ISR.
static volatile uint32_t s_edge_times[CTM_RING_SIZE];
static volatile int s_edge_head = 0;

// Official state. Written only by monitor task; read from any task.
// Initialized asleep, the safe default for door_lock pulse selection.
static std::atomic<bool> s_official_awake{false};
static std::atomic<bool> s_initialized{false};

// Sticky classifier state. Owned by monitor task.
static bool s_raw_inferred_awake = false;
static int64_t s_raw_stable_since_us = 0;

// Callback slot. Atomic so registrations from any task are safe.
static std::atomic<ctm_state_changed_cb_t> s_callback{nullptr};

static TaskHandle_t s_task_handle = nullptr;

// ---------------------------------------------------------------------------
// ISR (edge detection on GPIO2)
// ---------------------------------------------------------------------------

static void IRAM_ATTR ctm_edge_isr(void *arg)
{
    // esp_timer_get_time is IRAM-safe and returns microseconds.
    // Storing as milliseconds keeps the math compact.
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    int head = s_edge_head;
    s_edge_times[head] = now_ms;
    s_edge_head = (head + 1) % CTM_RING_SIZE;
}

// ---------------------------------------------------------------------------
// Edge counting
// ---------------------------------------------------------------------------

// Walk ring buffer backwards from head, count edges within window.
static int count_edges_in_window(uint32_t now_ms, uint32_t window_ms)
{
    int head_snapshot = s_edge_head;
    int count = 0;

    for (int i = 0; i < CTM_RING_SIZE; i++) {
        int idx = (head_snapshot - 1 - i + CTM_RING_SIZE) % CTM_RING_SIZE;
        uint32_t ts = s_edge_times[idx];
        if (ts == 0) {
            // Unused slot, or ring wrapped past zero (extremely
            // unlikely since millis wraps at 49.7 days). Stop here.
            break;
        }
        if ((now_ms - ts) <= window_ms) {
            count++;
        } else {
            // Past the window. Older entries are out of scope.
            break;
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// Sticky classifier
// ---------------------------------------------------------------------------

static void update_classifier(int64_t now_us, bool raw_awake_now)
{
    if (raw_awake_now != s_raw_inferred_awake) {
        // Raw state just changed; restart sticky timer.
        s_raw_inferred_awake = raw_awake_now;
        s_raw_stable_since_us = now_us;
        return;
    }

    // Raw state hasn't changed since last tick. Stable long enough
    // to commit?
    int64_t stable_for_us = now_us - s_raw_stable_since_us;
    if (stable_for_us < (int64_t)CTM_STICKY_MS * 1000) {
        return;
    }

    // Stable long enough. Commit to official state if different.
    bool prev_official = s_official_awake.load();
    if (prev_official == s_raw_inferred_awake) {
        return;
    }

    s_official_awake.store(s_raw_inferred_awake);

    ESP_LOGI(TAG, "CTM state flip: %s -> %s.",
             prev_official ? "AWAKE" : "ASLEEP",
             s_raw_inferred_awake ? "AWAKE" : "ASLEEP");

    // Snapshot the callback pointer atomically to avoid racing with a
    // registration change mid-call.
    ctm_state_changed_cb_t cb = s_callback.load();
    if (cb != nullptr) {
        cb(s_raw_inferred_awake);
    }
}

// ---------------------------------------------------------------------------
// Monitor task
// ---------------------------------------------------------------------------

static void ctm_monitor_task(void *arg)
{
    ESP_LOGI(TAG, "ctm_monitor task started (tick %lums, threshold %d edges/%lums).",
             CTM_TICK_MS, CTM_AWAKE_THRESHOLD_5S, CTM_WINDOW_MS);

    // Initialize stable_since to now so the first tick doesn't
    // immediately flip the state. Initial state is asleep; if the
    // CTM is actually awake at boot, the first ~5s register
    // raw_awake=true and the sticky timer counts from boot.
    s_raw_stable_since_us = esp_timer_get_time();

    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CTM_TICK_MS));

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        int64_t now_us = esp_timer_get_time();

        int edges_5s = count_edges_in_window(now_ms, CTM_WINDOW_MS);
        bool raw_awake_now = (edges_5s >= CTM_AWAKE_THRESHOLD_5S);

        update_classifier(now_us, raw_awake_now);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t ctm_state_init(void)
{
    ESP_LOGI(TAG, "Initializing CTM state detector on GPIO%d.", CTM_GPIO);

    for (int i = 0; i < CTM_RING_SIZE; i++) {
        s_edge_times[i] = 0;
    }
    s_edge_head = 0;

    // The LM393 output is open-collector. The board pulls it up to 3V3
    // through R9 (10k), so the internal pull-up here is a fallback that
    // keeps the line from floating if R9 is unpopulated. Interrupt on
    // any edge (rising and falling).
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CTM_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s.", esp_err_to_name(ret));
        return ret;
    }

    // ESP_ERR_INVALID_STATE is fine here. It means another module
    // already installed the ISR service.
    ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s.", esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_isr_handler_add(CTM_GPIO, ctm_edge_isr, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s.", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t task_ret = xTaskCreate(
        ctm_monitor_task,
        "ctm_monitor",
        CTM_TASK_STACK_BYTES,
        nullptr,
        CTM_TASK_PRIORITY,
        &s_task_handle
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(ctm_monitor) failed.");
        gpio_isr_handler_remove(CTM_GPIO);
        return ESP_ERR_NO_MEM;
    }

    s_initialized.store(true);
    ESP_LOGI(TAG, "CTM state detector ready (initial state: ASLEEP, threshold %d edges/%lums, sticky %lums).",
             CTM_AWAKE_THRESHOLD_5S, CTM_WINDOW_MS, CTM_STICKY_MS);

    return ESP_OK;
}

bool ctm_state_is_awake(void)
{
    // Pre-init returns false (asleep), the conservative default.
    if (!s_initialized.load()) {
        return false;
    }
    return s_official_awake.load();
}

void ctm_state_register_callback(ctm_state_changed_cb_t cb)
{
    s_callback.store(cb);
}