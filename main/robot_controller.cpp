#include "robot_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "telegram_client.h"
#include "telegram_messages.h"
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
    float obstacle_distance_cm = 20.0F;
    float follow_distance_cm = 20.0F;
    bool manual_front_brake_active = false;
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

bool apply_motion(MotorMotion motion, uint32_t expected_revision)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state.revision != expected_revision) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    motor_controller_drive(motion);
    s_state.applied_motion = motion;
    xSemaphoreGive(s_mutex);
    return true;
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

float filter_distance_sample(float distance, float samples[3],
                             uint32_t *sample_count, uint32_t *next_index)
{
    if (distance <= 0.0F) {
        return -1.0F;
    }
    samples[*next_index] = distance;
    *next_index = (*next_index + 1) % 3;
    *sample_count = std::min<uint32_t>(*sample_count + 1U, 3U);

    float sorted[3] = {};
    for (uint32_t index = 0; index < *sample_count; ++index) {
        sorted[index] = samples[index];
    }
    std::sort(sorted, sorted + *sample_count);
    if (*sample_count == 1) {
        return sorted[0];
    }
    if (*sample_count == 2) {
        return (sorted[0] + sorted[1]) / 2.0F;
    }
    return sorted[1];
}

bool set_manual_front_brake_active(bool active, uint32_t expected_revision)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state.revision != expected_revision) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    s_state.manual_front_brake_active = active;
    xSemaphoreGive(s_mutex);
    return true;
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
    bool manual_front_brake_active = false;
    uint32_t manual_low_distance_samples = 0;
    float follow_samples[3] = {};
    uint32_t follow_sample_count = 0;
    uint32_t follow_next_index = 0;
    float room_samples[3] = {};
    uint32_t room_sample_count = 0;
    uint32_t room_next_index = 0;
    float room_baseline_sum = 0.0F;
    uint32_t room_baseline_samples = 0;

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
            manual_front_brake_active = false;
            manual_low_distance_samples = 0;
            follow_sample_count = 0;
            follow_next_index = 0;
            room_sample_count = 0;
            room_next_index = 0;
            room_baseline_sum = 0.0F;
            room_baseline_samples = 0;
            set_manual_front_brake_active(false, revision);
            apply_motion(MotorMotion::Stop, revision);
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
                manual_front_brake_active = false;
                manual_low_distance_samples = 0;
                set_manual_front_brake_active(false, revision);
            }

            if (command != MotorMotion::Forward && manual_front_brake_active) {
                manual_front_brake_active = false;
                manual_low_distance_samples = 0;
                set_manual_front_brake_active(false, revision);
            }

            const uint32_t measurement_interval =
                command == MotorMotion::Forward
                    ? app_config::kUltrasonicMinIntervalMs
                    : app_config::kManualDistanceUpdateMs;

            if (last_measurement_ms == 0 ||
                now - last_measurement_ms >= measurement_interval) {
                const float distance = measure_and_publish();
                last_measurement_ms = now;

                if (command == MotorMotion::Forward && distance > 0.0F) {
                    if (manual_front_brake_active) {
                        if (distance >= app_config::kManualFrontBrakeReleaseCm) {
                            manual_front_brake_active = false;
                            manual_low_distance_samples = 0;
                        }
                    } else if (distance < app_config::kManualFrontBrakeDistanceCm) {
                        ++manual_low_distance_samples;
                        if (manual_low_distance_samples >=
                            app_config::kManualFrontBrakeSamples) {
                            manual_front_brake_active = true;
                            manual_low_distance_samples = 0;
                        }
                    } else {
                        manual_low_distance_samples = 0;
                    }
                } else if (command != MotorMotion::Forward) {
                    manual_front_brake_active = false;
                    manual_low_distance_samples = 0;
                }
                set_manual_front_brake_active(manual_front_brake_active, revision);
            }

            if (command == MotorMotion::Forward && manual_front_brake_active) {
                apply_motion(MotorMotion::Stop, revision);
            } else {
                apply_motion(command, revision);
            }
        } else if (mode == RobotMode::Obstacle) {
            switch (obstacle_phase) {
            case ObstaclePhase::Forward: {
                if (last_measurement_ms == 0 ||
                    now - last_measurement_ms >=
                        app_config::kUltrasonicMinIntervalMs) {
                    const float distance = measure_and_publish();
                    last_measurement_ms = now;
                    if (distance <= 0.0F) {
                        apply_motion(MotorMotion::Stop, revision);
                    } else if (distance < obstacle_distance) {
                        apply_motion(MotorMotion::Stop, revision);
                        obstacle_phase = ObstaclePhase::StopBeforeBack;
                        phase_started_ms = now;
                    } else {
                        apply_motion(MotorMotion::Forward, revision);
                    }
                }
                break;
            }
            case ObstaclePhase::StopBeforeBack:
                apply_motion(MotorMotion::Stop, revision);
                if (now - phase_started_ms >= app_config::kObstacleStopMs) {
                    obstacle_phase = ObstaclePhase::Backward;
                    phase_started_ms = now;
                }
                break;
            case ObstaclePhase::Backward:
                apply_motion(MotorMotion::Backward, revision);
                if (now - phase_started_ms >= app_config::kObstacleBackMs) {
                    obstacle_phase = ObstaclePhase::Turn;
                    phase_started_ms = now;
                }
                break;
            case ObstaclePhase::Turn:
                apply_motion(turn_left_next ? MotorMotion::RotateLeft
                                            : MotorMotion::RotateRight,
                             revision);
                if (now - phase_started_ms >= app_config::kObstacleTurnMs) {
                    apply_motion(MotorMotion::Stop, revision);
                    obstacle_phase = ObstaclePhase::Settle;
                    phase_started_ms = now;
                }
                break;
            case ObstaclePhase::Settle:
                apply_motion(MotorMotion::Stop, revision);
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
                const float filtered_distance = filter_distance_sample(
                    distance, follow_samples, &follow_sample_count,
                    &follow_next_index);
                if (filtered_distance <= 0.0F) {
                    apply_motion(MotorMotion::Stop, revision);
                } else if (filtered_distance < follow_distance) {
                    apply_motion(MotorMotion::Backward, revision);
                } else if (filtered_distance <= follow_distance + 5.0F) {
                    apply_motion(MotorMotion::Stop, revision);
                } else if (filtered_distance <= follow_distance + 25.0F) {
                    apply_motion(MotorMotion::Forward, revision);
                } else {
                    apply_motion(MotorMotion::Stop, revision);
                }
            }
        } else if (mode == RobotMode::RoomMonitor) {
            apply_motion(MotorMotion::Stop, revision);

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            const bool calibrating = s_state.room_calibrating;
            const bool intruder_detected = s_state.room_intruder_detected;
            const float baseline = s_state.room_baseline_cm;
            xSemaphoreGive(s_mutex);

            if (calibrating &&
                now - mode_started_ms >= app_config::kRoomCalibrationMs) {
                if (now - mode_started_ms >=
                    app_config::kRoomCalibrationTimeoutMs) {
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    if (s_state.revision == revision) {
                        s_state.mode = RobotMode::Manual;
                        s_state.command = MotorMotion::Stop;
                        reset_room_state_locked();
                        ++s_state.revision;
                    }
                    xSemaphoreGive(s_mutex);
                    telegram_client_enqueue_message(
                        telegram_messages::kRoomMonitorUnavailable);
                } else if (last_measurement_ms == 0 ||
                           now - last_measurement_ms >=
                               app_config::kUltrasonicMinIntervalMs) {
                    const float distance = measure_and_publish();
                    last_measurement_ms = now;
                    if (distance > 0.0F) {
                        room_baseline_sum += distance;
                        ++room_baseline_samples;
                    }
                    if (room_baseline_samples >=
                        app_config::kRoomBaselineSamples) {
                        const float calibrated_baseline =
                            room_baseline_sum / room_baseline_samples;
                        xSemaphoreTake(s_mutex, portMAX_DELAY);
                        if (s_state.revision == revision) {
                            s_state.room_baseline_cm = calibrated_baseline;
                            s_state.room_calibrating = false;
                        }
                        xSemaphoreGive(s_mutex);
                        char message[160];
                        telegram_messages::format_room_monitor_started(
                            message, sizeof(message), calibrated_baseline);
                        telegram_client_enqueue_message(message);
                    }
                }
            } else if (!calibrating &&
                       (last_measurement_ms == 0 ||
                        now - last_measurement_ms >= app_config::kRoomCheckMs)) {
                const float distance = measure_and_publish();
                last_measurement_ms = now;
                const float filtered_distance = filter_distance_sample(
                    distance, room_samples, &room_sample_count, &room_next_index);
                if (filtered_distance > 0.0F) {
                    const float change = baseline - filtered_distance;
                    if (change > app_config::kRoomChangeCm) {
                        room_clear_started_ms = 0;
                        if (!intruder_detected) {
                            xSemaphoreTake(s_mutex, portMAX_DELAY);
                            s_state.room_intruder_detected = true;
                            xSemaphoreGive(s_mutex);
                            char message[192];
                            telegram_messages::format_room_alert(
                                message, sizeof(message), filtered_distance);
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
    s_state.manual_front_brake_active = false;
    if (enabled && mode == RobotMode::RoomMonitor) {
        s_state.room_calibrating = true;
    }
    ++s_state.revision;
    motor_controller_stop();
    s_state.applied_motion = MotorMotion::Stop;
    xSemaphoreGive(s_mutex);
    if (stopped_room) {
        telegram_client_enqueue_message(telegram_messages::kRoomMonitorOff);
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
    s_state.manual_front_brake_active = false;
    ++s_state.revision;
    motor_controller_stop();
    s_state.applied_motion = MotorMotion::Stop;
    xSemaphoreGive(s_mutex);
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
        .manual_front_brake_active = s_state.manual_front_brake_active,
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
