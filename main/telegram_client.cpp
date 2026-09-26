#include "telegram_client.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app_config.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "robot_controller.h"
#include "telegram_messages.h"
#include "wifi_manager.h"

namespace {

constexpr const char *kTag = "telegram";
constexpr std::size_t kMessageSize = 768;

struct OutgoingMessage {
    char text[kMessageSize];
};

enum class SettingMode {
    None,
    FollowDistance,
    ObstacleDistance,
};

QueueHandle_t s_outgoing_queue = nullptr;
std::atomic_bool s_enabled{false};

int64_t load_last_update_id()
{
    nvs_handle_t handle;
    int64_t update_id = 0;
    if (nvs_open("telegram", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_i64(handle, "update_id", &update_id);
        nvs_close(handle);
    }
    return update_id;
}

void save_last_update_id(int64_t update_id)
{
    nvs_handle_t handle;
    if (nvs_open("telegram", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(kTag, "Could not open NVS to save Telegram update ID");
        return;
    }
    const esp_err_t result = nvs_set_i64(handle, "update_id", update_id);
    if (result == ESP_OK) {
        nvs_commit(handle);
    } else {
        ESP_LOGW(kTag, "Could not save Telegram update ID: %s",
                 esp_err_to_name(result));
    }
    nvs_close(handle);
}

int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data != nullptr &&
        event->data_len > 0 && event->user_data != nullptr) {
        auto *response = static_cast<std::string *>(event->user_data);
        response->append(static_cast<const char *>(event->data),
                         static_cast<std::size_t>(event->data_len));
    }
    return ESP_OK;
}

bool perform_request(const std::string &url, esp_http_client_method_t method,
                     const char *content_type, const std::string &body,
                     std::string *response)
{
    if (!wifi_manager_is_connected()) {
        return false;
    }

    std::string ignored_response;
    if (response == nullptr) {
        response = &ignored_response;
    }
    response->clear();

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = http_event_handler;
    config.user_data = response;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }

    esp_http_client_set_method(client, method);
    if (content_type != nullptr) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (!body.empty()) {
        esp_http_client_set_post_field(client, body.data(),
                                       static_cast<int>(body.size()));
    }

    const esp_err_t result = esp_http_client_perform(client);
    const int status = result == ESP_OK
                           ? esp_http_client_get_status_code(client)
                           : 0;
    esp_http_client_cleanup(client);

    if (result != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(kTag, "Telegram HTTP request failed: err=%s status=%d",
                 esp_err_to_name(result), status);
        return false;
    }
    return true;
}

std::string telegram_url(const char *method)
{
    return std::string("https://api.telegram.org/bot") +
           app_config::kTelegramBotToken + "/" + method;
}

bool post_json(cJSON *root)
{
    char *serialized = cJSON_PrintUnformatted(root);
    if (serialized == nullptr) {
        return false;
    }
    const std::string body(serialized);
    cJSON_free(serialized);
    return perform_request(telegram_url("sendMessage"), HTTP_METHOD_POST,
                           "application/json", body, nullptr);
}

bool send_message_now(const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(root, "chat_id", app_config::kTelegramChatId);
    cJSON_AddStringToObject(root, "text", message);
    const bool sent = post_json(root);
    cJSON_Delete(root);
    return sent;
}

void add_button_row(cJSON *keyboard, const char *first, const char *second = nullptr)
{
    cJSON *row = cJSON_CreateArray();
    cJSON_AddItemToArray(keyboard, row);
    cJSON *button = cJSON_CreateObject();
    cJSON_AddStringToObject(button, "text", first);
    cJSON_AddItemToArray(row, button);
    if (second != nullptr) {
        button = cJSON_CreateObject();
        cJSON_AddStringToObject(button, "text", second);
        cJSON_AddItemToArray(row, button);
    }
}

void send_menu()
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "chat_id", app_config::kTelegramChatId);
    cJSON_AddStringToObject(root, "text", telegram_messages::kMenuTitle);
    cJSON *reply_markup = cJSON_AddObjectToObject(root, "reply_markup");
    cJSON *keyboard = cJSON_AddArrayToObject(reply_markup, "keyboard");
    add_button_row(keyboard, telegram_messages::kCommandFollow,
                   telegram_messages::kCommandObstacle);
    add_button_row(keyboard, telegram_messages::kCommandRoom,
                   telegram_messages::kCommandStatus);
    add_button_row(keyboard, telegram_messages::kCommandSettings,
                   telegram_messages::kCommandStop);
    cJSON_AddBoolToObject(reply_markup, "resize_keyboard", true);
    post_json(root);
    cJSON_Delete(root);
}

