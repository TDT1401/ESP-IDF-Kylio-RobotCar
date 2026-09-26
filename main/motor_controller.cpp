#include "motor_controller.h"

#include "app_config.h"
#include "driver/gpio.h"

namespace {

void set_wheel(gpio_num_t pin_a, gpio_num_t pin_b, int direction)
{
    if (direction > 0) {
        gpio_set_level(pin_a, 0);
        gpio_set_level(pin_b, 1);
    } else if (direction < 0) {
        gpio_set_level(pin_a, 1);
        gpio_set_level(pin_b, 0);
    } else {
        gpio_set_level(pin_a, 0);
        gpio_set_level(pin_b, 0);
    }
}

void set_motors(int front_left, int front_right, int back_left, int back_right)
{
    set_wheel(app_config::kFrontLeftA, app_config::kFrontLeftB, front_left);
    set_wheel(app_config::kFrontRightA, app_config::kFrontRightB, front_right);
    set_wheel(app_config::kBackLeftA, app_config::kBackLeftB, back_left);
    set_wheel(app_config::kBackRightA, app_config::kBackRightB, back_right);
}

}  // namespace

esp_err_t motor_controller_init()
{
    const uint64_t pin_mask =
        (1ULL << app_config::kFrontLeftA) |
        (1ULL << app_config::kFrontLeftB) |
        (1ULL << app_config::kFrontRightA) |
        (1ULL << app_config::kFrontRightB) |
        (1ULL << app_config::kBackLeftA) |
        (1ULL << app_config::kBackLeftB) |
        (1ULL << app_config::kBackRightA) |
        (1ULL << app_config::kBackRightB);

    const gpio_config_t config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    const esp_err_t result = gpio_config(&config);
    if (result == ESP_OK) {
        motor_controller_stop();
    }
    return result;
}

void motor_controller_drive(MotorMotion motion)
{
    switch (motion) {
    case MotorMotion::Forward:
        set_motors(1, 1, 1, 1);
        break;
    case MotorMotion::Backward:
        set_motors(-1, -1, -1, -1);
        break;
    case MotorMotion::Left:
        set_motors(-1, 1, 1, -1);
        break;
    case MotorMotion::Right:
        set_motors(1, -1, -1, 1);
        break;
    case MotorMotion::ForwardLeft:
        set_motors(0, 1, 1, 0);
        break;
    case MotorMotion::ForwardRight:
        set_motors(1, 0, 0, 1);
        break;
    case MotorMotion::BackLeft:
        set_motors(-1, 0, 0, -1);
        break;
    case MotorMotion::BackRight:
        set_motors(0, -1, -1, 0);
        break;
    case MotorMotion::RotateLeft:
        set_motors(-1, 1, -1, 1);
        break;
    case MotorMotion::RotateRight:
        set_motors(1, -1, 1, -1);
        break;
    case MotorMotion::Stop:
    default:
        set_motors(0, 0, 0, 0);
        break;
    }
}

void motor_controller_stop()
{
    set_motors(0, 0, 0, 0);
}
