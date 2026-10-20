#include "motor_controller.h"

#include <algorithm>
#include <cstdlib>

#include "app_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

namespace {

constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
constexpr uint32_t kPwmPeriod = 256;
constexpr uint32_t kPwmFrequencyHz = 5000;

bool s_power_initialized = false;
int s_left_power = 0;
int s_right_power = 0;

uint32_t duty_for(int percent)
{
    return static_cast<uint32_t>((std::abs(percent) * kPwmPeriod + 50) / 100);
}

void set_channel(ledc_channel_t forward_pin, ledc_channel_t reverse_pin,
                 int power)
{
    // Motor writes are serialized by the robot controller mutex. Clear the
    // opposite input before enabling the new direction.
    if (power >= 0) {
        ESP_ERROR_CHECK(ledc_set_duty(kSpeedMode, reverse_pin, 0));
        ESP_ERROR_CHECK(ledc_update_duty(kSpeedMode, reverse_pin));
        ESP_ERROR_CHECK(ledc_set_duty(kSpeedMode, forward_pin, duty_for(power)));
        ESP_ERROR_CHECK(ledc_update_duty(kSpeedMode, forward_pin));
    } else {
        ESP_ERROR_CHECK(ledc_set_duty(kSpeedMode, forward_pin, 0));
        ESP_ERROR_CHECK(ledc_update_duty(kSpeedMode, forward_pin));
        ESP_ERROR_CHECK(ledc_set_duty(kSpeedMode, reverse_pin, duty_for(power)));
        ESP_ERROR_CHECK(ledc_update_duty(kSpeedMode, reverse_pin));
    }
}

esp_err_t configure_channel(ledc_channel_t channel, gpio_num_t pin)
{
    ledc_channel_config_t config = {};
    config.gpio_num = pin;
    config.speed_mode = kSpeedMode;
    config.channel = channel;
    config.timer_sel = kTimer;
    config.duty = 0;
    config.hpoint = 0;
    return ledc_channel_config(&config);
}

}  // namespace

esp_err_t motor_controller_init()
{
    const uint64_t pin_mask =
        (1ULL << app_config::kLeftIn1) |
        (1ULL << app_config::kLeftIn2) |
        (1ULL << app_config::kRightIn3) |
        (1ULL << app_config::kRightIn4);

    const gpio_config_t gpio = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_config(&gpio);
    if (result != ESP_OK) return result;
    gpio_set_level(app_config::kLeftIn1, 0);
    gpio_set_level(app_config::kLeftIn2, 0);
    gpio_set_level(app_config::kRightIn3, 0);
    gpio_set_level(app_config::kRightIn4, 0);

    ledc_timer_config_t timer = {};
    timer.speed_mode = kSpeedMode;
    timer.duty_resolution = LEDC_TIMER_8_BIT;
    timer.timer_num = kTimer;
    timer.freq_hz = kPwmFrequencyHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    result = ledc_timer_config(&timer);
    if (result != ESP_OK) return result;

    result = configure_channel(LEDC_CHANNEL_0, app_config::kLeftIn2);
    if (result != ESP_OK) return result;
    result = configure_channel(LEDC_CHANNEL_1, app_config::kLeftIn1);
    if (result != ESP_OK) return result;
    result = configure_channel(LEDC_CHANNEL_2, app_config::kRightIn4);
    if (result != ESP_OK) return result;
    result = configure_channel(LEDC_CHANNEL_3, app_config::kRightIn3);
    if (result != ESP_OK) return result;

    s_power_initialized = false;
    motor_controller_stop();
    return ESP_OK;
}

void motor_controller_set_power(int left_percent, int right_percent)
{
    const int left = std::clamp(left_percent, -100, 100);
    const int right = std::clamp(right_percent, -100, 100);
    if (!s_power_initialized || left != s_left_power) {
        set_channel(LEDC_CHANNEL_0, LEDC_CHANNEL_1, left);
    }
    if (!s_power_initialized || right != s_right_power) {
        set_channel(LEDC_CHANNEL_2, LEDC_CHANNEL_3, right);
    }
    s_left_power = left;
    s_right_power = right;
    s_power_initialized = true;
}

void motor_controller_stop()
{
    motor_controller_set_power(0, 0);
}
