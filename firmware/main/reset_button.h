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

// reset_button.h
//
// 10-second long-press of the XIAO BOOT button (GPIO9) triggers
// esp_matter::factory_reset(): wipes commissioned Matter state and
// reboots so the device can be re-commissioned.
//
// GPIO9 is dual-purpose: held LOW at power-up it puts the ESP32-C6
// into ROM serial bootloader mode. At runtime it's a normal input
// with internal pull-up.
//
// TODO: visual feedback during the press (LED blink accelerates near
// the 10s threshold).

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Must be called after esp_matter::start() since factory_reset()
// requires Matter to be initialized.
esp_err_t reset_button_init(void);

#ifdef __cplusplus
}
#endif