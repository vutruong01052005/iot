# Bài thực hành 2: Thu thập, lưu trữ và tiền xử lý dữ liệu IoT

> Pipeline demo: **ESP32/Wokwi → MQTT Mosquitto → Python Collector → InfluxDB 2.x → tiền xử lý → Streamlit**.

Project mở rộng node cảm biến từ Bài thực hành 1 thành pipeline có lưu trữ dữ liệu thời gian, xử lý dữ liệu thiếu và dashboard giám sát.

1.Mục tiêu

- Thu thập telemetry từ ESP32 qua MQTT.
- Kiểm tra và lưu dữ liệu cảm biến vào InfluxDB.
- Làm sạch dữ liệu, phát hiện outlier, resample và tạo đặc trưng.
- Hiển thị dữ liệu raw và dữ liệu sau xử lý trên Streamlit.

2.Kiến trúc

```text
DHT22 ─┐
LDR ───┼─> ESP32 / Wokwi ─MQTT─> Mosquitto ─> Collector ─> InfluxDB
HC-SR04┘                                                   ├─ sensor_raw
																													└─ sensor_preprocessed
																																	│
																														 Streamlit
```

3.Thành phần

| Thành phần | Vai trò |
| --- | --- |
| ESP32/Wokwi | Đọc DHT22, LDR, HC-SR04; đóng gói JSON và publish mỗi 5 giây. |
| Mosquitto | MQTT broker trong Docker, lắng nghe cổng host `1883`. |
| Collector | Subscribe telemetry, parse JSON và ghi field hợp lệ vào InfluxDB. |
| InfluxDB 2.x | Lưu measurement `sensor_raw` và `sensor_preprocessed` trong bucket `iot_raw`. |
| Preprocess | Xử lý missing/outlier, resample, rolling mean, delta và Min-Max. |
| Dashboard | Hiển thị các metric mới nhất, biểu đồ raw và dữ liệu đã xử lý. |

4.Cấu trúc thư mục

```text
iot-bai2/
├── collector/          # MQTT collector
├── dashboard/          # Streamlit app
├── firmware/           # PlatformIO, firmware và cấu hình Wokwi
│   ├── src/main.cpp
│   ├── diagram.json
│   ├── platformio.ini
│   └── wokwi.toml
├── mosquitto/          # Cấu hình MQTT broker
├── preprocess/         # Tiền xử lý InfluxDB
├── docker-compose.yml
├── diagram.json        # Sơ đồ khi mở workspace ở repo root
└── wokwi.toml          # Cấu hình khi mở workspace ở repo root
```

Hai bộ `diagram.json`/`wokwi.toml` phục vụ hai cách mở project. Khuyến nghị mở riêng thư mục `firmware` trong VS Code để PlatformIO và Wokwi dùng cùng một project root.

5.Yêu cầu

- Docker Desktop với Linux containers.
- VS Code, PlatformIO IDE và Wokwi for VS Code.
- Wokwi Private IoT Gateway khi mô phỏng cần truy cập MQTT broker chạy trên máy host.

6.Khởi động pipeline

Chạy tại thư mục gốc `iot-bai2`:

```powershell
docker compose up -d --build
docker compose ps
```

Đợi InfluxDB có trạng thái `healthy`, các container `mosquitto`, `collector` và `dashboard` ở trạng thái `running`.

| Dịch vụ | Địa chỉ |
| --- | --- |
| Dashboard | <http://localhost:8501> |
| InfluxDB UI | <http://localhost:8086> |
| MQTT broker từ máy host | `localhost:1883` |

Thông tin InfluxDB demo được khai báo trong `docker-compose.yml`: org `iot-lab`, bucket `iot_raw`, user `iot-user`. Credentials hiện tại chỉ dành cho môi trường lab; hãy đổi trước khi chia sẻ hoặc triển khai.

7.Chạy ESP32 trên Wokwi

1. Mở `iot-bai2/firmware` bằng **File → Open Folder** trong VS Code.
2. Chọn **PlatformIO: Build**.
3. Chạy **Wokwi: Start Simulator** và giữ tab mô phỏng hiển thị.
4. Mở Serial Monitor để xem payload và trạng thái publish.

Firmware đọc các chân sau:

| Cảm biến/thiết bị | Chân ESP32 |
| --- | --- |
| DHT22 data | GPIO15 |
| LDR analog output (AO) | GPIO34 |
| HC-SR04 TRIG | GPIO5 |
| HC-SR04 ECHO | GPIO18 |
| LED | GPIO2 |

