#pragma once

#include "esp_err.h"

namespace wifi_manager {

// Connect using saved NVS settings. If none exist or the connection times out,
// this function starts the AI-Companion-Setup access point and setup website.
esp_err_t Initialize();
void WaitUntilConnected();
const char* ServerUrl();

}  // namespace wifi_manager
