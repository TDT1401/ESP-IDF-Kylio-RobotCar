#include "robot_controller.h"

#include <cstdint>

#include "app_config.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

struct ControllerState {
    MotorMotion command = MotorMotion::Stop;
    MotorMotion applied_motion = MotorMotion::Stop;
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

void controller_task(void *)
{
    while (true) {
        const uint64_t now = now_ms();
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_state.command != MotorMotion::Stop &&
            now - s_state.command_updated_ms >
                app_config::kManualCommandTimeoutMs) {
            s_state.blocked_hold_id = s_state.active_hold_id;
            s_state.active_hold_id = 0;
            s_state.command = MotorMotion::Stop;
        }
        motor_controller_drive(s_state.command);
        s_state.applied_motion = s_state.command;
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
    if (active) {
        if (hold_id == 0 || hold_id == s_state.blocked_hold_id) {
            xSemaphoreGive(s_mutex);
            return false;
        }
        s_state.command = motion;
        s_state.active_hold_id = hold_id;
        s_state.command_updated_ms = now_ms();
    } else if (s_state.active_hold_id == hold_id &&
               (s_state.command == motion || motion == MotorMotion::Stop)) {
        s_state.command = MotorMotion::Stop;
        s_state.active_hold_id = 0;
        motor_controller_stop();
        s_state.applied_motion = MotorMotion::Stop;
    }
    xSemaphoreGive(s_mutex);
    return true;
}

void robot_controller_emergency_stop()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state.active_hold_id != 0) {
        s_state.blocked_hold_id = s_state.active_hold_id;
    }
    s_state.active_hold_id = 0;
    s_state.command = MotorMotion::Stop;
    motor_controller_stop();
    s_state.applied_motion = MotorMotion::Stop;
    xSemaphoreGive(s_mutex);
}

RobotSnapshot robot_controller_snapshot()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const RobotSnapshot snapshot = {
        .motion = s_state.applied_motion,
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
