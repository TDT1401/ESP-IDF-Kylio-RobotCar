#include "motor_controller.h"

#include "app_config.h"
#include "driver/gpio.h"

namespace {

MotorMotion s_last_motion = MotorMotion::Stop;
bool s_motion_initialized = false;

void set_channel(gpio_num_t pin_a, gpio_num_t pin_b, int direction)
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

void set_motors(int left, int right)
{
    set_channel(app_config::kLeftIn1, app_config::kLeftIn2, left);
    set_channel(app_config::kRightIn3, app_config::kRightIn4, right);
}

}  // namespace

esp_err_t motor_controller_init()
{
    const uint64_t pin_mask =
        (1ULL << app_config::kLeftIn1) |
        (1ULL << app_config::kLeftIn2) |
        (1ULL << app_config::kRightIn3) |
        (1ULL << app_config::kRightIn4);

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
    if (s_motion_initialized && motion == s_last_motion) {
        return;
    }

    switch (motion) {
    case MotorMotion::Forward:
        set_motors(1, 1);
        break;
    case MotorMotion::Backward:
        set_motors(-1, -1);
        break;
    case MotorMotion::Left:
        set_motors(-1, 1);
        break;
    case MotorMotion::Right:
        set_motors(1, -1);
        break;
    case MotorMotion::Stop:
    default:
        set_motors(0, 0);
        break;
    }
    s_last_motion = motion;
    s_motion_initialized = true;
}

void motor_controller_stop()
{
    motor_controller_drive(MotorMotion::Stop);
}
