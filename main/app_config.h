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

inline constexpr gpio_num_t kFrontLeftA = GPIO_NUM_13;
inline constexpr gpio_num_t kFrontLeftB = GPIO_NUM_12;
inline constexpr gpio_num_t kFrontRightA = GPIO_NUM_14;
inline constexpr gpio_num_t kFrontRightB = GPIO_NUM_27;
inline constexpr gpio_num_t kBackLeftA = GPIO_NUM_26;
inline constexpr gpio_num_t kBackLeftB = GPIO_NUM_25;
inline constexpr gpio_num_t kBackRightA = GPIO_NUM_33;
inline constexpr gpio_num_t kBackRightB = GPIO_NUM_32;

inline constexpr gpio_num_t kUltrasonicTrig = GPIO_NUM_4;
inline constexpr gpio_num_t kUltrasonicEcho = GPIO_NUM_5;
inline constexpr uint32_t kUltrasonicTimeoutUs = 30000;
inline constexpr uint32_t kUltrasonicMinIntervalMs = 70;

inline constexpr uint32_t kControllerPeriodMs = 20;
inline constexpr uint32_t kManualCommandTimeoutMs = 750;
inline constexpr float kManualStopDistanceCm = 55.0F;
inline constexpr uint32_t kObstacleStopMs = 1000;
inline constexpr uint32_t kObstacleBackMs = 1500;
inline constexpr uint32_t kObstacleTurnMs = 700;
inline constexpr uint32_t kRoomCalibrationMs = 500;
inline constexpr uint32_t kRoomCheckMs = 300;
inline constexpr uint32_t kRoomClearMs = 3000;
inline constexpr float kRoomChangeCm = 100.0F;

inline constexpr WifiCredential kWifiNetworks[] = KYLIO_WIFI_NETWORKS;
inline constexpr std::size_t kWifiNetworkCount =
    sizeof(kWifiNetworks) / sizeof(kWifiNetworks[0]);
inline constexpr uint32_t kWifiRetriesPerNetwork = 3;

inline constexpr const char *kTelegramBotToken = KYLIO_TELEGRAM_BOT_TOKEN;
inline constexpr const char *kTelegramChatId = KYLIO_TELEGRAM_CHAT_ID;
inline constexpr uint32_t kTelegramPollMs = 1000;

inline constexpr const char *kWebUsername = KYLIO_WEB_USERNAME;
inline constexpr const char *kWebPassword = KYLIO_WEB_PASSWORD;

}  // namespace app_config