Sơ đồ Wokwi có LDR nối `AO → D34`; thay thuộc tính `lux` của linh kiện để thay đổi độ sáng mô phỏng. DHT22 có thể thay đổi nhiệt độ/độ ẩm bằng popup của cảm biến.

Để đọc serial qua RFC2217, chạy tại thư mục `firmware` trong terminal thứ hai:

```powershell
pio device monitor -p rfc2217://localhost:4003
```

Mỗi chu kỳ khoảng 5 giây, serial in JSON và trạng thái `publish=OK`. Nếu DHT lỗi, firmware in `DHT22 read failed: ...`. Mô phỏng Wokwi có thể tạm dừng khi tab bị ẩn.

8.MQTT và dữ liệu

- Topic telemetry: `iot/bai2/esp32/telemetry`.
- Topic trạng thái: `iot/bai2/esp32/status`.
- Payload gồm `device_id`, `ts`, `seq`, `temperature`, `humidity`, `light_lux`, `distance_cm`, `rssi` và `uptime_s`.
- `temperature`, `humidity`, `light_lux` hoặc `distance_cm` có thể là `null` nếu lần đọc cảm biến không hợp lệ; collector bỏ qua field null nhưng vẫn lưu các field hợp lệ khác.

Ví dụ payload:

```json
{
	"device_id": "esp32",
	"ts": 1790752222040,
	"seq": 1,
	"temperature": 25.0,
	"humidity": 50.0,
	"light_lux": 499.6,
	"distance_cm": 100.86,
	"rssi": -96,
	"uptime_s": 10
}
```

9.MQTT host trong Wokwi

Firmware phải kết nối tới IPv4 của máy host đang chạy Docker, không dùng `localhost`, hostname Docker `mosquitto` hay IP nội bộ container. Xem địa chỉ adapter Wi-Fi bằng `ipconfig`, rồi cập nhật `MQTT_SERVER` trong `firmware/src/main.cpp` nếu DHCP cấp IP mới. Cho phép TCP `1883` qua Windows Firewall và bật Wokwi Private IoT Gateway. Cổng `1883` không mã hóa; cấu hình Mosquitto hiện cho phép anonymous chỉ nhằm phục vụ demo.

10.InfluxDB và tiền xử lý

Collector ghi dữ liệu raw vào bucket `iot_raw`, measurement `sensor_raw`. Script tiền xử lý ghi measurement `sensor_preprocessed` trong cùng bucket.

Chạy tiền xử lý tại repo root:

```powershell
docker compose run --rm --build preprocess
```

Các bước đang triển khai trong `preprocess/preprocess.py`:

1. Chuyển field cảm biến sang kiểu số và nội suy/forward-fill/backward-fill giá trị thiếu.
2. Phát hiện outlier bằng IQR, thay giá trị ngoài ngưỡng bằng missing rồi nội suy lại.
3. Resample theo cửa sổ 10 giây.
4. Tạo rolling mean 3 mẫu, `distance_delta` và các field Min-Max chuẩn hóa.
5. Ghi kết quả sang `sensor_preprocessed`.

Script xử lý các field hiện có; nếu thiếu DHT, nó cảnh báo và tiếp tục với cảm biến khả dụng, không tự tạo số đo DHT giả.

11.Kiểm tra và xử lý sự cố

```powershell
docker compose ps
docker compose logs -f collector
docker compose logs -f mosquitto
```

- **Dashboard báo thiếu DHT:** kiểm tra Serial Monitor. Nếu JSON có `temperature: null`/`humidity: null`, lỗi nằm ở lần đọc DHT; refresh dashboard không thể khôi phục mẫu raw.
- **Serial không hiện:** mở thư mục `firmware`, xác nhận `wokwi.toml` và `diagram.json` nằm cạnh `platformio.ini`, stop/start simulator, giữ tab Wokwi hiển thị. RFC2217 dùng `localhost:4003`, không phải cổng COM.
- **Wokwi không kết nối MQTT:** kiểm tra `MQTT_SERVER`, TCP `1883`, Windows Firewall và Wokwi Private IoT Gateway.
- **Dashboard không cập nhật:** mở <http://localhost:8501>, nhấn `Ctrl+F5`; kiểm tra `collector` và InfluxDB có mẫu mới.
- **Preprocess báo thiếu field:** rebuild service bằng `docker compose run --rm --build preprocess`; cần có `sensor_raw` trước khi chạy.

12. An toàn khi triển khai

Mosquitto hiện chạy anonymous trên mạng lab và MQTT port `1883` không mã hóa. Không dùng cấu hình này ngoài môi trường demo. Triển khai thực tế cần username/password, ACL, MQTT over TLS và secrets lưu ngoài repository.
