## Triển khai lvgl

Đây là các biết triển khai cho lvgl version 9, sử dụng framework esp-idf và sử dụng màn hình là ili9341.

**Tạo project**

```bash
idf.py create-project lvgl_test
cd lvgl_test
```

**Chuẩn bị**

Đầu tiên, ta cần thêm các dependency cần thiết vào file `idf_component.yml`:

```
dependencies:
  lvgl/lvgl: "9.4.0"
  espressif/esp_lcd_ili9341: "^2.0.2"
```

Tiếp theo, ta tạo file `main/lv_conf.h` và đảm bảo project include được file này. Nội dung của file có thể được copy từ file `lv_conf_template.h` của lvgl library.

**Khởi tạo spi bus**

Ta sử dụng hàm `spi_bus_initialize` để thực hiện khởi tạo spi bus.

Prototype của hàm như sau:

```c
esp_err_t spi_bus_initialize(spi_host_device_t host_id,
                             const spi_bus_config_t *bus_config,
                             spi_dma_chan_t dma_chan);
```

Trong đó:
- `host_id`: SPI controller, đối với ESP32 thì có thường có `SPI2_HOST` (hay gọi là HSPI) và `SPI3_HOST` (hay gọi là VSPI).
- `bus_config`: bus config. Một struct `spi_bus_config_t` sẽ có các trường như sau:
  - `mosi_io_num`: GPIO dùng làm MOSI.
  - `miso_io_num`: GPIO dùng làm MISO.
  - `sclk_io_num`: GPIO dùng làm SCLK (xung clock).
  - `quadwp_io_num`, `quadhd_io_num`: Dùng cho SPI Quad (WP/HOLD hay IO2/IO3). Chỉ có ý nghĩa nếu ta chạy QSPI với thiết bị hỗ trợ quad I/O. Nếu không sử dụng thì để các trường này bằng -1.
  - `data4_io_num`...`data7_io_num` (nếu có): Một số SoC/IDF hỗ trợ Octal SPI hoặc mở rộng lane. Nếu không sử dụng thì để các trường này bằng -1.
  - `max_transfer_sz`: kích thước transfer lớn nhất mà bus sẽ chuẩn bị tài nguyên (DMA hoặc buffer) để hỗ trợ. Đơn vị: byte.
- `dma_chan`: Chỉ định kênh DMA cho SPI.
  - `SPI_DMA_CH_AUTO`: IDF tự chọn kênh DMA phù hợp.
  - `SPI_DMA_DISABLED`: tắt DMA, SPI truyền bằng FIFO.

Đây là code cấu hình bus spi cho ili9341, sử dụng host controller là `SPI2_HOST`:

```c
spi_bus_config_t buscfg = {
    .mosi_io_num = ILI9341_MOSI_PIN,
    .miso_io_num = ILI9341_MISO_PIN,
    .sclk_io_num = ILI9341_CLK_PIN,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2,
};

ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
```

**Khởi tạo ili9341**

Khi đã khởi tạo spi bus thì bước kế tiếp ta phải làm đó là khởi tạo ili9341.

Đầu tiên ta cần sử dụng hàm `esp_lcd_new_panel_io_spi` để cấu hình I/O SPI.

Protype của hàm như sau:

```c
esp_err_t esp_lcd_new_panel_io_spi(
    esp_lcd_spi_bus_handle_t bus,
    const esp_lcd_panel_io_spi_config_t *io_config,
    esp_lcd_panel_io_handle_t *ret_io
);
```

Trong đó:
- `bus`: Là SPI bus đã được khởi tạo trước đó bằng.
- `io_config`: Đây là trái tim của hàm. Nó mô tả cách giao tiếp SPI với LCD.
  - `dc_gpio_num`: Data/Command pin.
  - `cs_gpio_num`: Chip select pin.
  - `pclk_hz`: Tần số SPI clock khi gửi data/pixel. Đơn vị: Hz.
  - `lcd_cmd_bits`: Số bit của command được gửi đi.
  - `lcd_param_bits`: Số bit cho tham số đi sau command.
  - `spi_mode`: SPI mode (0...3).
  - `trans_queue_depth`: Số lượng SPI transaction tối đã mà queue có thể giữ, quan trọng khi dùng với DMA.
  - `on_color_trans_done`: Callback được gọi sau khi truyền xong color data.
  - `user_ctx`: User context.
  - `flags`: Cờ điều khiển
    - `dc_low_on_data`:
      - 0 (default): DC=0 → CMD; DC=1 → DATA
      - 1: đảo logic
    - `lsb_first`: Thứ tự bit (0: MSB first; 1: LSB first).
    - `sio_mode`: Single I/O mode
- `ret_io`: hanlde của panel I/O.

Tiếp theo, ta sẽ khởi tạo một panel cho ili9341 bằng cách sử dụng hàm `esp_lcd_new_panel_ili9341`.

Tham số của hàm như sau:

