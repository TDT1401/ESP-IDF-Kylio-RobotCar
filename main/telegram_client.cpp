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
    bool remove_reply_keyboard;
};

enum class SettingMode {
    None,
    FollowDistance,
    ObstacleDistance,
};

enum class Panel {
    Control,
    Settings,
    FollowDistance,
    ObstacleDistance,
    ConfirmFollow,
    ConfirmObstacle,
    ConfirmRoom,
    FollowInput,
    ObstacleInput,
};

inline constexpr char kCallbackControl[] = "panel:control";
inline constexpr char kCallbackRefresh[] = "panel:refresh";
inline constexpr char kCallbackSettings[] = "panel:settings";
inline constexpr char kCallbackFollowSettings[] = "panel:follow";
inline constexpr char kCallbackAvoidSettings[] = "panel:avoid";
inline constexpr char kCallbackAskFollow[] = "ask:follow";
inline constexpr char kCallbackAskObstacle[] = "ask:obstacle";
inline constexpr char kCallbackAskRoom[] = "ask:room";
inline constexpr char kCallbackActivateFollow[] = "activate:follow";
inline constexpr char kCallbackActivateObstacle[] = "activate:obstacle";
inline constexpr char kCallbackActivateRoom[] = "activate:room";
inline constexpr char kCallbackStop[] = "robot:stop";
inline constexpr char kCallbackFollowMinus10[] = "adjust:follow:-10";
inline constexpr char kCallbackFollowMinus5[] = "adjust:follow:-5";
inline constexpr char kCallbackFollowPlus5[] = "adjust:follow:5";
inline constexpr char kCallbackFollowPlus10[] = "adjust:follow:10";
inline constexpr char kCallbackAvoidMinus10[] = "adjust:avoid:-10";
inline constexpr char kCallbackAvoidMinus5[] = "adjust:avoid:-5";
inline constexpr char kCallbackAvoidPlus5[] = "adjust:avoid:5";
inline constexpr char kCallbackAvoidPlus10[] = "adjust:avoid:10";
inline constexpr char kCallbackInputFollow[] = "input:follow";
inline constexpr char kCallbackInputAvoid[] = "input:avoid";

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

bool post_json(const char *method, cJSON *root)
{
    char *serialized = cJSON_PrintUnformatted(root);
    if (serialized == nullptr) {
        return false;
    }
    const std::string body(serialized);
    cJSON_free(serialized);
    return perform_request(telegram_url(method), HTTP_METHOD_POST,
                           "application/json", body, nullptr);
}

void add_message_options(cJSON *root)
{
    cJSON_AddStringToObject(root, "parse_mode", "HTML");
    cJSON_AddBoolToObject(root, "disable_web_page_preview", true);
}

bool send_message_now(const char *message, bool remove_reply_keyboard = false)
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(root, "chat_id", app_config::kTelegramChatId);
    cJSON_AddStringToObject(root, "text", message);
    add_message_options(root);
    if (remove_reply_keyboard) {
        cJSON *reply_markup = cJSON_AddObjectToObject(root, "reply_markup");
        cJSON_AddBoolToObject(reply_markup, "remove_keyboard", true);
    }
    const bool sent = post_json("sendMessage", root);
    cJSON_Delete(root);
    return sent;
}

void add_bot_command(cJSON *commands, const char *command,
                     const char *description)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "command", command);
    cJSON_AddStringToObject(item, "description", description);
    cJSON_AddItemToArray(commands, item);
}

bool set_bot_commands()
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON *commands = cJSON_AddArrayToObject(root, "commands");
    add_bot_command(commands, "start", "Mở bảng điều khiển");
    add_bot_command(commands, "menu", "Mở menu chính");
    add_bot_command(commands, "status", "Xem trạng thái robot");
    add_bot_command(commands, "settings", "Cài đặt khoảng cách");
    add_bot_command(commands, "stop", "Dừng robot");
    add_bot_command(commands, "follow", "Bật Follow Mode");
    add_bot_command(commands, "obstacle", "Bật Obstacle Mode");
    add_bot_command(commands, "room", "Bật Room Monitor");
    add_bot_command(commands, "help", "Hiển thị menu trợ giúp");
    const bool updated = post_json("setMyCommands", root);
    cJSON_Delete(root);
    return updated;
}

