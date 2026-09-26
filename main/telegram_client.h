#pragma once

#include "esp_err.h"

esp_err_t telegram_client_start();
bool telegram_client_is_configured();
bool telegram_client_is_enabled();
void telegram_client_set_enabled(bool enabled);
bool telegram_client_enqueue_message(const char *message);
