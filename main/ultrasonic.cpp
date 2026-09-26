#include "ultrasonic.h"

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

esp_err_t ultrasonic_init()
{
    gpio_config_t output_config = {
        .pin_bit_mask = 1ULL << app_config::kUltrasonicTrig,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_config(&output_config);
    if (result != ESP_OK) {
        return result;
    }

    const gpio_config_t input_config = {
        .pin_bit_mask = 1ULL << app_config::kUltrasonicEcho,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    result = gpio_config(&input_config);
    gpio_set_level(app_config::kUltrasonicTrig, 0);
    return result;
}

float ultrasonic_measure_cm()
{
    gpio_set_level(app_config::kUltrasonicTrig, 0);
    esp_rom_delay_us(2);
    gpio_set_level(app_config::kUltrasonicTrig, 1);
    esp_rom_delay_us(10);
    gpio_set_level(app_config::kUltrasonicTrig, 0);

    const int64_t wait_started_us = esp_timer_get_time();
    while (gpio_get_level(app_config::kUltrasonicEcho) == 0) {
        if (esp_timer_get_time() - wait_started_us >=
            app_config::kUltrasonicTimeoutUs) {
            return -1.0F;
        }
    }

    const int64_t pulse_started_us = esp_timer_get_time();
    while (gpio_get_level(app_config::kUltrasonicEcho) != 0) {
        if (esp_timer_get_time() - pulse_started_us >=
            app_config::kUltrasonicTimeoutUs) {
            return -1.0F;
        }
    }

    const int64_t duration_us = esp_timer_get_time() - pulse_started_us;
    return static_cast<float>(duration_us) * 0.0343F / 2.0F;
}
