# L298N: chặn bánh xe tự quay lúc vừa bật nguồn

## Mục đích

Ghi chú này xử lý hiện tượng một bánh xe quay ngắn khi vừa cấp nguồn cho
ESP32. Trước khi firmware bắt đầu chạy, các GPIO của ESP32 đang ở trạng thái
input hoặc chưa xác định. Nếu chân `EN` của L298N đã được bật bởi jumper và
hai chân `IN` bị trôi mức, driver có thể coi đó là một lệnh quay.

Mục tiêu là giữ kênh motor **tắt hoàn toàn khi boot/reset**, rồi chỉ bật sau
khi firmware đã đặt các chân điều khiển về trạng thái an toàn.

> Không thực hiện khi mạch đang cấp nguồn. Tháo nguồn pin/motor và USB trước
> khi thay đổi dây.

## Mạch hiện tại cần kiểm tra

Trong code hiện tại, bánh **trước phải** được điều khiển bởi hai GPIO sau:

| Tín hiệu trong code | GPIO ESP32 | Nối đến |
| --- | ---: | --- |
| `FrontRightA` | GPIO14 | một chân `INx` của kênh L298N điều khiển bánh trước phải |
| `FrontRightB` | GPIO27 | chân `INx` còn lại của chính kênh đó |

Tên chân trên module có thể là `IN1`/`IN2` hoặc `IN3`/`IN4`, tùy bánh được
nối vào `OUT1`/`OUT2` hay `OUT3`/`OUT4`. Hãy lần theo hai dây từ GPIO14 và
GPIO27; **không đoán theo số IN**.

## Cách 1 — thêm pull-down cho hai chân IN

Đây là cách đơn giản nhất, không yêu cầu sửa code. Chuẩn bị hai điện trở
10 kΩ (có thể dùng 4.7 kΩ đến 10 kΩ).

```text
ESP32 GPIO14 ----+---- L298N INx  (dây thứ nhất của bánh trước phải)
                 |
               [10 kΩ]
                 |
ESP32 GND --------+---- L298N GND

ESP32 GPIO27 ----+---- L298N INy  (dây thứ hai của bánh trước phải)
                 |
               [10 kΩ]
                 |
ESP32 GND --------+---- L298N GND
```

Mỗi điện trở có một đầu nối vào đúng đường tín hiệu `GPIO → IN`, đầu còn lại
nối GND chung. Điện trở không có chiều.

Không nối điện trở này vào `OUT1`–`OUT4`, cực motor, `12V`, `5V`, hoặc chân
`+` của pin.

Pull-down giữ cả hai chân IN ở LOW trước khi ESP32 khởi tạo. Cách này thường
giảm hoặc hết hiện tượng tự quay, nhưng vẫn không tắt hoàn toàn cầu H.

## Cách 2 — khuyến nghị: điều khiển chân EN của kênh motor

Module L298N không có `STBY`, nhưng có `ENA` và `ENB`:

- `ENA` điều khiển kênh có `OUT1`/`OUT2` và `IN1`/`IN2`.
- `ENB` điều khiển kênh có `OUT3`/`OUT4` và `IN3`/`IN4`.

Xác định bánh trước phải đang ở cặp OUT nào để chọn đúng `ENA` hoặc `ENB`.
Trên đa số module, EN tương ứng đang được nối sẵn lên 5 V bằng một jumper.

### Đấu dây

1. Tháo jumper của **đúng chân EN** đang điều khiển bánh trước phải.
2. Nối chân EN đó tới GPIO19 của ESP32. GPIO19 hiện chưa được dùng trong
   project và là chân output phù hợp.
3. Gắn một điện trở 10 kΩ từ chính chân EN đó xuống GND chung.
4. Đảm bảo GND ESP32, GND L298N và GND nguồn motor là cùng một mass.

```text
ESP32 GPIO19 ----+---- L298N ENA hoặc ENB  (đã tháo jumper)
                 |
               [10 kΩ]
                 |
ESP32 GND --------+---- L298N GND
```

**Không được giữ jumper EN cắm lên 5 V** khi đã nối EN với GPIO19. Jumper sẽ
giữ EN ở HIGH, làm mất tác dụng bảo vệ lúc boot.

### Vì sao cần cả GPIO19 và điện trở?

| Thời điểm | GPIO19 | Điện trở 10 kΩ | EN thực tế | Kết quả |
| --- | --- | --- | --- | --- |
| Mới bật nguồn / reset | chưa cấu hình | kéo xuống GND | LOW | L298N tắt kênh motor |
| Firmware đã sẵn sàng | output HIGH | vẫn kéo xuống yếu | HIGH | L298N cho motor hoạt động |

- Chỉ có điện trở: EN luôn LOW, motor không chạy.
- Chỉ có GPIO: khi ESP32 boot chân GPIO có thể floating, lỗi có thể vẫn xảy ra.
- Cả hai: mạch có trạng thái mặc định an toàn.

Sau khi đấu cách 2, **chưa cấp nguồn chạy xe ngay**. Cần cập nhật firmware để
GPIO19 được đặt LOW đầu tiên, chỉ đặt HIGH sau khi mọi chân motor đã được đặt
LOW. Báo lại khi đã hoàn tất việc cắm dây để cập nhật code.

## Kiểm tra an toàn trước khi dùng motor

1. Tắt nguồn hoàn toàn.
2. Kiểm tra jumper EN của kênh đã tháo; jumper EN của kênh khác có thể giữ
   nguyên nếu chưa sửa kênh đó.
3. Dùng đồng hồ đo liên tục để chắc chắn GPIO19 không bị chập vào 5 V hoặc
   GND.
4. Cấp nguồn ESP32 và L298N nhưng tháo dây motor hoặc kê bánh xe khỏi mặt bàn.
5. Quan sát lúc boot/reset. Bánh trước phải không được tự quay.
6. Sau khi firmware được cập nhật, thử lần lượt tiến, lùi và dừng.

## Tham khảo

Theo datasheet L298, `ENA`/`ENB` ở mức LOW sẽ vô hiệu hóa cầu H, bất kể mức
ở các chân IN; datasheet cũng khuyến nghị đưa enable về LOW trước khi bật/tắt
nguồn. [STMicroelectronics — L298 datasheet](https://www.st.com/resource/en/datasheet/l298.pdf)
