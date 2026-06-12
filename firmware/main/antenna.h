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

// antenna.h
//
// External antenna selection for the XIAO ESP32-C6.
//
// The module has an onboard RF switch controlled by GPIO3 and GPIO14
// that routes the radio between the built-in ceramic chip antenna
// (default) and the external U.FL connector. This file routes RF to
// the external antenna, and must run before any radio activity
// (Wi-Fi, BLE, Thread) starts; in practice, before esp_matter::start().

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configure GPIO3 and GPIO14 to route RF to the external antenna.
// Must be called before esp_matter::start() so the antenna is
// selected before the radio comes up.
esp_err_t antenna_init(void);

#ifdef __cplusplus
}
#endif