#include "web_server.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#include "app_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/base64.h"
#include "robot_controller.h"
#include "web_page.h"
#include "wifi_manager.h"

namespace {

constexpr const char *kTag = "web";
httpd_handle_t s_server = nullptr;
std::string s_expected_authorization;
uint64_t s_last_command_sequence = 0;
portMUX_TYPE s_command_sequence_lock = portMUX_INITIALIZER_UNLOCKED;

bool constant_time_equal(const char *left, const std::string &right)
{
    if (left == nullptr) {
        return false;
    }
    const std::size_t left_length = std::strlen(left);
    if (left_length != right.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t i = 0; i < left_length; ++i) {
        difference |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return difference == 0;
}

void initialize_authorization_value()
{
    const std::string credentials =
        std::string(app_config::kWebUsername) + ":" + app_config::kWebPassword;
    unsigned char encoded[192] = {};
    std::size_t encoded_length = 0;
    const int result = mbedtls_base64_encode(
        encoded, sizeof(encoded), &encoded_length,
        reinterpret_cast<const unsigned char *>(credentials.data()),
        credentials.size());
    if (result == 0) {
        s_expected_authorization = "Basic ";
        s_expected_authorization.append(
            reinterpret_cast<const char *>(encoded), encoded_length);
    }
}

bool authenticate(httpd_req_t *request)
{
    char authorization[256] = {};
    const esp_err_t result = httpd_req_get_hdr_value_str(
        request, "Authorization", authorization, sizeof(authorization));
    if (result == ESP_OK &&
        constant_time_equal(authorization, s_expected_authorization)) {
        return true;
    }

    httpd_resp_set_status(request, "401 Unauthorized");
    httpd_resp_set_hdr(request, "WWW-Authenticate",
                       "Basic realm=\"Kylio Robot\"");
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_sendstr(request, "Authentication required");
    return false;
}

bool query_value(httpd_req_t *request, const char *key, char *value,
                 std::size_t value_size)
{
    char query[160] = {};
    return httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, key, value, value_size) == ESP_OK;
}

bool accept_command_sequence(const char *value)
{
    errno = 0;
    char *end = nullptr;
    const uint64_t sequence = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || sequence == 0) {
        return false;
    }

    portENTER_CRITICAL(&s_command_sequence_lock);
    const bool accepted = sequence > s_last_command_sequence;
    if (accepted) {
        s_last_command_sequence = sequence;
    }
    portEXIT_CRITICAL(&s_command_sequence_lock);
    return accepted;
}

bool parse_hold_id(const char *value, uint64_t *hold_id)
{
    errno = 0;
    char *end = nullptr;
    const uint64_t parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0) {
        return false;
    }
    *hold_id = parsed;
    return true;
}

esp_err_t send_text(httpd_req_t *request, const char *status,
                    const char *message)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "text/plain");
    return httpd_resp_sendstr(request, message);
}

esp_err_t root_handler(httpd_req_t *request)
{
    if (!authenticate(request)) {
        return ESP_OK;
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, kWebPage, HTTPD_RESP_USE_STRLEN);
}

MotorMotion direction_to_motion(const char *direction, bool *valid)
{
    *valid = true;
    if (std::strcmp(direction, "f") == 0) return MotorMotion::Forward;
    if (std::strcmp(direction, "b") == 0) return MotorMotion::Backward;
    if (std::strcmp(direction, "l") == 0) return MotorMotion::Left;
    if (std::strcmp(direction, "r") == 0) return MotorMotion::Right;
    if (std::strcmp(direction, "s") == 0) return MotorMotion::Stop;
    *valid = false;
    return MotorMotion::Stop;
}

esp_err_t command_handler(httpd_req_t *request)
{
    if (!authenticate(request)) return ESP_OK;

    char direction[8] = {};
    char state_value[8] = {};
    char sequence[24] = {};
    char hold_value[24] = {};
    if (!query_value(request, "dir", direction, sizeof(direction)) ||
        !query_value(request, "state", state_value, sizeof(state_value)) ||
        !query_value(request, "seq", sequence, sizeof(sequence))) {
        return send_text(request, "400 Bad Request", "Missing dir, state, or seq");
    }

    bool valid = false;
    const MotorMotion motion = direction_to_motion(direction, &valid);
    if (!valid || (std::strcmp(state_value, "0") != 0 &&
                   std::strcmp(state_value, "1") != 0)) {
        return send_text(request, "400 Bad Request", "Invalid command");
    }
    if (!accept_command_sequence(sequence)) {
        return send_text(request, "409 Conflict", "Stale command ignored");
    }

    if (motion == MotorMotion::Stop) {
        robot_controller_emergency_stop();
        return send_text(request, "200 OK", "OK");
    }
    uint64_t hold_id = 0;
    if (!query_value(request, "hold", hold_value, sizeof(hold_value)) ||
        !parse_hold_id(hold_value, &hold_id)) {
        return send_text(request, "400 Bad Request", "Invalid hold ID");
    }
    if (!robot_controller_set_manual_command(
            motion, std::strcmp(state_value, "1") == 0, hold_id)) {
        return send_text(request, "409 Conflict", "Press was stopped");
    }
    return send_text(request, "200 OK", "OK");
}

esp_err_t state_handler(httpd_req_t *request)
{
    if (!authenticate(request)) return ESP_OK;
    const RobotSnapshot state = robot_controller_snapshot();
    char response[256];
    std::snprintf(
        response, sizeof(response),
        "{\"motion\":\"%s\",\"wifiConnected\":%s,"
        "\"ip\":\"%s\",\"rssi\":%d}",
        motor_motion_name(state.motion),
        wifi_manager_is_connected() ? "true" : "false",
        wifi_manager_ip_address().c_str(), wifi_manager_rssi());
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

esp_err_t not_found_handler(httpd_req_t *request, httpd_err_code_t)
{
    if (!authenticate(request)) return ESP_OK;
    return send_text(request, "404 Not Found", "Not found");
}

esp_err_t favicon_handler(httpd_req_t *request)
{
    if (!authenticate(request)) return ESP_OK;
    httpd_resp_set_status(request, "204 No Content");
    return httpd_resp_send(request, nullptr, 0);
}

void register_get(const char *uri, esp_err_t (*handler)(httpd_req_t *))
{
    const httpd_uri_t route = {
        .uri = uri,
        .method = HTTP_GET,
        .handler = handler,
        .user_ctx = nullptr,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &route));
}

}  // namespace

esp_err_t web_server_start()
{
    if (std::strcmp(app_config::kWebPassword, "change-me") == 0) {
        ESP_LOGE(kTag, "Refusing to start with the default web password");
        return ESP_ERR_INVALID_STATE;
    }
    initialize_authorization_value();
    if (s_expected_authorization.empty()) {
        return ESP_FAIL;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    const esp_err_t result = httpd_start(&s_server, &config);
    if (result != ESP_OK) {
        return result;
    }

    register_get("/", root_handler);
    register_get("/cmd", command_handler);
    register_get("/api/state", state_handler);
    register_get("/favicon.ico", favicon_handler);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND,
                               not_found_handler);
    ESP_LOGI(kTag, "HTTP server started");
    return ESP_OK;
}
