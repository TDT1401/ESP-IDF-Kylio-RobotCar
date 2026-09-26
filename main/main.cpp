#include "esp_err.h"
#include "esp_log.h"
#include "motor_controller.h"
#include "nvs_flash.h"
#include "robot_controller.h"
#include "telegram_client.h"
#include "ultrasonic.h"
#include "web_server.h"
#include "wifi_manager.h"

namespace {

constexpr const char *kTag = "main";

void initialize_nvs()
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

}  // namespace

extern "C" void app_main(void)
{
    initialize_nvs();

    // Initialize motors first so every control pin is LOW before tasks start.
    ESP_ERROR_CHECK(motor_controller_init());
    ESP_ERROR_CHECK(ultrasonic_init());
    ESP_ERROR_CHECK(robot_controller_start());

    const esp_err_t wifi_result = wifi_manager_start();
    if (wifi_result != ESP_OK) {
        ESP_LOGE(kTag, "Wi-Fi did not start: %s", esp_err_to_name(wifi_result));
    }

    ESP_ERROR_CHECK(telegram_client_start());
    ESP_ERROR_CHECK(web_server_start());
    ESP_LOGI(kTag, "Kylio firmware initialized");
}
