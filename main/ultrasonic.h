#pragma once

#include "esp_err.h"

esp_err_t ultrasonic_init();

// Returns a distance in centimetres, or a negative value on timeout/error.
float ultrasonic_measure_cm();
