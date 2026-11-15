#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"

struct WifiCredential {
    const char *ssid;
    const char *password;
};

#if __has_include("secrets.local.h")
#include "secrets.local.h"
#endif

#ifndef KYLIO_WIFI_NETWORKS
#define KYLIO_WIFI_NETWORKS {{"", ""}}
#endif

#ifndef KYLIO_TELEGRAM_BOT_TOKEN
#define KYLIO_TELEGRAM_BOT_TOKEN ""
#endif

#ifndef KYLIO_TELEGRAM_CHAT_ID
#define KYLIO_TELEGRAM_CHAT_ID ""
#endif

#ifndef KYLIO_WEB_USERNAME
#define KYLIO_WEB_USERNAME "kylio"
#endif

#ifndef KYLIO_WEB_PASSWORD
#define KYLIO_WEB_PASSWORD "change-me"
#endif

namespace app_config {

// One L298N: channel A drives both left motors, channel B both right motors.
inline constexpr gpio_num_t kLeftIn1 = GPIO_NUM_13;
inline constexpr gpio_num_t kLeftIn2 = GPIO_NUM_12;
inline constexpr gpio_num_t kRightIn3 = GPIO_NUM_14;
inline constexpr gpio_num_t kRightIn4 = GPIO_NUM_27;

inline constexpr uint32_t kControllerPeriodMs = 20;
inline constexpr uint32_t kManualCommandTimeoutMs = 350;
inline constexpr WifiCredential kWifiNetworks[] = KYLIO_WIFI_NETWORKS;
inline constexpr std::size_t kWifiNetworkCount =
    sizeof(kWifiNetworks) / sizeof(kWifiNetworks[0]);
inline constexpr uint32_t kWifiRetriesPerNetwork = 3;
inline constexpr uint32_t kWifiReconnectInitialMs = 1000;
inline constexpr uint32_t kWifiReconnectMaxMs = 10000;

inline constexpr const char *kTelegramBotToken = KYLIO_TELEGRAM_BOT_TOKEN;
inline constexpr const char *kTelegramChatId = KYLIO_TELEGRAM_CHAT_ID;
inline constexpr uint32_t kTelegramRetryInitialMs = 1000;
inline constexpr uint32_t kTelegramRetryMaxMs = 30000;

inline constexpr const char *kWebUsername = KYLIO_WEB_USERNAME;
inline constexpr const char *kWebPassword = KYLIO_WEB_PASSWORD;

}  // namespace app_config
