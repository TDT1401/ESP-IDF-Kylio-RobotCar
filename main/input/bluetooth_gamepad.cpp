#include "bluetooth_gamepad.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "esp_bt.h"
#include "esp_event.h"
#include "esp_hid_gap.h"
#include "esp_hidh.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "control/robot_controller.h"

#if CONFIG_BT_BLE_ENABLED
#include "esp_gattc_api.h"
#endif

namespace {

constexpr char kTag[] = "gamepad";
constexpr uint32_t kScanSeconds = 6;
constexpr std::size_t kMaxFields = 64;
constexpr std::size_t kMaxLocalUsages = 32;
constexpr std::size_t kMaxDevices = 32;

enum class TaskRequest { None, Scan, Connect };

struct HidField {
    uint8_t report_id;
    uint16_t bit_offset;
    uint8_t bit_size;
    uint16_t usage_page;
    uint16_t usage;
    int32_t logical_min;
    int32_t logical_max;
};

struct GamepadLayout {
    std::array<HidField, kMaxFields> fields{};
    std::size_t field_count = 0;
    std::array<uint16_t, 256> report_bits{};
    int axis_x = 0;
    int axis_y = 0;
    int hat = -1;
    bool has_axis_x = false;
    bool has_axis_y = false;
    bool has_hat = false;
    bool valid = false;
};

struct GlobalState {
    uint16_t usage_page = 0;
    int32_t logical_min = 0;
    int32_t logical_max = 0;
    uint8_t report_size = 0;
    uint8_t report_id = 0;
    uint16_t report_count = 0;
};

struct LocalState {
    std::array<uint16_t, kMaxLocalUsages> usages{};
    std::size_t usage_count = 0;
    uint16_t usage_min = 0;
    bool has_usage_min = false;

