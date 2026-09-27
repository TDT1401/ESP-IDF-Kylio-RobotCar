#pragma once

// All human-readable Telegram messages live here. Edit the text and format
// strings below without changing the Telegram client or robot-control logic.

#include <cstddef>
#include <cstdio>

#include "robot_controller.h"

namespace telegram_messages {

// Canonical commands remain available for typed input. Friendly labels from
// the previous reply keyboard are also accepted for backward compatibility.
inline constexpr char kCommandFollow[] = "/follow";
inline constexpr char kCommandObstacle[] = "/obstacle";
inline constexpr char kCommandRoom[] = "/room";
inline constexpr char kCommandStatus[] = "/status";
inline constexpr char kCommandSettings[] = "/settings";
inline constexpr char kCommandStop[] = "/stop";
inline constexpr char kCommandSettingFollow[] = "/setting_follow";
inline constexpr char kCommandSettingAvoid[] = "/setting_avoid";
inline constexpr char kCommandMenu[] = "/menu";

inline constexpr char kButtonFollow[] = "👣 Follow Mode";
inline constexpr char kButtonObstacle[] = "🛡️ Obstacle Mode";
inline constexpr char kButtonRoom[] = "🏠 Room Monitor";
inline constexpr char kButtonStatus[] = "📊 Trạng thái";
inline constexpr char kButtonSettings[] = "⚙️ Cài đặt";
inline constexpr char kButtonStop[] = "🛑 Dừng robot";
inline constexpr char kButtonSettingFollow[] = "📏 Khoảng cách Follow";
inline constexpr char kButtonSettingAvoid[] = "📐 Khoảng cách Avoid";
inline constexpr char kButtonMenu[] = "⬅️ Menu chính";

inline constexpr char kMenuTitle[] =
    "🤖 <b>KYLIO ROBOT</b>\n\nChọn chức năng bên dưới:";
inline constexpr char kSettingsMenuTitle[] =
    "⚙️ <b>CÀI ĐẶT KHOẢNG CÁCH</b>\n\n"
    "Chọn thông số cần điều chỉnh:";

inline constexpr char kInvalidDistance[] =
    "⚠️ <b>Giá trị không hợp lệ</b>\n\n"
    "Khoảng cách phải là một số từ <b>10–200 cm</b>.\n"
    "Hãy nhập lại hoặc chọn <b>Menu chính</b> để thoát.";
inline constexpr char kStopped[] =
    "🛑 <b>Robot đã dừng</b>\n\nChế độ đã chuyển về Manual.";
inline constexpr char kUnknownCommand[] =
    "❓ <b>Không nhận diện được lệnh</b>\n\n"
    "Chọn một chức năng trên bàn phím hoặc dùng /menu.";

inline constexpr char kRoomMonitorUnavailable[] =
    "⚠️ <b>Không thể bật Room Monitor</b>\n\n"
    "Cảm biến siêu âm không phản hồi.";
inline constexpr char kRoomMonitorOff[] =
    "🏠 <b>Room Monitor đã tắt</b>";
inline constexpr char kTelegramControlOn[] =
    "✅ <b>Telegram Control đã bật</b>\n\n"
    "Gửi /menu để mở bảng điều khiển.";
inline constexpr char kTelegramControlOff[] =
    "🔕 <b>Telegram Control đã tắt</b>";

inline const char *mode_display_name(RobotMode mode)
{
    switch (mode) {
    case RobotMode::Obstacle: return "Tránh vật cản";
    case RobotMode::Follow: return "Bám theo";
    case RobotMode::RoomMonitor: return "Giám sát phòng";
    case RobotMode::Manual:
    default: return "Manual";
    }
}

inline const char *motion_display_name(MotorMotion motion)
{
    switch (motion) {
    case MotorMotion::Forward: return "Tiến";
    case MotorMotion::Backward: return "Lùi";
    case MotorMotion::Left: return "Rẽ trái";
    case MotorMotion::Right: return "Rẽ phải";
    case MotorMotion::ForwardLeft: return "Tiến trái";
    case MotorMotion::ForwardRight: return "Tiến phải";
    case MotorMotion::BackLeft: return "Lùi trái";
    case MotorMotion::BackRight: return "Lùi phải";
    case MotorMotion::RotateLeft: return "Xoay trái";
    case MotorMotion::RotateRight: return "Xoay phải";
    case MotorMotion::Stop:
    default: return "Đã dừng";
    }
}

inline const char *room_monitor_display_name(const RobotSnapshot &snapshot)
{
    if (snapshot.mode != RobotMode::RoomMonitor) {
        return "Tắt";
    }
    if (snapshot.room_calibrating) {
        return "Đang hiệu chuẩn";
    }
    return snapshot.room_intruder_detected ? "Phát hiện thay đổi" : "Sẵn sàng";
}

inline void format_startup(char *out, std::size_t out_size, const char *ip)
{
    std::snprintf(
        out, out_size,
        "🤖 <b>Kylio đã khởi động</b>\n\n"
        "🌐 <a href=\"http://%s\">Mở Web Control</a>",
        ip);
}

inline void format_status(char *out, std::size_t out_size,
                          const RobotSnapshot &snapshot, int wifi_rssi,
                          const char *ip)
{
    char distance[48];
    if (snapshot.distance_valid) {
        std::snprintf(distance, sizeof(distance), "%.1f cm",
                      snapshot.distance_cm);
    } else {
        std::snprintf(distance, sizeof(distance), "Không có dữ liệu");
    }

    std::snprintf(
        out, out_size,
        "🤖 <b>TRẠNG THÁI KYLIO</b>\n\n"
        "⚙️ <b>Chế độ:</b> %s\n"
        "🧭 <b>Chuyển động:</b> %s\n"
        "📏 <b>Khoảng cách:</b> %s\n"
        "📡 <b>Cảm biến HC-SR04:</b> %s\n\n"
        "<b>Cài đặt khoảng cách</b>\n"
        "• Follow: <code>%.1f cm</code>\n"
        "• Avoid: <code>%.1f cm</code>\n\n"
        "🏠 <b>Room Monitor:</b> %s\n"
        "📶 <b>Wi-Fi:</b> <code>%d dBm</code>\n"
        "🌐 <b>IP:</b> <code>%s</code>",
        mode_display_name(snapshot.mode), motion_display_name(snapshot.motion),
        distance, snapshot.distance_valid ? "Hoạt động" : "Lỗi",
        snapshot.follow_distance_cm, snapshot.obstacle_distance_cm,
        room_monitor_display_name(snapshot), wifi_rssi, ip);
}

inline void format_settings_panel(char *out, std::size_t out_size,
                                  const RobotSnapshot &snapshot)
{
    std::snprintf(
        out, out_size,
        "⚙️ <b>CÀI ĐẶT KHOẢNG CÁCH</b>\n\n"
        "📏 <b>Follow:</b> <code>%.1f cm</code>\n"
        "📐 <b>Avoid:</b> <code>%.1f cm</code>\n\n"
        "Chọn thông số cần điều chỉnh:",
        snapshot.follow_distance_cm, snapshot.obstacle_distance_cm);
}

inline void format_distance_panel(char *out, std::size_t out_size,
                                  bool follow_distance, float distance)
{
    std::snprintf(
        out, out_size,
        "%s <b>KHOẢNG CÁCH %s</b>\n\n"
        "Giá trị hiện tại: <code>%.1f cm</code>\n"
        "Giới hạn: <b>10–200 cm</b>\n\n"
        "Dùng các nút bên dưới hoặc chọn <b>Nhập số</b>.",
        follow_distance ? "📏" : "📐",
        follow_distance ? "FOLLOW" : "AVOID", distance);
}

inline void format_mode_confirmation(char *out, std::size_t out_size,
                                     RobotMode mode)
{
    std::snprintf(
        out, out_size,
        "⚠️ <b>XÁC NHẬN CHUYỂN CHẾ ĐỘ</b>\n\n"
        "Bạn sắp bật <b>%s</b>.\n"
        "Robot có thể bắt đầu di chuyển ngay sau khi xác nhận.",
        mode_display_name(mode));
}

inline void format_distance_confirmation(char *out, std::size_t out_size,
                                         bool follow_distance, float distance)
{
    std::snprintf(out, out_size,
                  "✅ <b>Đã lưu cài đặt</b>\n\n"
                  "%s: <code>%.1f cm</code>",
                  follow_distance ? "Khoảng cách Follow"
                                  : "Khoảng cách Avoid",
                  distance);
}

inline void format_follow_distance_prompt(char *out, std::size_t out_size,
                                          float current_distance)
{
    std::snprintf(out, out_size,
                  "📏 <b>Khoảng cách Follow</b>\n\n"
                  "Hiện tại: <code>%.1f cm</code>\n"
                  "Nhập giá trị mới từ <b>10–200 cm</b>.",
                  current_distance);
}

inline void format_avoid_distance_prompt(char *out, std::size_t out_size,
                                         float current_distance)
{
    std::snprintf(out, out_size,
                  "📐 <b>Khoảng cách Avoid</b>\n\n"
                  "Hiện tại: <code>%.1f cm</code>\n"
                  "Nhập giá trị mới từ <b>10–200 cm</b>.",
                  current_distance);
}

inline void format_room_monitor_started(char *out, std::size_t out_size,
                                        float baseline_distance)
{
    std::snprintf(out, out_size,
                  "🏠 <b>Room Monitor đã bật</b>\n\n"
                  "Khoảng cách ban đầu: <code>%.1f cm</code>",
                  baseline_distance);
}

inline void format_room_alert(char *out, std::size_t out_size,
                              float distance)
{
    std::snprintf(out, out_size,
                  "🚨 <b>CẢNH BÁO PHÒNG</b>\n\n"
                  "Phát hiện thay đổi trong phòng.\n"
                  "Khoảng cách: <code>%.1f cm</code>",
                  distance);
}

}  // namespace telegram_messages
