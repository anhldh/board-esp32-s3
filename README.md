# Air Monitor — ESP32-S3

Đo CO2, nhiệt độ, độ ẩm. Hiện lên màn TFT và POST lên server mỗi phút.

## Phần cứng

- ESP32-S3 DevKitC-1
- TFT 2.8" 240x320, ST7789V, SPI
- SCD40 — CO2, I2C `0x62`, **chỉ 3.3V**
- SHT3X — nhiệt độ/độ ẩm, I2C `0x44`

| Màn | GPIO |  | Cảm biến | GPIO |
|---|---|---|---|---|
| CS | 10 |  | SDA | 8 |
| MOSI | 11 |  | SCL | 9 |
| SCK | 12 |  | | |
| DC | 13 |  | | |
| RST | 14 |  | | |
| MISO | — |  | | |

VCC và LED màn vào 3V3, hai cảm biến dùng chung bus I2C.

## Chạy

```bash
pio run -t upload
pio device monitor
```

## Nhịp hoạt động

| Việc | Chu kỳ |
|---|---|
| Đọc SCD40 | 30s (low power mode) |
| Đọc SHT3X | 10s |
| POST `/api/ingest` | 60s |

Retry 3 lần cách nhau 15s, giữ nguyên `recordedAt` để server ghi đè thay vì tạo bản trùng. Cảm biến lỗi thì bỏ hẳn key khỏi JSON chứ không gửi `0`. Chỉ giữ một gói đang chờ, không có buffer.

## Hiệu chuẩn CO2

ASC đã tắt trong code — phòng chỉ dùng quạt thông gió, đáy ~586 ppm, ASC sẽ coi đó là 400 và trừ oan gần 190 ppm.

Thay bằng FRC thủ công: mang ra ngoài trời, đợi 10 phút, **giữ nút BOOT 3 giây**. Kết quả ghi vào EEPROM của SCD40, sống qua mọi lần nạp lại firmware. Làm lại mỗi năm.

| Ngày | Tham chiếu | Độ lệch bù |
|---|---|---|
| 2026-08-03 | 450 ppm | −8 ppm |

## Hằng số

Đầu `src/main.cpp`:

| | Mặc định | |
|---|---|---|
| `USE_TFT` | 1 | `0` để chạy không màn |
| `SHT_INTERVAL` | 10000 | nhịp đọc SHT3X (ms) |
| `CO2_STALE` | 120000 | quá lâu không có số CO2 → reset cảm biến |
| `UPLOAD_INTERVAL` | 60000 | nhịp POST |
| `BTN_HOLD_MS` | 3000 | giữ nút để kích FRC |
| `FRC_REFERENCE` | 450 | ppm ngoài trời làm mốc |