    void clear()
    {
        usage_count = 0;
        usage_min = 0;
        has_usage_min = false;
    }
};

struct DeviceRecord {
    BluetoothDeviceInfo info{};
    std::array<uint8_t, 6> address{};
    esp_hid_transport_t transport = ESP_HID_TRANSPORT_MAX;
    uint8_t address_type = 0;
};

portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t s_pair_task = nullptr;
esp_hidh_dev_t *s_device = nullptr;
std::array<DeviceRecord, kMaxDevices> s_devices{};
std::size_t s_device_count = 0;
DeviceRecord s_requested_device{};
TaskRequest s_task_request = TaskRequest::None;
bool s_request_pending = false;
GamepadLayout s_layout;
int s_axis_x = 0;
int s_axis_y = 0;
int s_hat = -1;
bool s_ultimate_2c = false;
uint32_t s_input_report_count = 0;
uint32_t s_decoded_report_count = 0;
uint32_t s_notify_registrations = 0;
uint32_t s_notify_failures = 0;
bool s_logged_movement = false;
bool s_started = false;
bool s_connected = false;
bool s_scanning = false;
bool s_connecting = false;
char s_message[96] = "Bluetooth ready. Open Bluetooth Devices to scan.";

int32_t signed_value(uint32_t value, uint8_t size)
{
    if (size == 0 || size >= 32) return static_cast<int32_t>(value);
    const uint32_t sign = 1u << (size - 1);
    return static_cast<int32_t>((value ^ sign) - sign);
}

uint16_t item_value(const uint8_t *data, uint8_t size)
{
    uint16_t value = 0;
    for (uint8_t i = 0; i < size; ++i) {
        value |= static_cast<uint16_t>(data[i]) << (8 * i);
    }
    return value;
}

void add_input_fields(GamepadLayout *layout, const GlobalState &global,
                      const LocalState &local, uint8_t flags)
{
    const uint16_t start = layout->report_bits[global.report_id];
    const uint32_t total_bits =
        static_cast<uint32_t>(global.report_size) * global.report_count;
    if ((flags & 0x01) == 0 && (flags & 0x02) != 0 &&
        global.report_size != 0) {
        for (uint16_t i = 0; i < global.report_count &&
                             layout->field_count < layout->fields.size(); ++i) {
            uint16_t usage = 0;
            if (i < local.usage_count) {
                usage = local.usages[i];
            } else if (local.has_usage_min) {
                usage = static_cast<uint16_t>(local.usage_min + i);
            } else if (local.usage_count != 0) {
                usage = local.usages[local.usage_count - 1];
            }
            HidField &field = layout->fields[layout->field_count++];
            field.report_id = global.report_id;
            field.bit_offset = static_cast<uint16_t>(start + i * global.report_size);
            field.bit_size = global.report_size;
            field.usage_page = global.usage_page;
            field.usage = usage;
            field.logical_min = global.logical_min;
            field.logical_max = global.logical_max;
        }
    }
    layout->report_bits[global.report_id] = static_cast<uint16_t>(
        std::min<uint32_t>(start + total_bits, UINT16_MAX));
}

bool parse_gamepad_report_map(const uint8_t *data, std::size_t length,
                              GamepadLayout *layout)
{
    *layout = GamepadLayout{};
    GlobalState global{};
    std::array<GlobalState, 4> global_stack{};
    std::size_t global_stack_size = 0;
    LocalState local{};

    for (std::size_t i = 0; i < length;) {
        const uint8_t prefix = data[i++];
        if (prefix == 0xFE) {
            if (i + 2 > length) return false;
            const uint8_t long_length = data[i++];
            ++i;  // long-item tag
            if (i + long_length > length) return false;
            i += long_length;
            continue;
        }
        const uint8_t encoded_size = prefix & 0x03;
        const uint8_t size = encoded_size == 3 ? 4 : encoded_size;
        if (i + size > length) return false;
        const uint32_t value = item_value(data + i, size);
        const uint8_t type = (prefix >> 2) & 0x03;
        const uint8_t tag = (prefix >> 4) & 0x0F;
        i += size;

        if (type == 1) {  // Global item
            switch (tag) {
            case 0: global.usage_page = static_cast<uint16_t>(value); break;
            case 1: global.logical_min = signed_value(value, size * 8); break;
            case 2:
                global.logical_max = global.logical_min < 0
                    ? signed_value(value, size * 8)
                    : static_cast<int32_t>(value);
                break;
            case 7: global.report_size = static_cast<uint8_t>(value); break;
            case 8: global.report_id = static_cast<uint8_t>(value); break;
            case 9: global.report_count = static_cast<uint16_t>(value); break;
            case 10:
                if (global_stack_size < global_stack.size()) {
                    global_stack[global_stack_size++] = global;
                }
                break;
            case 11:
                if (global_stack_size != 0) global = global_stack[--global_stack_size];
                break;
            default: break;
            }
        } else if (type == 2) {  // Local item
            switch (tag) {
            case 0:
                if (local.usage_count < local.usages.size()) {
                    local.usages[local.usage_count++] = static_cast<uint16_t>(value);
                }
                break;
            case 1:
                local.usage_min = static_cast<uint16_t>(value);
                local.has_usage_min = true;
                break;
            default: break;
            }
        } else if (type == 0) {  // Main item
            if (tag == 8) add_input_fields(layout, global, local,
                                           static_cast<uint8_t>(value));
            local.clear();
        }
    }

    for (std::size_t i = 0; i < layout->field_count; ++i) {
        const HidField &field = layout->fields[i];
        if (field.usage_page == 1 && field.usage == 0x30 &&
            !layout->has_axis_x) {
            layout->axis_x = static_cast<int>(i);
            layout->has_axis_x = true;
        } else if (field.usage_page == 1 && field.usage == 0x31 &&
                   !layout->has_axis_y) {
            layout->axis_y = static_cast<int>(i);
            layout->has_axis_y = true;
        } else if (field.usage_page == 1 && field.usage == 0x39 &&
                   !layout->has_hat) {
            layout->hat = static_cast<int>(i);
            layout->has_hat = true;
        }
    }
    layout->valid = layout->has_axis_x || layout->has_axis_y || layout->has_hat;
    return layout->valid;
}

bool read_field(const HidField &field, const uint8_t *data, std::size_t length,
                int32_t *value)
{
    if (field.bit_size == 0 || field.bit_size > 32 ||
        static_cast<std::size_t>(field.bit_offset) + field.bit_size > length * 8) {
        return false;
    }
    uint32_t raw = 0;
    for (uint8_t bit = 0; bit < field.bit_size; ++bit) {
        const std::size_t position = field.bit_offset + bit;
        if ((data[position / 8] & (1u << (position % 8))) != 0) {
            raw |= 1u << bit;
        }
    }
    *value = field.logical_min < 0
        ? signed_value(raw, field.bit_size)
        : static_cast<int32_t>(raw);
    return true;
}

int normalize_axis(int32_t value, const HidField &field)
{
    if (field.logical_max <= field.logical_min) return 0;
    const int64_t scaled =
        (static_cast<int64_t>(value - field.logical_min) * 200) /
        (field.logical_max - field.logical_min) - 100;
    return std::clamp(static_cast<int>(scaled), -100, 100);
}

void set_status(bool scanning, bool connecting, bool connected,
                const char *message)
{
    char copy[sizeof(s_message)] = {};
    std::snprintf(copy, sizeof(copy), "%s", message);
    portENTER_CRITICAL(&s_state_lock);
    s_scanning = scanning;
    s_connecting = connecting;
    s_connected = connected;
    std::memcpy(s_message, copy, sizeof(s_message));
    portEXIT_CRITICAL(&s_state_lock);
}

#if CONFIG_BT_BLE_ENABLED
void gamepad_gattc_event_handler(esp_gattc_cb_event_t event,
                               esp_gatt_if_t gattc_if,
                               esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_FOR_NOTIFY_EVT) {
        portENTER_CRITICAL(&s_state_lock);
        if (param->reg_for_notify.status == ESP_GATT_OK) {
            ++s_notify_registrations;
        } else {
            ++s_notify_failures;
        }
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI(kTag, "HID notify registration: handle=0x%04x status=0x%x",
                 static_cast<unsigned>(param->reg_for_notify.handle),
                 static_cast<unsigned>(param->reg_for_notify.status));
    } else if (event == ESP_GATTC_WRITE_DESCR_EVT &&
               param->write.status != ESP_GATT_OK) {
        portENTER_CRITICAL(&s_state_lock);
        ++s_notify_failures;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGW(kTag, "HID descriptor write failed: handle=0x%04x status=0x%x",
                 static_cast<unsigned>(param->write.handle),
                 static_cast<unsigned>(param->write.status));
    }
    esp_hidh_gattc_event_handler(event, gattc_if, param);
}
#endif