void add_inline_callback_button(cJSON *row, const char *text,
                                const char *callback_data)
{
    cJSON *button = cJSON_CreateObject();
    cJSON_AddStringToObject(button, "text", text);
    cJSON_AddStringToObject(button, "callback_data", callback_data);
    cJSON_AddItemToArray(row, button);
}

void add_inline_url_button(cJSON *row, const char *text, const char *url)
{
    cJSON *button = cJSON_CreateObject();
    cJSON_AddStringToObject(button, "text", text);
    cJSON_AddStringToObject(button, "url", url);
    cJSON_AddItemToArray(row, button);
}

cJSON *add_inline_row(cJSON *keyboard)
{
    cJSON *row = cJSON_CreateArray();
    cJSON_AddItemToArray(keyboard, row);
    return row;
}

RobotMode confirmation_mode(Panel panel)
{
    switch (panel) {
    case Panel::ConfirmObstacle: return RobotMode::Obstacle;
    case Panel::ConfirmRoom: return RobotMode::RoomMonitor;
    case Panel::ConfirmFollow:
    default: return RobotMode::Follow;
    }
}

void add_control_keyboard(cJSON *keyboard, const RobotSnapshot &snapshot)
{
    cJSON *row = add_inline_row(keyboard);
    add_inline_callback_button(
        row, snapshot.mode == RobotMode::Follow ? "✅ Follow · Tắt"
                                                : "👣 Follow",
        snapshot.mode == RobotMode::Follow ? kCallbackStop
                                           : kCallbackAskFollow);
    add_inline_callback_button(
        row, snapshot.mode == RobotMode::Obstacle ? "✅ Obstacle · Tắt"
                                                  : "🛡️ Obstacle",
        snapshot.mode == RobotMode::Obstacle ? kCallbackStop
                                             : kCallbackAskObstacle);

    row = add_inline_row(keyboard);
    add_inline_callback_button(
        row, snapshot.mode == RobotMode::RoomMonitor ? "✅ Room · Tắt"
                                                     : "🏠 Room Monitor",
        snapshot.mode == RobotMode::RoomMonitor ? kCallbackStop
                                                : kCallbackAskRoom);
    add_inline_callback_button(row, "🛑 Dừng robot", kCallbackStop);

    row = add_inline_row(keyboard);
    add_inline_callback_button(row, "🔄 Làm mới", kCallbackRefresh);
    add_inline_callback_button(row, "⚙️ Cài đặt", kCallbackSettings);

    char url[64];
    std::snprintf(url, sizeof(url), "http://%s",
                  wifi_manager_ip_address().c_str());
    row = add_inline_row(keyboard);
    add_inline_url_button(row, "🌐 Mở Web Control", url);
}

void add_settings_keyboard(cJSON *keyboard, const RobotSnapshot &snapshot)
{
    char follow[48];
    char avoid[48];
    std::snprintf(follow, sizeof(follow), "📏 Follow · %.1f cm",
                  snapshot.follow_distance_cm);
    std::snprintf(avoid, sizeof(avoid), "📐 Avoid · %.1f cm",
                  snapshot.obstacle_distance_cm);

    cJSON *row = add_inline_row(keyboard);
    add_inline_callback_button(row, follow, kCallbackFollowSettings);
    row = add_inline_row(keyboard);
    add_inline_callback_button(row, avoid, kCallbackAvoidSettings);
    row = add_inline_row(keyboard);
    add_inline_callback_button(row, "⬅️ Bảng điều khiển", kCallbackControl);
}