void send_settings_menu()
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "chat_id", app_config::kTelegramChatId);
    cJSON_AddStringToObject(root, "text",
                            telegram_messages::kSettingsMenuTitle);
    cJSON *reply_markup = cJSON_AddObjectToObject(root, "reply_markup");
    cJSON *keyboard = cJSON_AddArrayToObject(reply_markup, "keyboard");
    add_button_row(keyboard, telegram_messages::kCommandSettingFollow,
                   telegram_messages::kCommandSettingAvoid);
    add_button_row(keyboard, telegram_messages::kCommandMenu);
    cJSON_AddBoolToObject(reply_markup, "resize_keyboard", true);
    post_json(root);
    cJSON_Delete(root);
}

void send_status()
{
    const RobotSnapshot snapshot = robot_controller_snapshot();
    char message[kMessageSize];
    telegram_messages::format_status(message, sizeof(message), snapshot,
                                     wifi_manager_rssi(),
                                     wifi_manager_ip_address().c_str());
    send_message_now(message);
}

std::string trim(std::string value)
{
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

bool parse_distance(const std::string &text, float *distance)
{
    char *end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || end == nullptr || *end != '\0' ||
        !std::isfinite(parsed)) {
        return false;
    }
    *distance = parsed;
    return true;
}

void handle_command(const std::string &raw_command, SettingMode *setting_mode)
{
    const std::string command = trim(raw_command);

    if (*setting_mode != SettingMode::None) {
        if (command == telegram_messages::kCommandMenu) {
            *setting_mode = SettingMode::None;
            send_menu();
            return;
        }
        if (command == telegram_messages::kCommandSettings) {
            *setting_mode = SettingMode::None;
            send_settings_menu();
            return;
        }

        float distance = 0.0F;
        const bool parsed = parse_distance(command, &distance);
        const bool updated = parsed &&
            (*setting_mode == SettingMode::FollowDistance
                 ? robot_controller_set_follow_distance(distance)
                 : robot_controller_set_obstacle_distance(distance));
        if (!updated) {
            send_message_now(telegram_messages::kInvalidDistance);
            return;
        }

        char confirmation[96];
        telegram_messages::format_distance_confirmation(
            confirmation, sizeof(confirmation),
            *setting_mode == SettingMode::FollowDistance, distance);
        *setting_mode = SettingMode::None;
        send_message_now(confirmation);
        send_settings_menu();
        return;
    }

    if (command == telegram_messages::kCommandMenu || command == "/start") {
        send_menu();
    } else if (command == telegram_messages::kCommandStatus) {
        send_status();
    } else if (command == telegram_messages::kCommandSettings) {
        send_settings_menu();
    } else if (command == telegram_messages::kCommandSettingFollow) {
        *setting_mode = SettingMode::FollowDistance;
        const RobotSnapshot snapshot = robot_controller_snapshot();
        char prompt[128];
        telegram_messages::format_follow_distance_prompt(
            prompt, sizeof(prompt), snapshot.follow_distance_cm);
        send_message_now(prompt);
    } else if (command == telegram_messages::kCommandSettingAvoid) {
        *setting_mode = SettingMode::ObstacleDistance;
        const RobotSnapshot snapshot = robot_controller_snapshot();
        char prompt[128];
        telegram_messages::format_avoid_distance_prompt(
            prompt, sizeof(prompt), snapshot.obstacle_distance_cm);
        send_message_now(prompt);
    } else if (command == telegram_messages::kCommandStop) {
        robot_controller_emergency_stop();
        send_message_now(telegram_messages::kStopped);
    } else if (command == telegram_messages::kCommandFollow) {
        send_message_now(telegram_messages::kFollowWebControl);
    } else if (command == telegram_messages::kCommandObstacle) {
        send_message_now(telegram_messages::kObstacleWebControl);
    } else if (command == telegram_messages::kCommandRoom) {
        send_message_now(telegram_messages::kRoomWebControl);
    } else if (command == "/help") {
        send_menu();
    } else {
        send_message_now(telegram_messages::kUnknownCommand);
    }
}