void apply_drive_axes(int axis_x, int axis_y, int hat)
{
    int forward = -axis_y;
    int steer = axis_x;
    if (hat >= 0) {
        constexpr int kHatForward[] = {100, 100, 0, -100, -100, -100, 0, 100};
        constexpr int kHatSteer[] = {0, 100, 100, 100, 0, -100, -100, -100};
        if (std::abs(forward) < 12 && std::abs(steer) < 12) {
            forward = kHatForward[hat];
            steer = kHatSteer[hat];
        }
    }
    const int left = std::clamp(forward + steer, -100, 100);
    const int right = std::clamp(forward - steer, -100, 100);
    const bool moving = std::abs(left) >= 12 || std::abs(right) >= 12;
    robot_controller_set_gamepad_command(left, right, moving);
}

bool apply_ultimate_2c_report(const esp_hidh_event_data_t *event)
{
    if (!s_ultimate_2c || event->input.data == nullptr ||
        (event->input.report_id != 1 && event->input.report_id != 0)) {
        return false;
    }

    // HID over GATT normally delivers the report ID separately. Accept an
    // included ID too, as some Classic HID paths include it in the payload.
    std::size_t offset = 0;
    if (event->input.length == 11 && event->input.data[0] == 1) {
        offset = 1;
    } else if (event->input.length != 10) {
        return false;
    }
    const uint8_t *data = event->input.data + offset;
    if (event->input.report_id == 0 && data[0] > 15) return false;

    // Direction values 0..7 are active; every other value is centered.
    s_hat = data[0] <= 7 ? data[0] : -1;
    s_axis_x = std::clamp((static_cast<int>(data[1]) - 127) * 100 / 128,
                          -100, 100);
    s_axis_y = std::clamp((static_cast<int>(data[2]) - 127) * 100 / 128,
                          -100, 100);
    if (!s_logged_movement &&
        (std::abs(s_axis_x) >= 12 || std::abs(s_axis_y) >= 12 || s_hat >= 0)) {
        ESP_LOGI(kTag, "8BitDo input decoded: x=%d y=%d hat=%d",
                 s_axis_x, s_axis_y, s_hat);
        s_logged_movement = true;
    }
    apply_drive_axes(s_axis_x, s_axis_y, s_hat);
    return true;
}

