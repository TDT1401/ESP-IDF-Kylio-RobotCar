#pragma once

#include <string>

#include "esp_err.h"

esp_err_t wifi_manager_start();
bool wifi_manager_is_connected();
std::string wifi_manager_ip_address();
int wifi_manager_rssi();
