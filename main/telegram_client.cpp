#include "telegram_client.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "app_config.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

namespace {

constexpr const char *kTag = "telegram";

esp_err_t response_handler(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data != nullptr &&
        event->data_len > 0 && event->user_data != nullptr) {
        auto *response = static_cast<std::string *>(event->user_data);
        response->append(static_cast<const char *>(event->data),
                         static_cast<std::size_t>(event->data_len));
    }
    return ESP_OK;
}

bool send_startup_message(const std::string &ip)
{
    char message[256];
    std::snprintf(message, sizeof(message),
                  "🤖 <b>Kylio đã hoạt động</b>\n"
                  "🌐 <a href=\"http://%s\">http://%s</a>",
                  ip.c_str(), ip.c_str());

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(root, "chat_id", app_config::kTelegramChatId);
    cJSON_AddStringToObject(root, "text", message);
    cJSON_AddStringToObject(root, "parse_mode", "HTML");
    cJSON_AddBoolToObject(root, "disable_web_page_preview", true);
    char *serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized == nullptr) {
        return false;
    }
    const std::string body(serialized);
    cJSON_free(serialized);

    const std::string url = std::string("https://api.telegram.org/bot") +
                            app_config::kTelegramBotToken + "/sendMessage";
    std::string response;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = response_handler;
    config.user_data = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.data(),
                                   static_cast<int>(body.size()));
    const esp_err_t result = esp_http_client_perform(client);
    const int status = result == ESP_OK
                           ? esp_http_client_get_status_code(client)
                           : 0;
    esp_http_client_cleanup(client);

    bool accepted = false;
    if (result == ESP_OK && status >= 200 && status < 300) {
        cJSON *reply = cJSON_ParseWithLength(response.data(), response.size());
        if (reply != nullptr) {
            accepted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok"));
            cJSON_Delete(reply);
        }
    }
    if (!accepted) {
        ESP_LOGW(kTag, "Startup notification failed: err=%s status=%d",
                 esp_err_to_name(result), status);
    }
    return accepted;
}

void telegram_task(void *)
{
    uint32_t retry_ms = app_config::kTelegramRetryInitialMs;
    while (true) {
        if (wifi_manager_is_connected()) {
            const std::string ip = wifi_manager_ip_address();
            if (ip != "0.0.0.0") {
                if (send_startup_message(ip)) {
                    ESP_LOGI(kTag, "Startup notification sent");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(retry_ms));
                retry_ms = std::min<uint32_t>(retry_ms * 2U,
                                              app_config::kTelegramRetryMaxMs);
                continue;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t telegram_client_start()
{
    if (app_config::kTelegramBotToken[0] == '\0' ||
        app_config::kTelegramChatId[0] == '\0') {
        ESP_LOGW(kTag, "Telegram credentials are not configured");
        return ESP_OK;
    }
    const BaseType_t created =
        xTaskCreate(telegram_task, "telegram", 8192, nullptr, 4, nullptr);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