bool apply_gamepad_report(const esp_hidh_event_data_t *event)
{
    if (!s_layout.valid || event->input.data == nullptr ||
        event->input.length == 0) return false;
    int next_x = s_axis_x;
    int next_y = s_axis_y;
    int next_hat = s_hat;
    bool x_updated = false;
    bool y_updated = false;
    bool hat_updated = false;

    for (std::size_t i = 0; i < s_layout.field_count; ++i) {
        const HidField &field = s_layout.fields[i];
        if (field.report_id != event->input.report_id) continue;
        int32_t value = 0;
        if (!read_field(field, event->input.data, event->input.length, &value)) continue;
        if (s_layout.has_axis_x && static_cast<int>(i) == s_layout.axis_x) {
            next_x = normalize_axis(value, field);
            x_updated = true;
        } else if (s_layout.has_axis_y && static_cast<int>(i) == s_layout.axis_y) {
            next_y = normalize_axis(value, field);
            y_updated = true;
        } else if (s_layout.has_hat && static_cast<int>(i) == s_layout.hat) {
            next_hat = value >= 0 && value <= 7 ? value : -1;
            hat_updated = true;
        }
    }

    if (x_updated) s_axis_x = next_x;
    if (y_updated) s_axis_y = next_y;
    if (hat_updated) s_hat = next_hat;

    if (!x_updated && !y_updated && !hat_updated) return false;
    apply_drive_axes(s_axis_x, s_axis_y, s_hat);
    return true;
}

