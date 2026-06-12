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

// reset_button.cpp

#include "reset_button.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <driver/gpio.h>
#include <button_gpio.h>
#include <iot_button.h>

static const char *TAG = "reset_button";

// XIAO ESP32-C6 BOOT button: GPIO9, active LOW, internal pull-up.
#define RESET_BUTTON_GPIO          GPIO_NUM_9
#define RESET_BUTTON_ACTIVE_LEVEL  0
#define RESET_LONG_PRESS_MS        10000

static void factory_reset_cb(void *arg, void *data)
{
    ESP_LOGW(TAG, "Long press detected. Triggering Matter factory reset.");
    esp_matter::factory_reset();
}

esp_err_t reset_button_init(void)
{
    ESP_LOGI(TAG, "Initializing factory reset button on GPIO%d (long press: %dms).",
             RESET_BUTTON_GPIO, RESET_LONG_PRESS_MS);

    const button_config_t btn_cfg = {
        .long_press_time = RESET_LONG_PRESS_MS,
    };

    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = RESET_BUTTON_GPIO,
        .active_level = RESET_BUTTON_ACTIVE_LEVEL,
    };

    button_handle_t handle = NULL;
    esp_err_t ret = iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button device: %s.", esp_err_to_name(ret));
        return ret;
    }

    iot_button_register_cb(handle, BUTTON_LONG_PRESS_UP, NULL, factory_reset_cb, NULL);

    ESP_LOGI(TAG, "Factory reset button ready. Hold %dms to reset.", RESET_LONG_PRESS_MS);
    return ESP_OK;
}