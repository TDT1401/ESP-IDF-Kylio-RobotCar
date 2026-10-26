#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

struct BluetoothDeviceInfo {
    std::size_t id;
    char name[64];
    char address[18];
    char transport[20];
    char usage[20];
    int rssi;
};

struct BluetoothGamepadStatus {
    bool connected;
    bool scanning;
    bool connecting;
    std::uint32_t input_reports;
    std::uint32_t decoded_reports;
    std::uint32_t notify_registrations;
    std::uint32_t notify_failures;
    char message[96];
};

esp_err_t bluetooth_gamepad_start();
esp_err_t bluetooth_gamepad_scan();
esp_err_t bluetooth_gamepad_connect(std::size_t device_id);
esp_err_t bluetooth_gamepad_disconnect();
BluetoothGamepadStatus bluetooth_gamepad_status();
std::size_t bluetooth_gamepad_device_count();
bool bluetooth_gamepad_device_get(std::size_t index, BluetoothDeviceInfo *out);