void add_distance_keyboard(cJSON *keyboard, bool follow_distance)
{
    const char *minus_10 = follow_distance ? kCallbackFollowMinus10
                                           : kCallbackAvoidMinus10;
    const char *minus_5 = follow_distance ? kCallbackFollowMinus5
                                          : kCallbackAvoidMinus5;
    const char *plus_5 = follow_distance ? kCallbackFollowPlus5
                                         : kCallbackAvoidPlus5;
    const char *plus_10 = follow_distance ? kCallbackFollowPlus10
                                          : kCallbackAvoidPlus10;

    cJSON *row = add_inline_row(keyboard);
    add_inline_callback_button(row, "−10", minus_10);
    add_inline_callback_button(row, "−5", minus_5);
    add_inline_callback_button(row, "+5", plus_5);
    add_inline_callback_button(row, "+10", plus_10);
    row = add_inline_row(keyboard);
    add_inline_callback_button(row, "✏️ Nhập số",
                               follow_distance ? kCallbackInputFollow
                                               : kCallbackInputAvoid);
    add_inline_callback_button(row, "⬅️ Cài đặt", kCallbackSettings);
}

void add_confirmation_keyboard(cJSON *keyboard, RobotMode mode)
{
    const char *activate = mode == RobotMode::Follow
                               ? kCallbackActivateFollow
                               : mode == RobotMode::Obstacle
                                     ? kCallbackActivateObstacle
                                     : kCallbackActivateRoom;
    cJSON *row = add_inline_row(keyboard);
    add_inline_callback_button(row, "✅ Xác nhận", activate);
    add_inline_callback_button(row, "❌ Hủy", kCallbackControl);
}

void add_input_keyboard(cJSON *keyboard, bool follow_distance)
{
    cJSON *row = add_inline_row(keyboard);
    add_inline_callback_button(row, "⬅️ Quay lại",
                               follow_distance ? kCallbackFollowSettings
                                               : kCallbackAvoidSettings);
    add_inline_callback_button(row, "🏠 Điều khiển", kCallbackControl);
}

