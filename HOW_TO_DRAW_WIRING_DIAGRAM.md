# Cách vẽ sơ đồ mạch điện cho Kylio Robot Car

## Mục tiêu

Tạo tài liệu để biết chính xác dây nào nối vào đâu, thuận tiện cho sửa chữa,
thay đổi chân GPIO và nâng cấp phần cứng sau này.

Không cần thiết kế PCB. Hãy vẽ theo hai mức:

1. **Sơ đồ đấu dây thực tế**: thể hiện các module, dây và nhãn chân giống xe
   đang lắp.
2. **Schematic**: dùng ký hiệu điện tử chuẩn, dễ đọc và in thành PDF.

Hãy hoàn thiện sơ đồ đấu dây thực tế trước, sau đó mới chuyển thành schematic.

## Công cụ đề xuất

Dùng [EasyEDA](https://easyeda.com/editor): chạy trực tiếp trên trình duyệt,
dễ đặt ký hiệu, vẽ dây và xuất PDF/PNG. Không cần dùng chức năng thiết kế PCB
cho mục tiêu này.

## Bước 1 — ghi nhận mạch hiện tại

Trước khi tháo hoặc thay đổi dây:

1. Tắt toàn bộ nguồn: pin motor, nguồn ESP32 và USB.
2. Chụp ảnh rõ từng mặt của ESP32, từng module L298N, HC-SR04 và bộ nguồn.
3. Chụp cận cảnh các hàng chân có dây cắm; ảnh phải đọc được nhãn `IN1`,
   `ENA`, `OUT1`, `5V`, `GND`…
4. Đánh số mỗi module nếu có nhiều module cùng loại: `L298N-A`, `L298N-B`.
5. Dán nhãn hoặc chụp thêm ảnh mỗi đầu dây trước khi tháo.

> Không suy đoán theo màu dây. Luôn lần theo dây từ đầu này sang đầu kia.

## Bước 2 — tạo bảng kết nối

Tạo một hàng cho mỗi dây. Điền đủ cả dây nguồn và GND. Bảng mẫu:

| Từ           | Đến          | Nhãn mạch         | Ghi chú                         |
| ------------ | ------------ | ----------------- | ------------------------------- |
| ESP32 GPIO13 | L298N-A IN1  | `MOTOR_FL_A`      | Bánh trước trái, input A        |
| ESP32 GPIO12 | L298N-A IN2  | `MOTOR_FL_B`      | Bánh trước trái, input B        |
| ESP32 GPIO14 | L298N-A IN3  | `MOTOR_FR_A`      | Bánh trước phải, input A        |
| ESP32 GPIO27 | L298N-A IN4  | `MOTOR_FR_B`      | Bánh trước phải, input B        |
| ESP32 GPIO26 | L298N-B IN1  | `MOTOR_BL_A`      | Bánh sau trái, input A          |
| ESP32 GPIO25 | L298N-B IN2  | `MOTOR_BL_B`      | Bánh sau trái, input B          |
| ESP32 GPIO33 | L298N-B IN3  | `MOTOR_BR_A`      | Bánh sau phải, input A          |
| ESP32 GPIO32 | L298N-B IN4  | `MOTOR_BR_B`      | Bánh sau phải, input B          |
| ESP32 GPIO18 | HC-SR04 TRIG | `ULTRASONIC_TRIG` | chân xuất xung                  |
| ESP32 GPIO5  | HC-SR04 ECHO | `ULTRASONIC_ECHO` | tín hiệu về ESP32, tối đa 3.3 V |
| ESP32 GND    | L298N GND    | `GND`             | mass chung                      |
| ESP32 GND    | HC-SR04 GND  | `GND`             | mass chung                      |

Thêm các hàng còn thiếu cho:

- `ENA`/`ENB` và jumper của mỗi L298N.
- `OUT1` đến `OUT4` của từng L298N tới motor tương ứng.
- Nguồn pin vào L298N; đường 5 V/3.3 V vào ESP32 và HC-SR04.
- Mọi điện trở, công tắc, công tắc nguồn, tụ hoặc mạch hạ áp.

## Bước 3 — vẽ sơ đồ đấu dây trong EasyEDA

1. Tạo **New Project**, rồi **New Schematic**.
2. Đặt năm khối lớn: `ESP32`, `L298N-A`, `L298N-B` (nếu có), `HC-SR04`,
   `Nguồn`, và `Motor`.
3. Không cần tìm đúng hình module ngay. Có thể dùng ký hiệu đầu nối
   (`Connector`) và tự ghi nhãn chân.
4. Đặt ESP32 bên trái, driver motor ở giữa, motor bên phải, cảm biến ở phía
   trên, nguồn ở phía dưới.
5. Nối dây theo bảng kết nối ở bước 2.
6. Đặt nhãn mạch thay vì kéo dây quá dài: `GND`, `5V`, `MOTOR_FR_A`,
   `ULTRASONIC_TRIG`… Hai điểm có cùng nhãn được hiểu là cùng một dây.
7. Vẽ GND chung rõ ràng giữa ESP32, L298N, cảm biến và nguồn motor.

## Bước 4 — bổ sung mạch chống tự quay lúc boot (nếu thực hiện)

Nếu dùng phương án điều khiển Enable của L298N:

1. Xác định bánh trước phải thuộc `ENA` hay `ENB` bằng cặp `OUT` thực tế.
2. Tháo jumper của đúng chân EN đó.
3. Vẽ `GPIO19` nối tới chân EN.
4. Vẽ điện trở `R_EN = 10 kΩ` từ chân EN xuống `GND`.
5. Ghi chú trên sơ đồ: `Jumper EN removed`.

Xem chi tiết tại [L298N_STARTUP_SAFETY.md](L298N_STARTUP_SAFETY.md).

## Bước 5 — kiểm tra trước khi xem là hoàn tất

- Mỗi GPIO trong `main/app_config.h` xuất hiện đúng một lần trong bảng.
- Không có GPIO nào nối nhầm vào `OUT1`–`OUT4` của L298N.
- Tất cả module dùng chung GND.
- ECHO của HC-SR04 không vượt 3.3 V khi vào ESP32.
- Nếu có EN nối GPIO19, jumper EN đã được thể hiện là tháo ra.
- Sơ đồ và thực tế khớp từng dây bằng cách dùng đồng hồ đo thông mạch.

## Bước 6 — lưu và xuất

Đặt tên file theo ngày hoặc phiên bản, ví dụ `kylio_robot_wiring_v1`.
Xuất cả PDF và PNG để xem nhanh, rồi lưu link/project EasyEDA cùng ảnh chụp
module. Khi thay đổi dây hoặc GPIO, cập nhật đồng thời:

1. bảng kết nối;
2. schematic;
3. `main/app_config.h` trong firmware.
