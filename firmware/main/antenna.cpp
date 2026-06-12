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

// antenna.cpp

#include "antenna.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "antenna";

// GPIO3 enables the onboard RF switch when driven LOW. The Seeed
// docs call this WIFI_ENABLE in their Arduino example. The name is
// misleading: the pin gates the RF switch, not Wi-Fi itself.
#define RF_SWITCH_ENABLE_GPIO  GPIO_NUM_3

// GPIO14 selects which antenna the RF switch routes to:
//   LOW  - built-in ceramic chip antenna
//   HIGH - external U.FL antenna
#define ANTENNA_SELECT_GPIO    GPIO_NUM_14

// Brief settle delay between enabling the switch and selecting the
// antenna. Matches the 100ms delay in the Seeed Arduino example.
#define ANTENNA_SETTLE_MS      100

esp_err_t antenna_init(void)
{
    ESP_LOGI(TAG, "Initializing external antenna (GPIO%d enable, GPIO%d select).",
             RF_SWITCH_ENABLE_GPIO, ANTENNA_SELECT_GPIO);

    gpio_config_t enable_cfg = {
        .pin_bit_mask = (1ULL << RF_SWITCH_ENABLE_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&enable_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(enable) failed: %s.", esp_err_to_name(ret));
        return ret;
    }

    // Drive enable LOW to activate the RF switch.
    gpio_set_level(RF_SWITCH_ENABLE_GPIO, 0);

    vTaskDelay(pdMS_TO_TICKS(ANTENNA_SETTLE_MS));

    gpio_config_t select_cfg = {
        .pin_bit_mask = (1ULL << ANTENNA_SELECT_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&select_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(select) failed: %s.", esp_err_to_name(ret));
        return ret;
    }

    // Drive select HIGH to route RF to the external antenna.
    gpio_set_level(ANTENNA_SELECT_GPIO, 1);

    ESP_LOGI(TAG, "External antenna selected.");
    return ESP_OK;
}