#pragma once

// All human-readable Telegram messages live here. Edit the text and format
// strings below without changing the Telegram client or robot-control logic.

#include <cstddef>
#include <cstdio>

#include "robot_controller.h"

namespace telegram_messages {

// These are Telegram command labels used by the on-screen keyboards. Keep the
// leading '/' so Telegram treats them as commands.
inline constexpr char kCommandFollow[] = "/follow";
inline constexpr char kCommandObstacle[] = "/obstacle";
inline constexpr char kCommandRoom[] = "/room";
inline constexpr char kCommandStatus[] = "/status";
inline constexpr char kCommandSettings[] = "/settings";
inline constexpr char kCommandStop[] = "/stop";
inline constexpr char kCommandSettingFollow[] = "/setting_follow";
inline constexpr char kCommandSettingAvoid[] = "/setting_avoid";
inline constexpr char kCommandMenu[] = "/menu";

inline constexpr char kMenuTitle[] = "KYLIO ROBOT MENU";
inline constexpr char kSettingsMenuTitle[] = "ROBOT SETTINGS";

inline constexpr char kInvalidDistance[] =
    "Khoang cach phai la mot so tu 10 den 200 cm. "
    "Nhap lai hoac dung /menu de thoat.";
inline constexpr char kStopped[] = "Robot da dung. Mode da chuyen ve Manual.";
inline constexpr char kFollowWebControl[] =
    "Follow Mode duoc bat/tat tren Web Control.";
inline constexpr char kObstacleWebControl[] =
    "Obstacle Mode duoc bat/tat tren Web Control.";
inline constexpr char kRoomWebControl[] =
    "Room Monitor duoc bat/tat tren Web Control.";
inline constexpr char kUnknownCommand[] = "Lenh khong hop le. Dung /menu.";

inline constexpr char kRoomMonitorUnavailable[] =
    "Khong the bat Room Monitor: cam bien sieu am khong phan hoi.";
inline constexpr char kRoomMonitorOff[] = "Room Monitor OFF";

inline void format_startup(char *out, std::size_t out_size, const char *ip)
{
    std::snprintf(out, out_size, "Kylio da khoi dong. Web Control: http://%s",
                  ip);
}

inline void format_status(char *out, std::size_t out_size,
                          const RobotSnapshot &snapshot, int wifi_rssi,
                          const char *ip)
{
    std::snprintf(
        out, out_size,
        "KYLIO ROBOT STATUS\n\n"
        "Mode: %s\nMotion: %s\nDistance: %s%.1f%s\n"
        "HC-SR04: %s\nFollow distance: %.1f cm\n"
        "Avoid distance: %.1f cm\nRoom intruder: %s\n"
        "Wi-Fi RSSI: %d dBm\nIP: %s",
        robot_mode_name(snapshot.mode), motor_motion_name(snapshot.motion),
        snapshot.distance_valid ? "" : "ERROR (",
        snapshot.distance_valid ? snapshot.distance_cm : 0.0F,
        snapshot.distance_valid ? " cm" : ")",
        snapshot.distance_valid ? "OK" : "ERROR",
        snapshot.follow_distance_cm, snapshot.obstacle_distance_cm,
        snapshot.room_intruder_detected ? "YES" : "NO", wifi_rssi, ip);
}

inline void format_distance_confirmation(char *out, std::size_t out_size,
                                         bool follow_distance, float distance)
{
    std::snprintf(out, out_size, "%s distance = %.1f cm",
                  follow_distance ? "Follow" : "Avoid", distance);
}

inline void format_follow_distance_prompt(char *out, std::size_t out_size,
                                          float current_distance)
{
    std::snprintf(out, out_size,
                  "Nhap Follow distance (10-200 cm). Current: %.1f cm",
                  current_distance);
}

inline void format_avoid_distance_prompt(char *out, std::size_t out_size,
                                         float current_distance)
{
    std::snprintf(out, out_size,
                  "Nhap Avoid distance (10-200 cm). Current: %.1f cm",
                  current_distance);
}

inline void format_room_monitor_started(char *out, std::size_t out_size,
                                        float baseline_distance)
{
    std::snprintf(out, out_size,
                  "Room Monitor ON\nKhoang cach ban dau: %.1f cm",
                  baseline_distance);
}

inline void format_room_alert(char *out, std::size_t out_size,
                              float distance)
{
    std::snprintf(out, out_size,
                  "CANH BAO! Phat hien thay doi trong phong. "
                  "Khoang cach: %.1f cm",
                  distance);
}

}  // namespace telegram_messages