```c
esp_err_t esp_lcd_new_panel_ili9341(
    esp_lcd_panel_io_handle_t io,
    const esp_lcd_panel_dev_config_t *panel_dev_config,
    esp_lcd_panel_handle_t *ret_panel
);
```

Trong đó:
- `io`: Đây là handle của panel IO. Được lấy từ tham số output `ret_io` của hàm `esp_lcd_new_panel_io_spi`.
- `panel_dev_config`: Đây là cấu hình panel dùng chung trong esp_lcd.
  - `reset_gpio_num`: GPIO nối chân RST của LCD. Nếu không dùng hard reset thì đặt -1.
  - `rgb_ele_order`: Thứ tự thành phần màu mà panel mong đợi: RGB hay BGR.
  - `bits_per_pixel`: Số bit mỗi pixel được gửi xuống LCD (thường 16 cho RGB565).
- `ret_panel`: Đây là output parameter trả về handle của panel sau khi tạo thành công.

Khi đã khởi tạo panel thành công thì ta sẽ được một handle `ret_panel`. Ta sẽ dùng handle này để tiếp tục khởi tạo panel bằng các hàm:

```c
esp_lcd_panel_reset(panel_handle)
esp_lcd_panel_init(panel_handle)
esp_lcd_panel_draw_bitmap(panel_handle, ...)
esp_lcd_panel_disp_on_off(panel_handle, ...)
```

Code đầy đủ khởi tạo ili9341 như sau:

```c
esp_lcd_panel_io_spi_config_t io_config = {
    .cs_gpio_num = ILI9341_CS_PIN,
    .dc_gpio_num = ILI9341_DC_PIN,
    .spi_mode = 0,
    .pclk_hz = 26 * 1000 * 1000, // 26 MHz
    .trans_queue_depth = 10,
    .on_color_trans_done = NULL,
    .user_ctx = NULL,
    .lcd_cmd_bits = 8,
    .lcd_param_bits = 8,
    .flags = {
        .dc_low_on_data = 0,
        .lsb_first = 0,
        .cs_high_active = 0,
        .disable_control_phase = 0,
    }
};

ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
    return ret;
}

esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = ILI9341_RST_PIN,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = 16,
};

ret = esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create ILI9341 panel: %s", esp_err_to_name(ret));
    return ret;
}

esp_lcd_panel_reset(panel_handle);
esp_lcd_panel_init(panel_handle);
esp_lcd_panel_invert_color(panel_handle, false);
esp_lcd_panel_mirror(panel_handle, false, false);
esp_lcd_panel_set_gap(panel_handle, 0, 0);
esp_lcd_panel_disp_on_off(panel_handle, true);
```

**Khởi tạo lvgl**

OK, khi đã cấu hình xong spi và ili9341 thì bước tiếp theo, ta khởi tạo lvgl:

```c
lv_init();

lv_tick_set_cb(tick_get_cb);

size_t buf_size = DISPLAY_WIDTH * 40 * sizeof(lv_color_t);
buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);

if (!buf1 || !buf2) {
    ESP_LOGE(TAG, "Failed to allocate display buffers");
    return ESP_ERR_NO_MEM;
}

display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
lv_display_set_flush_cb(display, lvgl_flush_cb);
lv_display_set_buffers(display, buf1, buf2, sizeof(lv_color_t) * DISPLAY_WIDTH * 40, LV_DISPLAY_RENDER_MODE_PARTIAL);
```

Thực hiện đăng ký hàm callback `tick_get_cb` thông qua hàm `lv_tick_set_cb` để lvgl có thể cập nhập được thời gian. Nếu không hoạt động render không thể thực hiện được.

```c
static uint32_t tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
```

Hàm `lv_display_set_flush_cb` sẽ thực hiện đăng ký một callback của chúng ta để khi lvgl muốn thực hiện flush pixel từ virtual display buffer ra màn hình.

```c
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t x1 = area->x1;
    int32_t y1 = area->y1;
    int32_t x2 = area->x2;
    int32_t y2 = area->y2;

    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2 + 1, y2 + 1, (void *)px_map);
    
    lv_display_flush_ready(disp);
}
```

Trong hàm callback `lvgl_flush_cb` thì cuối hàng ta cần phải gọi `lv_display_flush_ready` nếu không lvgl sẽ hiểu là chưa flush xong, từ đó nó sẽ không thực hiện chu trình render tiếp theo -> Dẫn đến bị treo.

Ngoài ra, khi sử dụng DMA thì ta cũng phải gọi hàm `lv_display_flush_ready` trong callback `on_color_trans_done`.

Cuối cùng, ta sẽ tạo một active screen làm root object tree, từ đó có thể tạo các widget như button, text,...

```c
lv_obj_t *scr = lv_screen_active();

lv_obj_t *label = lv_label_create(scr);
lv_label_set_text(label, "Hello World");
lv_obj_center(label);
```
