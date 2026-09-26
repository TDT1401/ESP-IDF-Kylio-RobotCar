#pragma once

#include "esp_err.h"

enum class MotorMotion {
    Stop,
    Forward,
    Backward,
    Left,
    Right,
    ForwardLeft,
    ForwardRight,
    BackLeft,
    BackRight,
    RotateLeft,
    RotateRight,
};

esp_err_t motor_controller_init();
void motor_controller_drive(MotorMotion motion);
void motor_controller_stop();
