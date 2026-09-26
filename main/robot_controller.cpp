#include "robot_controller.h"

#include <cmath>
#include <cstdio>

#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "telegram_client.h"
#include "ultrasonic.h"

namespace {

constexpr const char *kTag = "robot";

struct ControllerState {
    RobotMode mode = RobotMode::Manual;
    MotorMotion command = MotorMotion::Stop;
    MotorMotion applied_motion = MotorMotion::Stop;
    uint64_t command_updated_ms = 0;
    uint32_t revision = 0;
    float distance_cm = -1.0F;
    float obstacle_distance_cm = 30.0F;
    float follow_distance_cm = 30.0F;
    bool room_calibrating = false;
    bool room_intruder_detected = false;
    float room_baseline_cm = -1.0F;
};

enum class ObstaclePhase {
    Forward,
    StopBeforeBack,
    Backward,
    Turn,
    Settle,
};

ControllerState s_state;
SemaphoreHandle_t s_mutex = nullptr;

uint64_t now_ms()
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

void apply_motion(MotorMotion motion)
{
    motor_controller_drive(motion);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.applied_motion = motion;
    xSemaphoreGive(s_mutex);
}

float measure_and_publish()
{
    const float distance = ultrasonic_measure_cm();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.distance_cm = distance;
    xSemaphoreGive(s_mutex);
    return distance;
}

void reset_room_state_locked()
{
    s_state.room_calibrating = false;
    s_state.room_intruder_detected = false;
    s_state.room_baseline_cm = -1.0F;
}

void controller_task(void *)
{
    uint32_t observed_revision = UINT32_MAX;
    ObstaclePhase obstacle_phase = ObstaclePhase::Forward;
    bool turn_left_next = true;
    uint64_t phase_started_ms = now_ms();
    uint64_t mode_started_ms = phase_started_ms;
    uint64_t last_measurement_ms = 0;
    uint64_t room_clear_started_ms = 0;

    while (true) {
        const uint64_t now = now_ms();

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        RobotMode mode = s_state.mode;
        MotorMotion command = s_state.command;
        const uint64_t command_updated_ms = s_state.command_updated_ms;
        const uint32_t revision = s_state.revision;
        const float obstacle_distance = s_state.obstacle_distance_cm;
        const float follow_distance = s_state.follow_distance_cm;
        xSemaphoreGive(s_mutex);

        if (revision != observed_revision) {
            observed_revision = revision;
            obstacle_phase = ObstaclePhase::Forward;
            phase_started_ms = now;
            mode_started_ms = now;
            last_measurement_ms = 0;
            room_clear_started_ms = 0;
            apply_motion(MotorMotion::Stop);
        }

        if (mode == RobotMode::Manual) {
            if (command != MotorMotion::Stop &&
                now - command_updated_ms > app_config::kManualCommandTimeoutMs) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                if (s_state.mode == RobotMode::Manual &&
                    s_state.command == command) {
                    s_state.command = MotorMotion::Stop;
                }
                xSemaphoreGive(s_mutex);
                command = MotorMotion::Stop;
            }

            if (command == MotorMotion::Forward) {
                if (last_measurement_ms == 0 ||
                    now - last_measurement_ms >=
                        app_config::kUltrasonicMinIntervalMs) {
                    const float distance = measure_and_publish();
                    last_measurement_ms = now;
                    if (distance <= 0.0F ||
                        distance < app_config::kManualStopDistanceCm) {
                        command = MotorMotion::Stop;
                    }
                } else {
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    const float distance = s_state.distance_cm;
                    xSemaphoreGive(s_mutex);
                    if (distance <= 0.0F ||
                        distance < app_config::kManualStopDistanceCm) {
                        command = MotorMotion::Stop;
                    }
                }
            }
            apply_motion(command);
        } else if (mode == RobotMode::Obstacle) {
            switch (obstacle_phase) {
            case ObstaclePhase::Forward: {
                if (last_measurement_ms == 0 ||
                    now - last_measurement_ms >=
                        app_config::kUltrasonicMinIntervalMs) {
                    const float distance = measure_and_publish();
                    last_measurement_ms = now;
                    if (distance <= 0.0F) {
                        apply_motion(MotorMotion::Stop);
                    } else if (distance < obstacle_distance) {
                        apply_motion(MotorMotion::Stop);
                        obstacle_phase = ObstaclePhase::StopBeforeBack;
                        phase_started_ms = now;
                    } else {
                        apply_motion(MotorMotion::Forward);
                    }
                }
                break;
            }
            case ObstaclePhase::StopBeforeBack:
                apply_motion(MotorMotion::Stop);
                if (now - phase_started_ms >= app_config::kObstacleStopMs) {
                    obstacle_phase = ObstaclePhase::Backward;
                    phase_started_ms = now;
                }
                break;
            case ObstaclePhase::Backward:
                apply_motion(MotorMotion::Backward);
                if (now - phase_started_ms >= app_config::kObstacleBackMs) {
                    obstacle_phase = ObstaclePhase::Turn;
                    phase_started_ms = now;
                }
                break;
            case ObstaclePhase::Turn:
                apply_motion(turn_left_next ? MotorMotion::Left
                                            : MotorMotion::Right);
                if (now - phase_started_ms >= app_config::kObstacleTurnMs) {
                    apply_motion(MotorMotion::Stop);
                    obstacle_phase = ObstaclePhase::Settle;
                    phase_started_ms = now;
                }
                break;
            case ObstaclePhase::Settle:
                apply_motion(MotorMotion::Stop);
                if (now - phase_started_ms >= app_config::kObstacleStopMs) {
                    turn_left_next = !turn_left_next;
                    obstacle_phase = ObstaclePhase::Forward;
                    last_measurement_ms = 0;
                }
                break;
            }
        } else if (mode == RobotMode::Follow) {
            if (last_measurement_ms == 0 ||
                now - last_measurement_ms >=
                    app_config::kUltrasonicMinIntervalMs) {
                const float distance = measure_and_publish();
                last_measurement_ms = now;
                if (distance <= 0.0F) {
                    apply_motion(MotorMotion::Stop);
                } else if (distance < follow_distance) {
                    apply_motion(MotorMotion::Backward);
                } else if (distance <= follow_distance + 5.0F) {
                    apply_motion(MotorMotion::Stop);
                } else if (distance <= follow_distance + 25.0F) {
                    apply_motion(MotorMotion::Forward);
                } else {
                    apply_motion(MotorMotion::Stop);
                }
            }
        } else if (mode == RobotMode::RoomMonitor) {
            apply_motion(MotorMotion::Stop);

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            const bool calibrating = s_state.room_calibrating;
            const bool intruder_detected = s_state.room_intruder_detected;
            const float baseline = s_state.room_baseline_cm;
            xSemaphoreGive(s_mutex);

            if (calibrating &&
                now - mode_started_ms >= app_config::kRoomCalibrationMs) {
                const float distance = measure_and_publish();
                last_measurement_ms = now;
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                if (distance > 0.0F) {
                    s_state.room_baseline_cm = distance;
                    s_state.room_calibrating = false;
                } else {
                    s_state.mode = RobotMode::Manual;
                    s_state.command = MotorMotion::Stop;
                    reset_room_state_locked();
                    ++s_state.revision;
                }
                xSemaphoreGive(s_mutex);

                if (distance > 0.0F) {
                    char message[160];
                    std::snprintf(message, sizeof(message),
                                  "Room Monitor ON\nKhoang cach ban dau: %.1f cm",
                                  distance);
                    telegram_client_enqueue_message(message);
                } else {
                    telegram_client_enqueue_message(
                        "Khong the bat Room Monitor: cam bien sieu am khong phan hoi.");
                }
            } else if (!calibrating &&
                       (last_measurement_ms == 0 ||
                        now - last_measurement_ms >= app_config::kRoomCheckMs)) {
                const float distance = measure_and_publish();
                last_measurement_ms = now;
                if (distance > 0.0F) {
                    const float change = baseline - distance;
                    if (change > app_config::kRoomChangeCm) {
                        room_clear_started_ms = 0;
                        if (!intruder_detected) {
                            xSemaphoreTake(s_mutex, portMAX_DELAY);
                            s_state.room_intruder_detected = true;
                            xSemaphoreGive(s_mutex);
                            char message[192];
                            std::snprintf(message, sizeof(message),
                                          "CANH BAO! Phat hien thay doi trong phong. "
                                          "Khoang cach: %.1f cm",
                                          distance);
                            telegram_client_enqueue_message(message);
                        }
                    } else if (intruder_detected) {
                        if (room_clear_started_ms == 0) {
                            room_clear_started_ms = now;
                        } else if (now - room_clear_started_ms >=
                                   app_config::kRoomClearMs) {
                            xSemaphoreTake(s_mutex, portMAX_DELAY);
                            s_state.room_intruder_detected = false;
                            xSemaphoreGive(s_mutex);
                            room_clear_started_ms = 0;
                            ESP_LOGI(kTag, "Room clear; monitor re-armed");
                        }
                    }
                }
            }
        }

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

    const BaseType_t created = xTaskCreate(
        controller_task, "robot_controller", 4096, nullptr, 6, nullptr);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void robot_controller_set_mode(RobotMode mode, bool enabled)
{
    bool stopped_room = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    stopped_room = !enabled && s_state.mode == RobotMode::RoomMonitor;
    s_state.mode = enabled ? mode : RobotMode::Manual;
    s_state.command = MotorMotion::Stop;
    reset_room_state_locked();
    if (enabled && mode == RobotMode::RoomMonitor) {
        s_state.room_calibrating = true;
    }
    ++s_state.revision;
    xSemaphoreGive(s_mutex);

    motor_controller_stop();
    if (stopped_room) {
        telegram_client_enqueue_message("Room Monitor OFF");
    }
}

bool robot_controller_set_manual_command(MotorMotion motion, bool active)
{
    bool accepted = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state.mode == RobotMode::Manual) {
        if (active) {
            s_state.command = motion;
            s_state.command_updated_ms = now_ms();
        } else if (s_state.command == motion || motion == MotorMotion::Stop) {
            s_state.command = MotorMotion::Stop;
        }
        accepted = true;
    }
    xSemaphoreGive(s_mutex);
    return accepted;
}

void robot_controller_emergency_stop()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.mode = RobotMode::Manual;
    s_state.command = MotorMotion::Stop;
    reset_room_state_locked();
    ++s_state.revision;
    xSemaphoreGive(s_mutex);
    motor_controller_stop();
}

