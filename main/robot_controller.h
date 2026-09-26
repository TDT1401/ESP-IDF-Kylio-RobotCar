#pragma once

#include <cstdint>

#include "esp_err.h"
#include "motor_controller.h"

enum class RobotMode {
    Manual,
    Obstacle,
    Follow,
    RoomMonitor,
};

struct RobotSnapshot {
    RobotMode mode;
    MotorMotion motion;
    float distance_cm;
    bool distance_valid;
    float obstacle_distance_cm;
    float follow_distance_cm;
    bool manual_front_brake_active;
    bool room_calibrating;
    bool room_intruder_detected;
    float room_baseline_cm;
};

esp_err_t robot_controller_start();
void robot_controller_set_mode(RobotMode mode, bool enabled);
bool robot_controller_set_manual_command(MotorMotion motion, bool active);
void robot_controller_emergency_stop();
bool robot_controller_set_follow_distance(float distance_cm);
bool robot_controller_set_obstacle_distance(float distance_cm);
RobotSnapshot robot_controller_snapshot();

const char *robot_mode_name(RobotMode mode);
const char *motor_motion_name(MotorMotion motion);
