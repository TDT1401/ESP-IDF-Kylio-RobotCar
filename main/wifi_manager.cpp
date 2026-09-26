#include "wifi_manager.h"

#include <cstdio>
#include <cstring>

#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

namespace {

constexpr const char *kTag = "wifi";

std::size_t s_network_index = 0;
uint32_t s_retry_count = 0;
bool s_connected = false;
char s_ip_address[16] = "0.0.0.0";
portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

bool network_is_configured(std::size_t index)
{
    return index < app_config::kWifiNetworkCount &&
           app_config::kWifiNetworks[index].ssid != nullptr &&
           app_config::kWifiNetworks[index].ssid[0] != '\0';
}

bool select_first_network()
{
    for (std::size_t i = 0; i < app_config::kWifiNetworkCount; ++i) {
        if (network_is_configured(i)) {
            s_network_index = i;
            return true;
        }
    }
    return false;
}

bool select_next_network()
{
    if (app_config::kWifiNetworkCount == 0) {
        return false;
    }
    for (std::size_t offset = 1; offset <= app_config::kWifiNetworkCount;
         ++offset) {
        const std::size_t candidate =
            (s_network_index + offset) % app_config::kWifiNetworkCount;
        if (network_is_configured(candidate)) {
            s_network_index = candidate;
            return true;
        }
    }
    return false;
}

esp_err_t connect_current_network()
{
    if (!network_is_configured(s_network_index)) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t config = {};
    std::snprintf(reinterpret_cast<char *>(config.sta.ssid),
                  sizeof(config.sta.ssid), "%s",
                  app_config::kWifiNetworks[s_network_index].ssid);
    std::snprintf(reinterpret_cast<char *>(config.sta.password),
                  sizeof(config.sta.password), "%s",
                  app_config::kWifiNetworks[s_network_index].password);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    ESP_LOGI(kTag, "Connecting to Wi-Fi network %u: %s",
             static_cast<unsigned>(s_network_index + 1),
             app_config::kWifiNetworks[s_network_index].ssid);

    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result != ESP_OK) {
        return result;
    }
    return esp_wifi_connect();
}

void event_handler(void *, esp_event_base_t event_base, int32_t event_id,
                   void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(connect_current_network());
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
        portENTER_CRITICAL(&s_state_lock);
        s_connected = false;
        std::snprintf(s_ip_address, sizeof(s_ip_address), "0.0.0.0");
        portEXIT_CRITICAL(&s_state_lock);

        if (++s_retry_count >= app_config::kWifiRetriesPerNetwork) {
            s_retry_count = 0;
            select_next_network();
        }
        ESP_LOGW(kTag, "Wi-Fi disconnected (reason=%u); reconnecting",
                 static_cast<unsigned>(event->reason));
        ESP_ERROR_CHECK_WITHOUT_ABORT(connect_current_network());
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        char address[16];
        std::snprintf(address, sizeof(address), IPSTR, IP2STR(&event->ip_info.ip));

        portENTER_CRITICAL(&s_state_lock);
        s_connected = true;
        std::snprintf(s_ip_address, sizeof(s_ip_address), "%s", address);
        portEXIT_CRITICAL(&s_state_lock);
        s_retry_count = 0;
        ESP_LOGI(kTag, "Connected; open http://%s", address);
    }
}

}  // namespace

esp_err_t wifi_manager_start()
{
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK) {
        return result;
    }
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    if (!select_first_network()) {
        ESP_LOGE(kTag,
                 "No Wi-Fi network configured. Create main/secrets.local.h");
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_netif_create_default_wifi_sta() == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&init_config);
    if (result != ESP_OK) {
        return result;
    }

    result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &event_handler, nullptr);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &event_handler, nullptr);
    if (result != ESP_OK) {
        return result;
    }

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    return esp_wifi_start();
}

bool wifi_manager_is_connected()
{
    portENTER_CRITICAL(&s_state_lock);
    const bool connected = s_connected;
    portEXIT_CRITICAL(&s_state_lock);
    return connected;
}

std::string wifi_manager_ip_address()
{
    char address[sizeof(s_ip_address)];
    portENTER_CRITICAL(&s_state_lock);
    std::memcpy(address, s_ip_address, sizeof(address));
    portEXIT_CRITICAL(&s_state_lock);
    address[sizeof(address) - 1] = '\0';
    return address;
}

int wifi_manager_rssi()
{
    if (!wifi_manager_is_connected()) {
        return 0;
    }
    wifi_ap_record_t access_point = {};
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
        return 0;
    }
    return access_point.rssi;
}