bool robot_controller_set_follow_distance(float distance_cm)
{
    if (!std::isfinite(distance_cm) || distance_cm < 10.0F ||
        distance_cm > 200.0F) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.follow_distance_cm = distance_cm;
    xSemaphoreGive(s_mutex);
    return true;
}

bool robot_controller_set_obstacle_distance(float distance_cm)
{
    if (!std::isfinite(distance_cm) || distance_cm < 10.0F ||
        distance_cm > 200.0F) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.obstacle_distance_cm = distance_cm;
    xSemaphoreGive(s_mutex);
    return true;
}

RobotSnapshot robot_controller_snapshot()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const RobotSnapshot snapshot = {
        .mode = s_state.mode,
        .motion = s_state.applied_motion,
        .distance_cm = s_state.distance_cm,
        .distance_valid = s_state.distance_cm > 0.0F &&
                          s_state.distance_cm <= 400.0F,
        .obstacle_distance_cm = s_state.obstacle_distance_cm,
        .follow_distance_cm = s_state.follow_distance_cm,
        .room_calibrating = s_state.room_calibrating,
        .room_intruder_detected = s_state.room_intruder_detected,
        .room_baseline_cm = s_state.room_baseline_cm,
    };
    xSemaphoreGive(s_mutex);
    return snapshot;
}

const char *robot_mode_name(RobotMode mode)
{
    switch (mode) {
    case RobotMode::Obstacle:
        return "obstacle";
    case RobotMode::Follow:
        return "follow";
    case RobotMode::RoomMonitor:
        return "room";
    case RobotMode::Manual:
    default:
        return "manual";
    }
}

const char *motor_motion_name(MotorMotion motion)
{
    switch (motion) {
    case MotorMotion::Forward: return "forward";
    case MotorMotion::Backward: return "backward";
    case MotorMotion::Left: return "left";
    case MotorMotion::Right: return "right";
    case MotorMotion::ForwardLeft: return "forward-left";
    case MotorMotion::ForwardRight: return "forward-right";
    case MotorMotion::BackLeft: return "back-left";
    case MotorMotion::BackRight: return "back-right";
    case MotorMotion::RotateLeft: return "rotate-left";
    case MotorMotion::RotateRight: return "rotate-right";
    case MotorMotion::Stop:
    default:
        return "stopped";
    }
}
