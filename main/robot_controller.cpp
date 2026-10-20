#include "robot_controller.h"

#include <algorithm>
#include <cstdint>

#include "app_config.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr int kJoystickSlewPerTick = 10;

enum class CommandMode { Manual, Joystick };

struct ControllerState {
    int target_left = 0;
    int target_right = 0;
    int applied_left = 0;
    int applied_right = 0;
    MotorMotion manual_motion = MotorMotion::Stop;
    CommandMode mode = CommandMode::Manual;
    uint64_t command_updated_ms = 0;
    uint64_t active_hold_id = 0;
    uint64_t blocked_hold_id = 0;
};

ControllerState s_state;
SemaphoreHandle_t s_mutex = nullptr;

uint64_t now_ms()
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

void stop_locked()
{
    if (s_state.active_hold_id != 0) {
        s_state.blocked_hold_id = s_state.active_hold_id;
    }
    s_state.active_hold_id = 0;
    s_state.target_left = 0;
    s_state.target_right = 0;
    s_state.applied_left = 0;
    s_state.applied_right = 0;
    s_state.manual_motion = MotorMotion::Stop;
    motor_controller_stop();
}

bool begin_or_refresh_hold(uint64_t hold_id)
{
    if (hold_id == 0 || hold_id == s_state.blocked_hold_id) {
        return false;
    }
    if (s_state.active_hold_id != 0 && s_state.active_hold_id != hold_id) {
        s_state.blocked_hold_id = s_state.active_hold_id;
    }
    s_state.active_hold_id = hold_id;
    s_state.command_updated_ms = now_ms();
    return true;
}

int approach(int current, int target)
{
    return current + std::clamp(target - current,
                                -kJoystickSlewPerTick, kJoystickSlewPerTick);
}

MotorMotion motion_from_power(int left, int right)
{
    if (left == 0 && right == 0) return MotorMotion::Stop;
    if (left > 0 && right > 0) return MotorMotion::Forward;
    if (left < 0 && right < 0) return MotorMotion::Backward;
    return left < right ? MotorMotion::Left : MotorMotion::Right;
}

void controller_task(void *)
{
    while (true) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_state.active_hold_id != 0 &&
            now_ms() - s_state.command_updated_ms >
                app_config::kManualCommandTimeoutMs) {
            stop_locked();
        } else {
            if (s_state.mode == CommandMode::Joystick) {
                s_state.applied_left = approach(s_state.applied_left,
                                                s_state.target_left);
                s_state.applied_right = approach(s_state.applied_right,
                                                 s_state.target_right);
            } else {
                s_state.applied_left = s_state.target_left;
                s_state.applied_right = s_state.target_right;
            }
            motor_controller_set_power(s_state.applied_left,
                                       s_state.applied_right);
        }
        xSemaphoreGive(s_mutex);
        vTaskDelay(pdMS_TO_TICKS(app_config::kControllerPeriodMs));
    }
}

}  // namespace

esp_err_t robot_controller_start()
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreate(controller_task, "robot_controller", 4096, nullptr,
                       6, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool robot_controller_set_manual_command(MotorMotion motion, bool active,
                                         uint64_t hold_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!active) {
        if (s_state.active_hold_id == hold_id &&
            s_state.mode == CommandMode::Manual &&
            s_state.manual_motion == motion) {
            stop_locked();
        }
        xSemaphoreGive(s_mutex);
        return true;
    }
    if (!begin_or_refresh_hold(hold_id)) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    s_state.mode = CommandMode::Manual;
    s_state.manual_motion = motion;
    switch (motion) {
    case MotorMotion::Forward:
        s_state.target_left = 100;
        s_state.target_right = 100;
        break;
    case MotorMotion::Backward:
        s_state.target_left = -100;
        s_state.target_right = -100;
        break;
    case MotorMotion::Left:
        s_state.target_left = -100;
        s_state.target_right = 100;
        break;
    case MotorMotion::Right:
        s_state.target_left = 100;
        s_state.target_right = -100;
        break;
    case MotorMotion::Stop:
    default:
        stop_locked();
        break;
    }
    xSemaphoreGive(s_mutex);
    return true;
}

bool robot_controller_set_joystick_command(int left_power, int right_power,
                                           bool active, uint64_t hold_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!active) {
        if (s_state.active_hold_id == hold_id &&
            s_state.mode == CommandMode::Joystick) {
            stop_locked();
        }
        xSemaphoreGive(s_mutex);
        return true;
    }
    if (!begin_or_refresh_hold(hold_id)) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    s_state.mode = CommandMode::Joystick;
    s_state.target_left = std::clamp(left_power, -100, 100);
    s_state.target_right = std::clamp(right_power, -100, 100);
    xSemaphoreGive(s_mutex);
    return true;
}

void robot_controller_emergency_stop()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    stop_locked();
    xSemaphoreGive(s_mutex);
}

RobotSnapshot robot_controller_snapshot()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const RobotSnapshot snapshot = {
        .motion = motion_from_power(s_state.applied_left, s_state.applied_right),
        .left_power = s_state.applied_left,
        .right_power = s_state.applied_right,
    };
    xSemaphoreGive(s_mutex);
    return snapshot;
}

const char *motor_motion_name(MotorMotion motion)
{
    switch (motion) {
    case MotorMotion::Forward: return "forward";
    case MotorMotion::Backward: return "backward";
    case MotorMotion::Left: return "left";
    case MotorMotion::Right: return "right";
    case MotorMotion::Stop:
    default: return "stopped";
    }
}
