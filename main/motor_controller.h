#pragma once

#include "esp_err.h"

enum class MotorMotion {
    Stop,
    Forward,
    Backward,
    Left,
    Right,
};

esp_err_t motor_controller_init();
void motor_controller_set_power(int left_percent, int right_percent);
void motor_controller_stop();