void hid_event_callback(void *, esp_event_base_t, int32_t event_id,
                        void *event_data)
{
    const auto event = static_cast<esp_hidh_event_t>(event_id);
    auto *param = static_cast<esp_hidh_event_data_t *>(event_data);
    if (event != ESP_HIDH_INPUT_EVENT) {
        ESP_LOGI(kTag, "HID callback event=%ld, stack minimum free=%u bytes",
                 static_cast<long>(event_id),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }
    switch (event) {
    case ESP_HIDH_OPEN_EVENT:
        if (param->open.status == ESP_OK) {
            s_device = param->open.dev;
            s_axis_x = s_axis_y = 0;
            s_hat = -1;
            portENTER_CRITICAL(&s_state_lock);
            s_input_report_count = 0;
            s_decoded_report_count = 0;
            portEXIT_CRITICAL(&s_state_lock);
            s_logged_movement = false;
            s_layout = GamepadLayout{};
            const esp_hid_device_config_t *config =
                esp_hidh_dev_config_get(param->open.dev);
            if (config != nullptr && config->report_maps_len != 0 &&
                config->report_maps != nullptr) {
                for (uint8_t i = 0; i < config->report_maps_len; ++i) {
                    if (config->report_maps[i].data == nullptr ||
                        config->report_maps[i].len == 0) {
                        continue;
                    }
                    if (parse_gamepad_report_map(config->report_maps[i].data,
                                                 config->report_maps[i].len,
                                                 &s_layout)) {
                        break;
                    }
                }
            }
            const char *device_name = esp_hidh_dev_name_get(param->open.dev);
            if (device_name == nullptr || device_name[0] == '\0') {
                device_name = "HID controller";
            }
            s_ultimate_2c =
                (config != nullptr && config->vendor_id == 0x2DC8 &&
                 config->product_id == 0x301B) ||
                std::strstr(device_name, "8BitDo Ultimate 2C") != nullptr;
            if (!s_ultimate_2c) {
                portENTER_CRITICAL(&s_state_lock);
                s_ultimate_2c = std::strstr(
                    s_requested_device.info.name, "8BitDo Ultimate 2C") != nullptr;
                portEXIT_CRITICAL(&s_state_lock);
            }
            char status[96];
            std::snprintf(status, sizeof(status),
                          "HID link open to %.48s; waiting for input",
                          device_name);
            set_status(false, false, true, status);
            ESP_LOGI(kTag, "HID connected: %s, gamepad decoder %s",
                     device_name,
                     s_ultimate_2c ? "8BitDo Ultimate 2C" :
                     s_layout.valid ? "generic" : "unrecognized");
        } else {
            s_device = nullptr;
            set_status(false, false, false, "Connection failed. Scan and retry.");
            ESP_LOGW(kTag, "HID open failed, status=%d",
                     static_cast<int>(param->open.status));
        }
        break;
    case ESP_HIDH_INPUT_EVENT: {
        portENTER_CRITICAL(&s_state_lock);
        const uint32_t report_count = ++s_input_report_count;
        portEXIT_CRITICAL(&s_state_lock);
        if (report_count <= 5) {
            ESP_LOGI(kTag, "HID input #%lu: map=%u report=%u usage=%u length=%u bytes=%02X %02X %02X %02X",
                     static_cast<unsigned long>(report_count),
                     static_cast<unsigned>(param->input.map_index),
                     static_cast<unsigned>(param->input.report_id),
                     static_cast<unsigned>(param->input.usage),
                     static_cast<unsigned>(param->input.length),
                     static_cast<unsigned>(param->input.length > 0 && param->input.data != nullptr ? param->input.data[0] : 0),
                     static_cast<unsigned>(param->input.length > 1 && param->input.data != nullptr ? param->input.data[1] : 0),
                     static_cast<unsigned>(param->input.length > 2 && param->input.data != nullptr ? param->input.data[2] : 0),
                     static_cast<unsigned>(param->input.length > 3 && param->input.data != nullptr ? param->input.data[3] : 0));
        }
        bool decoded = apply_ultimate_2c_report(param);
        if (!decoded && (param->input.usage == ESP_HID_USAGE_GAMEPAD ||
                         s_layout.valid)) {
            decoded = apply_gamepad_report(param);
        }
        if (decoded) {
            portENTER_CRITICAL(&s_state_lock);
            const bool first_control = ++s_decoded_report_count == 1;
            portEXIT_CRITICAL(&s_state_lock);
            if (first_control) {
                set_status(false, false, true, "Controller ready; input received");
            }
        } else if (report_count == 1) {
            set_status(false, false, true,
                       "HID input received; report not decoded");
        }
        break;
    }
    case ESP_HIDH_CLOSE_EVENT:
        s_device = nullptr;
        s_layout = GamepadLayout{};
        s_ultimate_2c = false;
        s_axis_x = s_axis_y = 0;
        s_hat = -1;
        robot_controller_emergency_stop();
        set_status(false, false, false, "Controller disconnected");
        break;
    default:
        break;
    }
}

void pairing_task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        TaskRequest request = TaskRequest::None;
        DeviceRecord selected{};
        portENTER_CRITICAL(&s_state_lock);
        request = s_task_request;
        if (request == TaskRequest::Connect) selected = s_requested_device;
        s_task_request = TaskRequest::None;
        s_request_pending = false;
        portEXIT_CRITICAL(&s_state_lock);

        if (request == TaskRequest::Scan) {
            std::size_t result_count = 0;
            esp_hid_scan_result_t *results = nullptr;
            ESP_LOGI(kTag, "Scanning for Bluetooth HID devices");
            const esp_err_t scan_result =
                esp_hid_scan(kScanSeconds, &result_count, &results);
            std::size_t found_count = 0;
            if (scan_result == ESP_OK) {
                for (auto *item = results; item != nullptr &&
                     found_count < kMaxDevices; item = item->next) {
                    DeviceRecord record{};
                    record.info.id = found_count;
                    std::snprintf(record.info.name, sizeof(record.info.name),
                                  "%s", item->name != nullptr ? item->name : "Unnamed HID device");
                    std::snprintf(record.info.address, sizeof(record.info.address),
                                  "%02X:%02X:%02X:%02X:%02X:%02X",
                                  item->bda[0], item->bda[1], item->bda[2],
                                  item->bda[3], item->bda[4], item->bda[5]);
                    std::snprintf(record.info.transport, sizeof(record.info.transport),
                                  "%s", item->transport == ESP_HID_TRANSPORT_BLE
                                      ? "BLE" : "Bluetooth Classic");
                    const char *usage = esp_hid_usage_str(item->usage);
                    std::snprintf(record.info.usage, sizeof(record.info.usage),
                                  "%s", usage != nullptr ? usage : "HID");
                    record.info.rssi = item->rssi;
                    std::copy(item->bda, item->bda + record.address.size(),
                              record.address.begin());
                    record.transport = item->transport;
                    record.address_type = item->transport == ESP_HID_TRANSPORT_BLE
                        ? static_cast<uint8_t>(item->ble.addr_type) : 0;
                    portENTER_CRITICAL(&s_state_lock);
                    s_devices[found_count] = record;
                    portEXIT_CRITICAL(&s_state_lock);
                    ++found_count;
                }
            }
            if (results != nullptr) esp_hid_scan_results_free(results);

            portENTER_CRITICAL(&s_state_lock);
            s_device_count = found_count;
            portEXIT_CRITICAL(&s_state_lock);
            if (scan_result != ESP_OK) {
                set_status(false, false, false, "Bluetooth scan failed. Try again.");
            } else if (found_count == 0) {
                set_status(false, false, false,
                           "No HID devices found. Put a controller in pairing mode and scan again.");
            } else {
                char message[96];
                std::snprintf(message, sizeof(message), "Found %u HID device(s). Select one to connect.",
                              static_cast<unsigned>(found_count));
                set_status(false, false, false, message);
            }
        } else if (request == TaskRequest::Connect) {
            ESP_LOGI(kTag, "Opening %s at %s via %s (BLE address type %u)",
                     selected.info.name, selected.info.address,
                     selected.info.transport,
                     static_cast<unsigned>(selected.address_type));
            ESP_LOGI(kTag, "Pairing task stack minimum free=%u bytes",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            esp_hidh_dev_t *opened = esp_hidh_dev_open(
                selected.address.data(), selected.transport,
                selected.address_type);
            ESP_LOGI(kTag, "esp_hidh_dev_open returned %s",
                     opened != nullptr ? "a device handle" : "NULL");
            if (opened == nullptr) {
                set_status(false, false, false, "Could not open HID device. Scan and retry.");
            }
        }
    }
}

}  // namespace