bool show_panel(Panel panel, const char *chat_id, int64_t message_id = 0)
{
    const RobotSnapshot snapshot = robot_controller_snapshot();
    char text[kMessageSize];
    switch (panel) {
    case Panel::Settings:
        telegram_messages::format_settings_panel(text, sizeof(text), snapshot);
        break;
    case Panel::FollowDistance:
        telegram_messages::format_distance_panel(
            text, sizeof(text), true, snapshot.follow_distance_cm);
        break;
    case Panel::ObstacleDistance:
        telegram_messages::format_distance_panel(
            text, sizeof(text), false, snapshot.obstacle_distance_cm);
        break;
    case Panel::ConfirmFollow:
    case Panel::ConfirmObstacle:
    case Panel::ConfirmRoom:
        telegram_messages::format_mode_confirmation(
            text, sizeof(text), confirmation_mode(panel));
        break;
    case Panel::FollowInput:
        telegram_messages::format_follow_distance_prompt(
            text, sizeof(text), snapshot.follow_distance_cm);
        break;
    case Panel::ObstacleInput:
        telegram_messages::format_avoid_distance_prompt(
            text, sizeof(text), snapshot.obstacle_distance_cm);
        break;
    case Panel::Control:
    default:
        telegram_messages::format_status(
            text, sizeof(text), snapshot, wifi_manager_rssi(),
            wifi_manager_ip_address().c_str());
        break;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(root, "chat_id", chat_id);
    if (message_id != 0) {
        cJSON_AddNumberToObject(root, "message_id",
                                static_cast<double>(message_id));
    }
    cJSON_AddStringToObject(root, "text", text);
    add_message_options(root);
    cJSON *reply_markup = cJSON_AddObjectToObject(root, "reply_markup");
    cJSON *keyboard = cJSON_AddArrayToObject(reply_markup, "inline_keyboard");

    switch (panel) {
    case Panel::Settings:
        add_settings_keyboard(keyboard, snapshot);
        break;
    case Panel::FollowDistance:
        add_distance_keyboard(keyboard, true);
        break;
    case Panel::ObstacleDistance:
        add_distance_keyboard(keyboard, false);
        break;
    case Panel::ConfirmFollow:
    case Panel::ConfirmObstacle:
    case Panel::ConfirmRoom:
        add_confirmation_keyboard(keyboard, confirmation_mode(panel));
        break;
    case Panel::FollowInput:
        add_input_keyboard(keyboard, true);
        break;
    case Panel::ObstacleInput:
        add_input_keyboard(keyboard, false);
        break;
    case Panel::Control:
    default:
        add_control_keyboard(keyboard, snapshot);
        break;
    }

    const bool sent = post_json(message_id == 0 ? "sendMessage"
                                                : "editMessageText",
                                root);
    cJSON_Delete(root);
    return sent;
}

void send_menu()
{
    show_panel(Panel::Control, app_config::kTelegramChatId);
}

void send_settings_menu()
{
    show_panel(Panel::Settings, app_config::kTelegramChatId);
}

void send_status()
{
    send_menu();
}

void begin_follow_distance_setting(SettingMode *setting_mode)
{
    *setting_mode = SettingMode::FollowDistance;
    show_panel(Panel::FollowInput, app_config::kTelegramChatId);
}

void begin_obstacle_distance_setting(SettingMode *setting_mode)
{
    *setting_mode = SettingMode::ObstacleDistance;
    show_panel(Panel::ObstacleInput, app_config::kTelegramChatId);
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

bool command_is(const std::string &command, const char *canonical,
                const char *button_label)
{
    return command == canonical || command == button_label;
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

bool answer_callback_query(const char *callback_query_id,
                           const char *text = nullptr,
                           bool show_alert = false)
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(root, "callback_query_id", callback_query_id);
    if (text != nullptr) {
        cJSON_AddStringToObject(root, "text", text);
    }
    if (show_alert) {
        cJSON_AddBoolToObject(root, "show_alert", true);
    }
    const bool answered = post_json("answerCallbackQuery", root);
    cJSON_Delete(root);
    return answered;
}

bool chat_id_string(const cJSON *chat_id, char *out, std::size_t out_size)
{
    if (!cJSON_IsNumber(chat_id)) {
        return false;
    }
    std::snprintf(out, out_size, "%lld",
                  static_cast<long long>(chat_id->valuedouble));
    return true;
}

bool callback_is(const char *data, const char *expected)
{
    return data != nullptr && std::strcmp(data, expected) == 0;
}

Panel confirmation_panel_for_callback(const char *data)
{
    if (callback_is(data, kCallbackAskObstacle)) {
        return Panel::ConfirmObstacle;
    }
    if (callback_is(data, kCallbackAskRoom)) {
        return Panel::ConfirmRoom;
    }
    return Panel::ConfirmFollow;
}

RobotMode activation_mode_for_callback(const char *data)
{
    if (callback_is(data, kCallbackActivateObstacle)) {
        return RobotMode::Obstacle;
    }
    if (callback_is(data, kCallbackActivateRoom)) {
        return RobotMode::RoomMonitor;
    }
    return RobotMode::Follow;
}

const char *activation_answer(RobotMode mode)
{
    switch (mode) {
    case RobotMode::Obstacle: return "Đã bật Obstacle Mode";
    case RobotMode::RoomMonitor: return "Đang hiệu chuẩn Room Monitor";
    case RobotMode::Follow: return "Đã bật Follow Mode";
    case RobotMode::Manual:
    default: return "Đã chuyển về Manual";
    }
}

bool decode_distance_adjustment(const char *data, bool *follow_distance,
                                float *delta)
{
    constexpr const char *kFollowPrefix = "adjust:follow:";
    constexpr const char *kAvoidPrefix = "adjust:avoid:";
    const char *value = nullptr;
    if (data != nullptr &&
        std::strncmp(data, kFollowPrefix, std::strlen(kFollowPrefix)) == 0) {
        *follow_distance = true;
        value = data + std::strlen(kFollowPrefix);
    } else if (data != nullptr &&
               std::strncmp(data, kAvoidPrefix,
                            std::strlen(kAvoidPrefix)) == 0) {
        *follow_distance = false;
        value = data + std::strlen(kAvoidPrefix);
    } else {
        return false;
    }
    return parse_distance(value, delta);
}

void handle_callback_query(const cJSON *callback_query,
                           SettingMode *setting_mode)
{
    const cJSON *query_id = cJSON_GetObjectItemCaseSensitive(
        callback_query, "id");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(
        callback_query, "data");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(
        callback_query, "message");
    const cJSON *chat = cJSON_IsObject(message)
                            ? cJSON_GetObjectItemCaseSensitive(message, "chat")
                            : nullptr;
    const cJSON *chat_id = cJSON_IsObject(chat)
                               ? cJSON_GetObjectItemCaseSensitive(chat, "id")
                               : nullptr;
    const cJSON *message_id = cJSON_IsObject(message)
                                  ? cJSON_GetObjectItemCaseSensitive(
                                        message, "message_id")
                                  : nullptr;

    if (!cJSON_IsString(query_id) || query_id->valuestring == nullptr) {
        return;
    }
    if (!cJSON_IsString(data) || data->valuestring == nullptr ||
        !cJSON_IsNumber(message_id)) {
        answer_callback_query(query_id->valuestring,
                              "Bảng điều khiển đã hết hạn. Gửi /menu.", true);
        return;
    }

    char callback_chat_id[32];
    if (!chat_id_string(chat_id, callback_chat_id,
                        sizeof(callback_chat_id)) ||
        std::strcmp(callback_chat_id, app_config::kTelegramChatId) != 0) {
        answer_callback_query(query_id->valuestring,
                              "Bạn không có quyền điều khiển robot.", true);
        ESP_LOGW(kTag, "Ignored callback from an unauthorized chat");
        return;
    }

    const char *callback_data = data->valuestring;
    const int64_t panel_message_id =
        static_cast<int64_t>(message_id->valuedouble);

    if (callback_is(callback_data, kCallbackControl) ||
        callback_is(callback_data, kCallbackRefresh)) {
        *setting_mode = SettingMode::None;
        answer_callback_query(
            query_id->valuestring,
            callback_is(callback_data, kCallbackRefresh) ? "Đã làm mới"
                                                         : nullptr);
        show_panel(Panel::Control, callback_chat_id, panel_message_id);
        return;
    }
    if (callback_is(callback_data, kCallbackSettings)) {
        *setting_mode = SettingMode::None;
        answer_callback_query(query_id->valuestring);
        show_panel(Panel::Settings, callback_chat_id, panel_message_id);
        return;
    }
    if (callback_is(callback_data, kCallbackFollowSettings) ||
        callback_is(callback_data, kCallbackAvoidSettings)) {
        *setting_mode = SettingMode::None;
        const bool follow = callback_is(callback_data,
                                        kCallbackFollowSettings);
        answer_callback_query(query_id->valuestring);
        show_panel(follow ? Panel::FollowDistance
                          : Panel::ObstacleDistance,
                   callback_chat_id, panel_message_id);
        return;
    }
    if (callback_is(callback_data, kCallbackAskFollow) ||
        callback_is(callback_data, kCallbackAskObstacle) ||
        callback_is(callback_data, kCallbackAskRoom)) {
        *setting_mode = SettingMode::None;
        answer_callback_query(query_id->valuestring);
        show_panel(confirmation_panel_for_callback(callback_data),
                   callback_chat_id, panel_message_id);
        return;
    }
    if (callback_is(callback_data, kCallbackActivateFollow) ||
        callback_is(callback_data, kCallbackActivateObstacle) ||
        callback_is(callback_data, kCallbackActivateRoom)) {
        const RobotMode mode = activation_mode_for_callback(callback_data);
        *setting_mode = SettingMode::None;
        robot_controller_set_mode(mode, true);
        answer_callback_query(query_id->valuestring, activation_answer(mode));
        show_panel(Panel::Control, callback_chat_id, panel_message_id);
        return;
    }
    if (callback_is(callback_data, kCallbackStop)) {
        *setting_mode = SettingMode::None;
        robot_controller_emergency_stop();
        answer_callback_query(query_id->valuestring, "Robot đã dừng");
        show_panel(Panel::Control, callback_chat_id, panel_message_id);
        return;
    }
    if (callback_is(callback_data, kCallbackInputFollow) ||
        callback_is(callback_data, kCallbackInputAvoid)) {
        const bool follow = callback_is(callback_data, kCallbackInputFollow);
        *setting_mode = follow ? SettingMode::FollowDistance
                               : SettingMode::ObstacleDistance;
        answer_callback_query(query_id->valuestring,
                              "Hãy gửi giá trị từ 10 đến 200 cm");
        show_panel(follow ? Panel::FollowInput : Panel::ObstacleInput,
                   callback_chat_id, panel_message_id);
        return;
    }

    bool follow_distance = false;
    float delta = 0.0F;
    if (decode_distance_adjustment(callback_data, &follow_distance, &delta)) {
        const RobotSnapshot snapshot = robot_controller_snapshot();
        const float current = follow_distance
                                  ? snapshot.follow_distance_cm
                                  : snapshot.obstacle_distance_cm;
        const float updated_distance = std::max(
            10.0F, std::min(200.0F, current + delta));
        if (std::fabs(updated_distance - current) < 0.01F) {
            answer_callback_query(query_id->valuestring,
                                  "Đã đạt giới hạn 10–200 cm", true);
            return;
        }
        const bool updated = follow_distance
                                 ? robot_controller_set_follow_distance(
                                       updated_distance)
                                 : robot_controller_set_obstacle_distance(
                                       updated_distance);
        if (!updated) {
            answer_callback_query(query_id->valuestring,
                                  "Giới hạn cho phép là 10–200 cm", true);
            return;
        }
        *setting_mode = SettingMode::None;
        answer_callback_query(query_id->valuestring, "Đã cập nhật khoảng cách");
        show_panel(follow_distance ? Panel::FollowDistance
                                   : Panel::ObstacleDistance,
                   callback_chat_id, panel_message_id);
        return;
    }

    answer_callback_query(query_id->valuestring,
                          "Nút này không còn hiệu lực. Gửi /menu.", true);
}

void handle_command(const std::string &raw_command, SettingMode *setting_mode)
{
    const std::string command = trim(raw_command);

    if (*setting_mode != SettingMode::None) {
        if (command_is(command, telegram_messages::kCommandMenu,
                       telegram_messages::kButtonMenu) ||
            command == "/start" || command == "/help") {
            *setting_mode = SettingMode::None;
            send_menu();
            return;
        }
        if (command_is(command, telegram_messages::kCommandStatus,
                       telegram_messages::kButtonStatus)) {
            *setting_mode = SettingMode::None;
            send_status();
            return;
        }
        if (command_is(command, telegram_messages::kCommandSettings,
                       telegram_messages::kButtonSettings)) {
            *setting_mode = SettingMode::None;
            send_settings_menu();
            return;
        }
        if (command_is(command, telegram_messages::kCommandSettingFollow,
                       telegram_messages::kButtonSettingFollow)) {
            begin_follow_distance_setting(setting_mode);
            return;
        }
        if (command_is(command, telegram_messages::kCommandSettingAvoid,
                       telegram_messages::kButtonSettingAvoid)) {
            begin_obstacle_distance_setting(setting_mode);
            return;
        }
        if (command_is(command, telegram_messages::kCommandStop,
                       telegram_messages::kButtonStop)) {
            *setting_mode = SettingMode::None;
            robot_controller_emergency_stop();
            send_menu();
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

        char confirmation[kMessageSize];
        telegram_messages::format_distance_confirmation(
            confirmation, sizeof(confirmation),
            *setting_mode == SettingMode::FollowDistance, distance);
        *setting_mode = SettingMode::None;
        send_message_now(confirmation);
        send_settings_menu();
        return;
    }

    if (command_is(command, telegram_messages::kCommandMenu,
                   telegram_messages::kButtonMenu) ||
        command == "/start") {
        send_menu();
    } else if (command_is(command, telegram_messages::kCommandStatus,
                          telegram_messages::kButtonStatus)) {
        send_status();
    } else if (command_is(command, telegram_messages::kCommandSettings,
                          telegram_messages::kButtonSettings)) {
        send_settings_menu();
    } else if (command_is(command, telegram_messages::kCommandSettingFollow,
                          telegram_messages::kButtonSettingFollow)) {
        begin_follow_distance_setting(setting_mode);
    } else if (command_is(command, telegram_messages::kCommandSettingAvoid,
                          telegram_messages::kButtonSettingAvoid)) {
        begin_obstacle_distance_setting(setting_mode);
    } else if (command_is(command, telegram_messages::kCommandStop,
                          telegram_messages::kButtonStop)) {
        robot_controller_emergency_stop();
        send_menu();
    } else if (command_is(command, telegram_messages::kCommandFollow,
                          telegram_messages::kButtonFollow)) {
        show_panel(Panel::ConfirmFollow, app_config::kTelegramChatId);
    } else if (command_is(command, telegram_messages::kCommandObstacle,
                          telegram_messages::kButtonObstacle)) {
        show_panel(Panel::ConfirmObstacle, app_config::kTelegramChatId);
    } else if (command_is(command, telegram_messages::kCommandRoom,
                          telegram_messages::kButtonRoom)) {
        show_panel(Panel::ConfirmRoom, app_config::kTelegramChatId);
    } else if (command == "/help") {
        send_menu();
    } else {
        send_message_now(telegram_messages::kUnknownCommand);
    }
}

bool poll_updates(int64_t *last_update_id, SettingMode *setting_mode)
{
    char suffix[256];
    std::snprintf(suffix, sizeof(suffix),
                  "getUpdates?offset=%lld&limit=10&timeout=2&"
                  "allowed_updates=%%5B%%22message%%22%%2C%%22callback_query%%22%%5D",
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
    if (!cJSON_IsArray(result)) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "Missing update array in Telegram response");
        return false;
    }

    const int update_count = cJSON_GetArraySize(result);
    for (int index = 0; index < update_count; ++index) {
        const cJSON *update = cJSON_GetArrayItem(result, index);
        if (!cJSON_IsObject(update)) {
            continue;
        }

        const cJSON *update_id =
            cJSON_GetObjectItemCaseSensitive(update, "update_id");
        if (cJSON_IsNumber(update_id)) {
            const int64_t received_id =
                static_cast<int64_t>(update_id->valuedouble);
            *last_update_id = std::max(*last_update_id, received_id);
        }

        const cJSON *callback_query =
            cJSON_GetObjectItemCaseSensitive(update, "callback_query");
        if (cJSON_IsObject(callback_query)) {
            handle_callback_query(callback_query, setting_mode);
            continue;
        }

        const cJSON *message =
            cJSON_GetObjectItemCaseSensitive(update, "message");
        const cJSON *chat = cJSON_IsObject(message)
                                ? cJSON_GetObjectItemCaseSensitive(message,
                                                                   "chat")
                                : nullptr;
        const cJSON *chat_id = cJSON_IsObject(chat)
                                   ? cJSON_GetObjectItemCaseSensitive(chat,
                                                                      "id")
                                   : nullptr;
        const cJSON *message_text = cJSON_IsObject(message)
                                        ? cJSON_GetObjectItemCaseSensitive(
                                              message, "text")
                                        : nullptr;

        char sender[32];
        if (chat_id_string(chat_id, sender, sizeof(sender)) &&
            cJSON_IsString(message_text) &&
            message_text->valuestring != nullptr) {
            if (std::strcmp(sender, app_config::kTelegramChatId) == 0) {
                handle_command(message_text->valuestring, setting_mode);
            } else {
                ESP_LOGW(kTag, "Ignored message from an unauthorized chat");
            }
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
    bool bot_commands_attempted = false;
    SettingMode setting_mode = SettingMode::None;
    int64_t next_send_attempt_ms = 0;
    uint32_t send_retry_delay_ms = app_config::kTelegramRetryInitialMs;

    while (true) {
        if (telegram_client_is_configured() && wifi_manager_is_connected()) {
            if (!bot_commands_attempted) {
                bot_commands_attempted = true;
                if (!set_bot_commands()) {
                    ESP_LOGW(kTag, "Could not update the Telegram command menu");
                }
            }

            if (!startup_message_enqueued) {
                OutgoingMessage startup = {};
                telegram_messages::format_startup(
                    startup.text, sizeof(startup.text),
                    wifi_manager_ip_address().c_str());
                startup.remove_reply_keyboard = true;
                startup_message_enqueued = s_outgoing_queue != nullptr &&
                    xQueueSend(s_outgoing_queue, &startup, 0) == pdTRUE;
            }

            OutgoingMessage outgoing = {};
            if (s_outgoing_queue != nullptr &&
                xQueuePeek(s_outgoing_queue, &outgoing, 0) == pdTRUE &&
                now_ms() >= next_send_attempt_ms) {
                if (send_message_now(outgoing.text,
                                     outgoing.remove_reply_keyboard)) {
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
