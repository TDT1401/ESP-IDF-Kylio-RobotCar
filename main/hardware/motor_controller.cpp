#include "motor_controller.h"

#include <algorithm>
#include <cstdlib>

#include "config/app_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_rom_sys.h"

namespace {

constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
constexpr uint32_t kPwmMaxDuty = 255;
constexpr uint32_t kPwmFrequencyHz = 5000;

bool s_power_initialized = false;
int s_left_power = 0;
int s_right_power = 0;

uint32_t duty_for(int percent)
{
    return static_cast<uint32_t>((std::abs(percent) * kPwmMaxDuty + 50) / 100);
}

void set_pwm_duty(ledc_channel_t channel, uint32_t duty)
{
    ESP_ERROR_CHECK(ledc_set_duty(kSpeedMode, channel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(kSpeedMode, channel));
}

void set_side(ledc_channel_t enable_channel, gpio_num_t forward_pin,
              gpio_num_t reverse_pin, int previous_power, int power)
{
    // Disable the bridge before stopping or changing direction.
    const bool reversing = (previous_power > 0 && power < 0) ||
                           (previous_power < 0 && power > 0);
    if (power == 0 || reversing) {
        set_pwm_duty(enable_channel, 0);
        ESP_ERROR_CHECK(gpio_set_level(forward_pin, 0));
        ESP_ERROR_CHECK(gpio_set_level(reverse_pin, 0));
        if (reversing) {
            // LEDC duty updates on the next PWM cycle.
            esp_rom_delay_us(1000000U / kPwmFrequencyHz);
        }
    }
    if (power != 0) {
        if (previous_power == 0 || reversing) {
            ESP_ERROR_CHECK(gpio_set_level(reverse_pin, power < 0));
            ESP_ERROR_CHECK(gpio_set_level(forward_pin, power > 0));
        }
        set_pwm_duty(enable_channel, duty_for(power));
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
        (1ULL << app_config::kRightIn4) |
        (1ULL << app_config::kLeftEnable) |
        (1ULL << app_config::kRightEnable);

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
    gpio_set_level(app_config::kLeftEnable, 0);
    gpio_set_level(app_config::kRightEnable, 0);

    ledc_timer_config_t timer = {};
    timer.speed_mode = kSpeedMode;
    timer.duty_resolution = LEDC_TIMER_8_BIT;
    timer.timer_num = kTimer;
    timer.freq_hz = kPwmFrequencyHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    result = ledc_timer_config(&timer);
    if (result != ESP_OK) return result;

    result = configure_channel(LEDC_CHANNEL_0, app_config::kLeftEnable);
    if (result != ESP_OK) return result;
    result = configure_channel(LEDC_CHANNEL_1, app_config::kRightEnable);
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
        set_side(LEDC_CHANNEL_0, app_config::kLeftIn2, app_config::kLeftIn1,
                 s_power_initialized ? s_left_power : 0, left);
    }
    if (!s_power_initialized || right != s_right_power) {
        set_side(LEDC_CHANNEL_1, app_config::kRightIn4, app_config::kRightIn3,
                 s_power_initialized ? s_right_power : 0, right);
    }
    s_left_power = left;
    s_right_power = right;
    s_power_initialized = true;
}

void motor_controller_stop()
{
    motor_controller_set_power(0, 0);
}