esp_err_t bluetooth_gamepad_start()
{
    const esp_err_t gap_result = esp_hid_gap_init(ESP_BT_MODE_BTDM);
    if (gap_result != ESP_OK) {
        set_status(false, false, false, "Bluetooth initialization failed");
        return gap_result;
    }

#if CONFIG_BT_BLE_ENABLED
    esp_err_t result = esp_ble_gattc_register_callback(gamepad_gattc_event_handler);
    if (result != ESP_OK) {
        set_status(false, false, false, "Bluetooth GATT initialization failed");
        return result;
    }
#endif

    const esp_hidh_config_t config = {
        .callback = hid_event_callback,
        .event_stack_size = 8192,
        .callback_arg = nullptr,
    };
    const esp_err_t init_result = esp_hidh_init(&config);
    if (init_result != ESP_OK) {
        set_status(false, false, false, "Bluetooth HID initialization failed");
        return init_result;
    }

    if (xTaskCreate(pairing_task, "gamepad_pair", 8192, nullptr, 5,
                    &s_pair_task) != pdPASS) {
        set_status(false, false, false, "Could not start Bluetooth task");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    set_status(false, false, false, "Bluetooth ready. Scan for nearby HID controllers.");
    ESP_LOGI(kTag, "Bluetooth HID host ready");
    return ESP_OK;
}

esp_err_t bluetooth_gamepad_scan()
{
    if (!s_started || s_pair_task == nullptr) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_state_lock);
    if (s_connected || s_scanning || s_connecting || s_request_pending) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_device_count = 0;
    s_scanning = true;
    s_task_request = TaskRequest::Scan;
    s_request_pending = true;
    portEXIT_CRITICAL(&s_state_lock);
    set_status(true, false, false, "Scanning for nearby Bluetooth HID devices...");
    xTaskNotifyGive(s_pair_task);
    return ESP_OK;
}

