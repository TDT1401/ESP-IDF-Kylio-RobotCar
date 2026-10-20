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
2. Chụp ảnh rõ từng mặt của ESP32, từng module L298N và bộ nguồn.
3. Chụp cận cảnh các hàng chân có dây cắm; ảnh phải đọc được nhãn `IN1`,
   `ENA`, `OUT1`, `5V`, `GND`…
4. Xác nhận chỉ còn một module L298N điều khiển bốn motor.
5. Dán nhãn hoặc chụp thêm ảnh mỗi đầu dây trước khi tháo.

> Không suy đoán theo màu dây. Luôn lần theo dây từ đầu này sang đầu kia.

## Bước 2 — tạo bảng kết nối

Tạo một hàng cho mỗi dây. Điền đủ cả dây nguồn và GND. Bảng mẫu:

| Từ           | Đến          | Nhãn mạch         | Ghi chú                         |
| ------------ | ------------ | ----------------- | ------------------------------- |
| ESP32 GPIO13 | L298N IN1    | `LEFT_IN1`       | Kênh A: hai motor bên trái      |
| ESP32 GPIO12 | L298N IN2    | `LEFT_IN2`       | Kênh A: hai motor bên trái      |
| ESP32 GPIO14 | L298N IN3    | `RIGHT_IN3`      | Kênh B: hai motor bên phải      |
| ESP32 GPIO27 | L298N IN4    | `RIGHT_IN4`      | Kênh B: hai motor bên phải      |
| ESP32 GND    | L298N GND    | `GND`             | mass chung                      |

Thêm các hàng còn thiếu cho:

- Hai motor bên trái mắc song song vào cặp `OUT1`/`OUT2` (kênh A).
- Hai motor bên phải mắc song song vào cặp `OUT3`/`OUT4` (kênh B).
- `ENA`/`ENB` của module cần được bật; firmware hiện chỉ điều khiển
  `IN1`–`IN4`, chưa điều khiển tốc độ bằng PWM.
- Nguồn pin vào L298N; đường 5 V/3.3 V vào ESP32.
- Mọi điện trở, công tắc, công tắc nguồn, tụ hoặc mạch hạ áp.

## Bước 3 — vẽ sơ đồ đấu dây trong EasyEDA

1. Tạo **New Project**, rồi **New Schematic**.
2. Đặt các khối lớn: `ESP32`, `L298N`, `Nguồn`, và bốn `Motor`.
3. Không cần tìm đúng hình module ngay. Có thể dùng ký hiệu đầu nối
   (`Connector`) và tự ghi nhãn chân.
4. Đặt ESP32 bên trái, driver motor ở giữa, motor bên phải, nguồn ở phía dưới.
5. Nối dây theo bảng kết nối ở bước 2.
6. Đặt nhãn mạch thay vì kéo dây quá dài: `GND`, `5V`, `LEFT_IN1`…
   Hai điểm có cùng nhãn được hiểu là cùng một dây.
7. Vẽ GND chung rõ ràng giữa ESP32, L298N và nguồn motor.

## Bước 4 — kiểm tra chiều quay và tải

Đấu cực của hai motor cùng bên sao cho chúng đều làm xe chạy tới khi kênh đó
nhận lệnh tiến. Sau khi cấp nguồn, thử từng hướng với bánh xe nhấc khỏi mặt đất.

Hai motor dùng chung một kênh sẽ cộng dòng tải. [Datasheet L298 của
ST](https://www.st.com/resource/en/datasheet/l298.pdf) nêu giới hạn tuyệt đối
2 A DC cho mỗi kênh; kiểm tra tổng dòng của cặp motor, nhất là lúc khởi động
hoặc kẹt bánh, trước khi vận hành.

## Bước 5 — kiểm tra trước khi xem là hoàn tất

- Mỗi GPIO trong `main/app_config.h` xuất hiện đúng một lần trong bảng.
- Không có GPIO nào nối nhầm vào `OUT1`–`OUT4` của L298N.
- Tất cả module dùng chung GND.
- Hai motor bên trái chung kênh A, hai motor bên phải chung kênh B.
- Sơ đồ và thực tế khớp từng dây bằng cách dùng đồng hồ đo thông mạch.

## Bước 6 — lưu và xuất

Đặt tên file theo ngày hoặc phiên bản, ví dụ `kylio_robot_wiring_v1`.
Xuất cả PDF và PNG để xem nhanh, rồi lưu link/project EasyEDA cùng ảnh chụp
module. Khi thay đổi dây hoặc GPIO, cập nhật đồng thời:

1. bảng kết nối;
2. schematic;
3. `main/app_config.h` trong firmware.
