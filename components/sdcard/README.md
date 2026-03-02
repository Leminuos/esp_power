## ESP-IDF components required

File `CMakeLists.txt` trong thư mục cần khai báo các components sau:

```cmake
REQUIRES fatfs driver sdmmc
```

**Chi tiết các components:**

| Header File | Component | Mô tả |
|-------------|-----------|-------|
| `esp_vfs_fat.h` | **fatfs** | FAT filesystem support |
| `driver/sdspi_host.h` | **driver** | SPI host driver cho SD card |
| `driver/spi_common.h` | **driver** | SPI common definitions |
| `driver/sdmmc_host.h` | **driver** | SDMMC host driver |
| `sdmmc_cmd.h` | **sdmmc** | SD/MMC protocol commands |

Các components này đã được include sẵn trong ESP-IDF framework.

## Cấu hình Long Filename Support**

Thực hiện chạy:

```bash
idf.py menuconfig
```

Đi đến:

```
Component config
  → FAT Filesystem support
    → Long filename support
        → Chọn "Long filename buffer in heap"
        → Hoặc "Long filename buffer in stack"
```

Hoặc thêm vào `sdkconfig`:

```
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_MAX_LFN=255
```

**Lưu ý:** Nếu không enable LFN, tên file dài hơn format 8.3 sẽ bị rút ngắn:
- `am_tham_ben_em.raw` → `AM_THA~1.RAW`
- `my_long_filename.txt` → `MY_LON~1.TXT`

**Format 8.3 là gì?**
- 8 ký tự cho tên file
- 3 ký tự cho extension
- Tên dài hơn sẽ tự động rút ngắn thành dạng "SHORT~1.EXT"

## Một số nguyên nhân có thể gây ra lỗi

### 1. Thiếu điện trở pullup

Tất cả các chân tín hiệu SPI cần điện trở pull-up 10kΩ:
- MISO (GPIO 19)
- MOSI (GPIO 23)
- CLK (GPIO 18)
- CS (GPIO 5)

**Giải pháp:**
- Thêm điện trở pull-up 10kΩ từ mỗi chân tín hiệu lên 3.3V
- Hoặc enable internal pull-up trong code

### 2. Nguồn cấp không đủ

SD card cần nguồn ổn định 3.3V với dòng đủ lớn.

**Giải pháp:**
- Sử dụng nguồn 3.3V riêng cho SD card
- Thêm tụ bypass 100nF và 10µF gần chân VCC của SD card

### 3. Dây kết nối quá dài hoặc kém chất lượng

**Giải pháp:**
- Sử dụng dây ngắn nhất có thể (< 15cm)
- Sử dụng dây có chất lượng tốt
- Tránh dây breadboard kém