esp_err_t bluetooth_gamepad_connect(std::size_t device_id)
{
    if (!s_started || s_pair_task == nullptr) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_state_lock);
    if (s_connected || s_scanning || s_connecting || s_request_pending) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (device_id >= s_device_count) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_requested_device = s_devices[device_id];
    s_notify_registrations = 0;
    s_notify_failures = 0;
    s_connecting = true;
    s_task_request = TaskRequest::Connect;
    s_request_pending = true;
    portEXIT_CRITICAL(&s_state_lock);
    char message[96];
    std::snprintf(message, sizeof(message), "Connecting to %.70s...",
                  s_requested_device.info.name);
    set_status(false, true, false, message);
    xTaskNotifyGive(s_pair_task);
    return ESP_OK;
}

esp_err_t bluetooth_gamepad_disconnect()
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (s_device == nullptr) {
        set_status(false, false, false, "Controller is not connected");
        return ESP_OK;
    }
    robot_controller_emergency_stop();
    set_status(false, false, true, "Disconnecting controller...");
    return esp_hidh_dev_close(s_device);
}

BluetoothGamepadStatus bluetooth_gamepad_status()
{
    BluetoothGamepadStatus status{};
    char message[sizeof(s_message)] = {};
    portENTER_CRITICAL(&s_state_lock);
    status.connected = s_connected;
    status.scanning = s_scanning;
    status.connecting = s_connecting;
    status.input_reports = s_input_report_count;
    status.decoded_reports = s_decoded_report_count;
    status.notify_registrations = s_notify_registrations;
    status.notify_failures = s_notify_failures;
    std::memcpy(message, s_message, sizeof(message));
    portEXIT_CRITICAL(&s_state_lock);
    std::snprintf(status.message, sizeof(status.message), "%s", message);
    return status;
}

std::size_t bluetooth_gamepad_device_count()
{
    portENTER_CRITICAL(&s_state_lock);
    const std::size_t count = s_device_count;
    portEXIT_CRITICAL(&s_state_lock);
    return count;
}

bool bluetooth_gamepad_device_get(std::size_t index, BluetoothDeviceInfo *out)
{
    if (out == nullptr) return false;
    portENTER_CRITICAL(&s_state_lock);
    const bool found = index < s_device_count;
    if (found) *out = s_devices[index].info;
    portEXIT_CRITICAL(&s_state_lock);
    return found;
}