bool poll_updates(int64_t *last_update_id, SettingMode *setting_mode)
{
    char suffix[128];
    std::snprintf(suffix, sizeof(suffix),
                  "getUpdates?offset=%lld&limit=1&timeout=0",
                  static_cast<long long>(*last_update_id + 1));
    std::string response;
    if (!perform_request(telegram_url(suffix), HTTP_METHOD_GET, nullptr, {},
                         &response)) {
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(response.data(), response.size());
    if (root == nullptr) {
        ESP_LOGW(kTag, "Invalid JSON in getUpdates response");
        return false;
    }

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *update = cJSON_IsArray(result)
                              ? cJSON_GetArrayItem(result, 0)
                              : nullptr;
    if (update == nullptr) {
        cJSON_Delete(root);
        return true;
    }

    const cJSON *update_id =
        cJSON_GetObjectItemCaseSensitive(update, "update_id");
    if (cJSON_IsNumber(update_id)) {
        *last_update_id = static_cast<int64_t>(update_id->valuedouble);
    }

    const cJSON *message =
        cJSON_GetObjectItemCaseSensitive(update, "message");
    const cJSON *chat = cJSON_IsObject(message)
                            ? cJSON_GetObjectItemCaseSensitive(message, "chat")
                            : nullptr;
    const cJSON *chat_id = cJSON_IsObject(chat)
                               ? cJSON_GetObjectItemCaseSensitive(chat, "id")
                               : nullptr;
    const cJSON *text = cJSON_IsObject(message)
                            ? cJSON_GetObjectItemCaseSensitive(message, "text")
                            : nullptr;

    if (cJSON_IsNumber(chat_id) && cJSON_IsString(text) &&
        text->valuestring != nullptr) {
        char sender[32];
        std::snprintf(sender, sizeof(sender), "%lld",
                      static_cast<long long>(chat_id->valuedouble));
        if (std::strcmp(sender, app_config::kTelegramChatId) == 0) {
            handle_command(text->valuestring, setting_mode);
        } else {
            ESP_LOGW(kTag, "Ignored message from an unauthorized chat");
        }
    }

    cJSON_Delete(root);
    return true;
}

void telegram_task(void *)
{
    int64_t last_poll_ms = 0;
    int64_t last_update_id = load_last_update_id();
    bool startup_message_enqueued = false;
    SettingMode setting_mode = SettingMode::None;
    int64_t next_send_attempt_ms = 0;
    uint32_t send_retry_delay_ms = app_config::kTelegramRetryInitialMs;

    while (true) {
        if (telegram_client_is_configured() && wifi_manager_is_connected()) {
            if (!startup_message_enqueued) {
                char message[kMessageSize];
                telegram_messages::format_startup(
                    message, sizeof(message), wifi_manager_ip_address().c_str());
                startup_message_enqueued = telegram_client_enqueue_message(
                    message);
            }

            OutgoingMessage outgoing = {};
            if (s_outgoing_queue != nullptr &&
                xQueuePeek(s_outgoing_queue, &outgoing, 0) == pdTRUE &&
                now_ms() >= next_send_attempt_ms) {
                if (send_message_now(outgoing.text)) {
                    xQueueReceive(s_outgoing_queue, &outgoing, 0);
                    send_retry_delay_ms = app_config::kTelegramRetryInitialMs;
                    next_send_attempt_ms = 0;
                } else {
                    next_send_attempt_ms = now_ms() + send_retry_delay_ms;
                    send_retry_delay_ms = std::min<uint32_t>(
                        send_retry_delay_ms * 2U,
                        app_config::kTelegramRetryMaxMs);
                }
            }

            const int64_t now = now_ms();
            if (s_enabled.load() &&
                now - last_poll_ms >= app_config::kTelegramPollMs) {
                last_poll_ms = now;
                const int64_t update_id_before_poll = last_update_id;
                if (poll_updates(&last_update_id, &setting_mode) &&
                    last_update_id != update_id_before_poll) {
                    save_last_update_id(last_update_id);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

}  // namespace

esp_err_t telegram_client_start()
{
    s_outgoing_queue = xQueueCreate(8, sizeof(OutgoingMessage));
    if (s_outgoing_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (!telegram_client_is_configured()) {
        ESP_LOGW(kTag,
                 "Telegram is not configured; set token and chat ID in "
                 "secrets.local.h");
    }
    const BaseType_t created =
        xTaskCreate(telegram_task, "telegram", 8192, nullptr, 4, nullptr);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool telegram_client_is_configured()
{
    return app_config::kTelegramBotToken[0] != '\0' &&
           app_config::kTelegramChatId[0] != '\0';
}

bool telegram_client_is_enabled()
{
    return s_enabled.load();
}

void telegram_client_set_enabled(bool enabled)
{
    s_enabled.store(enabled);
}

bool telegram_client_enqueue_message(const char *message)
{
    if (s_outgoing_queue == nullptr || message == nullptr ||
        !telegram_client_is_configured()) {
        return false;
    }
    OutgoingMessage outgoing = {};
    std::snprintf(outgoing.text, sizeof(outgoing.text), "%s", message);
    if (xQueueSend(s_outgoing_queue, &outgoing, 0) == pdTRUE) {
        return true;
    }
    ESP_LOGW(kTag, "Telegram outgoing queue is full; message dropped");
    return false;
}
