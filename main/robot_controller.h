#pragma once

#include <cstdint>

#include "esp_err.h"
#include "motor_controller.h"

struct RobotSnapshot {
    MotorMotion motion;
    int left_power;
    int right_power;
};

esp_err_t robot_controller_start();
bool robot_controller_set_manual_command(MotorMotion motion, bool active,
                                         uint64_t hold_id);
bool robot_controller_set_joystick_command(int left_power, int right_power,
                                           bool active, uint64_t hold_id);
void robot_controller_emergency_stop();
RobotSnapshot robot_controller_snapshot();

const char *motor_motion_name(MotorMotion motion);
