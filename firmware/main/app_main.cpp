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

//app_main.cpp

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#if CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <common_macros.h>
#include "lock_pulse.h"
#include "ctm_state.h"
#include "doors.h"
#include "reset_button.h"
#include "status_led.h"
#include "antenna.h"
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>

static const char *TAG = "app_main";
uint16_t door_lock_endpoint_id = 0;
uint16_t driver_door_endpoint_id = 0;
uint16_t pax_door_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
using namespace chip;

constexpr auto k_timeout_seconds = 300;

// Forward declaration for the door_lock.cpp linker anchor.
// See comment above the door_lock_init() definition in door_lock.cpp
// for the rationale.
extern "C" void door_lock_init();

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                           \
    {                                                                                   \
        .radio_mode = RADIO_MODE_NATIVE,                                                \
    }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                            \
    {                                                                                   \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                              \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, \
    }
#endif

#if CONFIG_ENABLE_ENCRYPTED_OTA
extern const char decryption_key_start[] asm("_binary_esp_image_encryption_key_pem_start");
extern const char decryption_key_end[] asm("_binary_esp_image_encryption_key_pem_end");

static const char *s_decryption_key = decryption_key_start;
static const uint16_t s_decryption_key_len = decryption_key_end - decryption_key_start;
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP address changed.");
        break;

    case chip::DeviceLayer::DeviceEventType::kThreadStateChange:
        ESP_LOGI(TAG, "Thread state changed.");
        if (chip::DeviceLayer::ConnectivityMgr().IsThreadAttached()) {
            ESP_LOGI(TAG, "Thread attached.");
            status_led_set_state(LED_STATE_THREAD_CONNECTED);
        } else {
            ESP_LOGI(TAG, "Thread not attached.");
            // Only switch to CONNECTING if we have fabrics, so we
            // don't overwrite BLE_ADVERTISING during initial
            // commissioning.
            if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
                status_led_set_state(LED_STATE_THREAD_CONNECTING);
            }
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete.");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed. Fail-safe timer expired.");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started.");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped.");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened.");
        // A commissioned device can open windows for Multi-Admin
        // pairing without being uncommissioned. Only show BLE
        // advertising LED if there are no fabrics.
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            status_led_set_state(LED_STATE_BLE_ADVERTISING);
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed.");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "Fabric removed successfully.");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            chip::CommissioningWindowManager  &commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
            constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
            if (!commissionMgr.IsCommissioningWindowOpen()) {
                // After removing the last fabric, open a commissioning window
                // advertising on DNS-SD only (Thread network is still attached).
                CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(kTimeoutSeconds,
                                                                            chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "Failed to open commissioning window. Error: %" CHIP_ERROR_FORMAT ".", err.Format());
                }
            }
        }
        break;
    }

    case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
        ESP_LOGI(TAG, "Fabric will be removed.");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
        ESP_LOGI(TAG, "Fabric updated.");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "Fabric committed.");
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed.");
        break;

    default:
        break;
    }
}

// Invoked when a controller issues an Identify command on any
// endpoint. Drives the status LED via status_led_start_identify /
// status_led_stop_identify.
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type=%u, effect=%u, variant=%u.", type, effect_id, effect_variant);

    switch (type) {
    case identification::callback_type_t::START:
        ESP_LOGI(TAG, "Identify START: blinking status LED.");
        status_led_start_identify();
        break;

    case identification::callback_type_t::STOP:
        ESP_LOGI(TAG, "Identify STOP: restoring status LED.");
        status_led_stop_identify();
        break;

    case identification::callback_type_t::EFFECT:
        // TriggerEffect command. effect_id selects the effect;
        // all effects currently map to the basic identify blink.
        ESP_LOGI(TAG, "Identify EFFECT %u variant %u: using default blink.", effect_id, effect_variant);
        status_led_start_identify();
        break;

    default:
        ESP_LOGW(TAG, "Unknown identification callback type: %u.", type);
        break;
    }

    return ESP_OK;
}

// Called by Matter on every attribute update. Currently a no-op;
// kept as a registration point so future per-attribute driver hooks
// can plug in here without changing the Matter setup.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
    uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
return ESP_OK;
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    // Antenna selection first: must be configured before the radio
    // comes up. Pure GPIO, no dependencies on NVS or anything else.
    antenna_init();

    nvs_flash_init();

    // Initialize status LED first so it can indicate progress during
    // the rest of boot.
    status_led_init();

    lock_pulse_init();
    ctm_state_init();
    doors_init();
    door_lock_init();

#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true
#endif
    };
    err = esp_pm_configure(&pm_config);
#endif

    // Matter node + mandatory Root Node device type on endpoint 0
    node::config_t node_config;

    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node."));

    door_lock::config_t door_lock_config;
    cluster::door_lock::feature::credential_over_the_air_access::config_t cota_config;
    cluster::door_lock::feature::pin_credential::config_t pin_credential_config;
    cluster::door_lock::feature::user::config_t user_config;
    endpoint_t *endpoint = door_lock::create(node, &door_lock_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create door lock endpoint."));
    cluster_t *door_lock_cluster = cluster::get(endpoint, DoorLock::Id);
    cluster::door_lock::feature::credential_over_the_air_access::add(door_lock_cluster, &cota_config);
    cluster::door_lock::feature::pin_credential::add(door_lock_cluster, &pin_credential_config);
    cluster::door_lock::feature::user::add(door_lock_cluster, &user_config);
    cluster::door_lock::attribute::create_auto_relock_time(door_lock_cluster, 0);
    // AutoRelockTime defaults to 0 (disabled) and resets to 0 on every
    // reboot because esp-matter doesn't persist this attribute. For van
    // use, auto-relock-while-loading is not ideal (lockouts during long
    // in-and-out activities), so the default-off behavior is acceptable.
    // Users who want persistent auto relock can file an issue; the fix
    // would be either an esp-matter patch (ATTRIBUTE_FLAG_NONVOLATILE)
    // or firmware-side NVS persistence in an auto_relock module.
    door_lock_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Door lock created with endpoint_id %d.", door_lock_endpoint_id);

    contact_sensor::config_t driver_door_config;
    endpoint_t *driver_door_ep = contact_sensor::create(node, &driver_door_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(driver_door_ep != nullptr, ESP_LOGE(TAG, "Failed to create driver door contact sensor endpoint."));
    driver_door_endpoint_id = endpoint::get_id(driver_door_ep);
    ESP_LOGI(TAG, "Driver door contact sensor created with endpoint_id %d.", driver_door_endpoint_id);

    // PAX contact sensor covers front passenger door, sliding cargo
    // door, and rear cargo doors. All are wired to the same LED
    // signal on the T1N, so a single endpoint represents all of them.
    contact_sensor::config_t pax_door_config;
    endpoint_t *pax_door_ep = contact_sensor::create(node, &pax_door_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(pax_door_ep != nullptr, ESP_LOGE(TAG, "Failed to create PAX door contact sensor endpoint."));
    pax_door_endpoint_id = endpoint::get_id(pax_door_ep);
    ESP_LOGI(TAG, "PAX door contact sensor created with endpoint_id %d.", pax_door_endpoint_id);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    // Set OpenThread platform config
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif

    // Matter start
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter. Error: %d.", err));

#if CONFIG_ENABLE_ENCRYPTED_OTA
    err = esp_matter_ota_requestor_encrypted_init(s_decryption_key, s_decryption_key_len);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to initialize encrypted OTA. Error: %d.", err));
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
#if CONFIG_OPENTHREAD_CLI
    esp_matter::console::otcli_register_commands();
#endif
    esp_matter::console::init();
#endif

    reset_button_init();
}