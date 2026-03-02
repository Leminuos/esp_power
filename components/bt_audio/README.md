# ESP32 Bluetooth Audio Source — API Guide

## Tổng quan

Module `bt_audio` phát audio qua Bluetooth A2DP trên ESP32. ESP32 đóng vai trò **A2DP Source**, gửi audio PCM đến loa/tai nghe Bluetooth (A2DP Sink).

Module tự quản lý reader task nội bộ — dev chỉ cần gọi `bt_audio_play(path)` và module lo toàn bộ việc đọc file, decode, buffer, stream qua BT.

## Kiến trúc

### Decoder Abstraction

Module sử dụng decoder interface để hỗ trợ nhiều format audio:

```
bt_audio_play("/sdcard/song.wav")
    │
    ├── find_decoder(".wav") → bt_audio_wav_decoder
    ├── decoder->open()      → parse header, lấy metadata
    ├── reader task          → decoder->read() loop → stream buffer
    └── A2DP data callback   → lấy từ stream buffer → SBC encode → BT
```

Built-in decoders: WAV (PCM 16-bit stereo 44100 Hz), raw PCM (headerless).

Thêm decoder mới (MP3, FLAC, ...):

```c
const bt_audio_decoder_t my_mp3_decoder = {
    .name  = "MP3",
    .open  = mp3_open,      // parse ID3, init decoder
    .read  = mp3_read,      // decode frame → output PCM
    .seek  = mp3_seek,      // NULL nếu không hỗ trợ
    .close = mp3_close,
};

const char *ext[] = {"mp3"};
bt_audio_register_decoder(&my_mp3_decoder, ext, 1);
```

Decoder `read()` phải output PCM signed 16-bit LE, stereo, 44100 Hz. Nếu source khác format, decoder tự convert.

### State Machine

| State | Mô tả |
|-------|--------|
| `INIT` | BT stack đã init, chưa kết nối |
| `DISCOVERING` | Đang quét thiết bị |
| `DISCOVERY_DONE` | Quét xong |
| `CONNECTING` | Đang kết nối |
| `CONNECTED` | Đã kết nối, sẵn sàng phát |
| `PLAYING` | Đang phát audio |
| `PAUSED` | Tạm dừng |
| `DISCONNECTED` | Mất kết nối |

### Event System

Callback được gọi từ BT task context — return nhanh, không block.

| Event | Mô tả | Payload |
|-------|--------|---------|
| `STATE_CHANGED` | State thay đổi | `bt_audio_evt_state_changed_t` |
| `DISCOVERY_RESULT` | Phát hiện thiết bị BT | NULL |
| `TRACK_FINISHED` | Phát xong track, A2DP auto stop | NULL |
| `DATA_UPDATE` | A2DP vừa lấy data — dùng để update UI | `bt_audio_playback_pos_t` |

## API Reference

### Khởi tạo

```c
esp_err_t bt_audio_init(const char *device_name);
void      bt_audio_register_callback(bt_audio_event_cb_t callback);
```

`bt_audio_init()` khởi tạo toàn bộ bluetooth stack và module audio. Gọi một lần duy nhất khi khởi động. Sau khi init, module ở trạng thái `IDLE` — chưa kết nối, sẵn sàng discovery.
- **device_name**: tên hiển thị khi thiết bị khác quét bluetooth (ví dụ: `"ESP32_Player"`)
- **Return**: `ESP_OK` nếu thành công

```c
bt_audio_init("ESP32_Player");
```

`bt_audio_register_callback` dùng để dăng ký hàm callback để nhận các event từ module. Callback được gọi từ BT task nên cần thực hiện nhanh, không block, không gọi BT API bên trong.

```c
void my_handler(const bt_audio_event_t *evt) {
    switch (evt->type) {
    case BT_AUDIO_EVT_STATE_CHANGED: {
        const bt_audio_evt_state_changed_t *s = evt->data;
        printf("State: %d -> %d\n", s->old_state, s->new_state);
        break;
    }
    case BT_AUDIO_EVT_TRACK_FINISHED:
        printf("Track done\n");
        break;
    case BT_AUDIO_EVT_DATA_UPDATE: {
        const bt_audio_playback_pos_t *pos = evt->data;
        // Update progress bar UI
        break;
    }
    default: break;
    }
}

bt_audio_register_callback(my_handler);
```

### Discovery & Connection

#### Hàm `bt_audio_start_discovery`

```c
void bt_audio_start_discovery(bool auto_connect);
```

Bắt đầu quét thiết bị bluetooth xung quanh. Module chỉ lọc thiết bị có Class of Device = Audio/Video (loa, tai nghe, ...).

- **auto_connect = true**: tự kết nối thiết bị audio đầu tiên tìm thấy, discovery dừng ngay sau khi kết nối
- **auto_connect = false**: lưu tất cả thiết bị vào danh sách nội bộ (tối đa `BT_AUDIO_MAX_DISCOVERED` = 16), dev tự chọn thiết bị qua các API phía dưới

Thời gian quét: ~12 giây (10 inquiry cycles). Khi quét xong, state chuyển sang `DISCOVERY_DONE`. Mỗi thiết bị tìm thấy sẽ bắn một event `DISCOVERY_RESULT`.

```c
// Auto-connect: không cần làm gì thêm, module tự kết nối
bt_audio_start_discovery(true);

// Manual: quét xong → chọn thiết bị
bt_audio_start_discovery(false);
```

#### Hàm `bt_audio_stop_discovery`

```c
void bt_audio_stop_discovery(void);
```

Dừng quét sớm trước khi hết 12 giây. Hữu ích khi UI cho phép user cancel.


#### Hàm `bt_audio_discovery_print`

```c
void bt_audio_discovery_print(void);
```

In danh sách thiết bị đã quét ra serial log dạng bảng, bao gồm tên, BD_ADDR, RSSI, Class of Device. Tiện cho debug.

```
│  0 │ JBL Flip 5            │ 00:11:22:33:44:55 │  -45 │ 2418 │
│  1 │ AirPods Pro           │ aa:bb:cc:dd:ee:ff │  -62 │ 2404 │
```

#### Hàm `bt_audio_discovery_get_count`

```c
esp_err_t bt_audio_discovery_get_count(uint16_t *count);
```

Lấy số lượng thiết bị đã quét được.

#### Hàm `bt_audio_discovery_get_results`

```c
esp_err_t bt_audio_discovery_get_results(uint16_t *count, bt_audio_discovered_dev_t *devs);
```

Copy danh sách thiết bị vào buffer do caller cấp phát.
- **count**: [in] kích thước mảng `devs`, [out] số thiết bị thực tế đã copy
- **devs**: mảng output, cần cấp phát trước.

```c
uint16_t count = BT_AUDIO_MAX_DISCOVERED;
bt_audio_discovered_dev_t devs[BT_AUDIO_MAX_DISCOVERED];
bt_audio_discovery_get_results(&count, devs);

for (int i = 0; i < count; i++) {
    printf("#%d: %s [%s] RSSI=%d\n", i, devs[i].name, devs[i].bda_str, devs[i].rssi);
}
```

#### Hàm `bt_audio_connect_by_index`

```c
esp_err_t bt_audio_connect_by_index(int index)
```

Kết nối đến thiết bị theo index trong danh sách discovery (từ `bt_audio_discovery_get_results()` hoặc `bt_audio_discovery_print()`).

- **index**: 0-based index
- **Return**: `ESP_OK` hoặc `ESP_ERR_INVALID_ARG` nếu index ngoài phạm vi

Kết quả kết nối trả về qua event `STATE_CHANGED` → `CONNECTED` hoặc `DISCONNECTED`.

```c
bt_audio_discovery_print();     // Xem danh sách
bt_audio_connect_by_index(0);   // Kết nối thiết bị đầu tiên
// Đợi event CONNECTED...
```

#### Hàm `bt_audi_connect`

```c
esp_err_t bt_audio_connect(const uint8_t bda[6]);
```

Kết nối trực tiếp bằng `BD_ADDR` (6 bytes). Dùng khi đã biết trước địa chỉ thiết bị (ví dụ: lưu trong NVS để reconnect).

```c
uint8_t bda[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
bt_audio_connect(bda);
```

#### Hàm bt_audio_disconnect

```c
esp_err_t bt_audio_disconnect(void);
```

Ngắt kết nối thiết bị hiện tại. Tự động gọi `bt_audio_stop()` nếu đang phát.

### Playback

**Play audio** 

```c
esp_err_t bt_audio_play(const char *path);
```

Phát file audio qua Bluetooth. Đây là API chính của module — một lệnh duy nhất thực hiện toàn bộ:

1. Auto-detect decoder theo file extension (`.wav`, `.pcm`, `.raw`, hoặc custom)
2. Mở file qua decoder, parse header/metadata (duration, title, ...)
3. Tạo stream buffer nội bộ
4. Tạo reader task đọc file → decode → push PCM vào buffer
5. Gửi lệnh A2DP START để bắt đầu streaming

Nếu đang phát bài khác, module tự `stop()` trước rồi play bài mới.

- **path**: đường dẫn file trên filesystem, ví dụ `"/sdcard/music/song.wav"`
- **Return**:
  - `ESP_OK`: bắt đầu phát thành công
  - `ESP_ERR_INVALID_ARG`: path NULL hoặc không tìm được decoder cho extension
  - `ESP_ERR_INVALID_STATE`: chưa kết nối BT
  - `ESP_ERR_NOT_FOUND`: không mở được file
  - `ESP_ERR_INVALID_SIZE`: header file không hợp lệ (WAV)
  - `ESP_ERR_NO_MEM`: không đủ RAM cho buffer hoặc reader task
  - `ESP_ERR_NOT_SUPPORTED`: không có decoder nào đăng ký cho extension này

```c
// Play WAV
bt_audio_play("/sdcard/song.wav");

// Play raw PCM
bt_audio_play("/sdcard/audio.pcm");

// Play bài mới (tự stop bài cũ)
bt_audio_play("/sdcard/next.wav");
```

**Pause audio**

```c
void bt_audio_pause(void);
```

Tạm dừng phát. Reader task ngừng đọc file, A2DP data callback output silence. State chuyển sang `PAUSED`.

Gọi `bt_audio_resume()` để tiếp tục từ vị trí đã dừng.

**Resume audio**

```c
void bt_audio_resume(void);
```

Tiếp tục phát sau pause. Reader task resume đọc file, state chuyển sang `PLAYING`.

**Stop audio**

```c
void bt_audio_stop(void);
```

Dừng phát hoàn toàn. Dừng reader task, đóng decoder (đóng file), gửi lệnh A2DP STOP. Sau khi stop, state quay về `IDLE` — sẵn sàng play bài khác.

Khác với track tự hết (bắn sự kiện `TRACK_FINISHED` rồi auto stop), `bt_audio_stop()` là dev chủ động dừng.

**Seek audio**

```c
void bt_audio_seek_ms(uint32_t position_ms);
```

Seek đến vị trí bất kỳ trong track (tính bằng milliseconds từ đầu bài).

Module tự xử lý toàn bộ: tạm pause reader task, gọi decoder seek, reset stream buffer, cập nhật position tracker, rồi resume reader.

Hoạt động cả khi đang play và khi đang pause. Nếu decoder không hỗ trợ seek (`.seek = NULL`), lệnh bị bỏ qua.

- **position_ms**: vị trí mong muốn (ms).

```c
bt_audio_seek_ms(0);        // Quay về đầu bài
bt_audio_seek_ms(30000);    // Seek đến 0:30
bt_audio_seek_ms(105000);   // Seek đến 1:45
```

### Volume

```c
void    bt_audio_set_volume(uint8_t volume_pct);   // 0–100
uint8_t bt_audio_get_volume(void);
```

Software volume với logarithmic curve -40dB qua lookup table, cho trải nghiệm volume tự nhiên (giống tai người cảm nhận). Giá trị >100 tự động set về 100.

### Status

```c
esp_err_t        bt_audio_get_position(bt_audio_playback_pos_t *pos);
esp_err_t        bt_audio_get_device_info(bt_audio_device_info_t *info);
const char      *bt_audio_get_title(void);
```

`bt_audio_get_title()` trả về tên bài hát từ file metadata. Rỗng nếu file không có metadata.

### Decoder Registration

```c
esp_err_t bt_audio_register_decoder(const bt_audio_decoder_t *decoder,
                                    const char *const extensions[],
                                    size_t ext_count);
```

WAV (`.wav`) và raw PCM (`.pcm`, `.raw`) đã đăng ký sẵn trong `bt_audio_init()`. Gọi thêm cho custom decoders.

## Workflow điển hình

### Basic

```c
void on_bt_event(const bt_audio_event_t *evt)
{
    switch (evt->type) {
    case BT_AUDIO_EVT_STATE_CHANGED: {
        const bt_audio_evt_state_changed_t *s = evt->data;

        if (s->new_state == BT_AUDIO_STATE_CONNECTED) {
            /* Kết nối thành công → phát nhạc ngay */
            bt_audio_play("/sdcard/song.wav");
        }

        if (s->new_state == BT_AUDIO_STATE_DISCONNECTED) {
            /* Mất kết nối → quét lại */
            bt_audio_start_discovery(true);
        }
        break;
    }

    case BT_AUDIO_EVT_TRACK_FINISHED:
        /* Bài hết → có thể play bài tiếp hoặc dừng */
        bt_audio_play("/sdcard/next_song.wav");
        break;

    default: break;
    }
}

void app_main(void)
{
    sdcard_init();
    bt_audio_init("ESP32_Player");
    bt_audio_register_callback(on_bt_event);
    bt_audio_start_discovery(true);     /* Tự kết nối thiết bị đầu tiên */
}
```

### Manual discovery — User chọn thiết bị

Khi muốn hiển thị danh sách thiết bị cho user chọn (ví dụ trên LCD, serial console, ...).

```c
/* Bước 1: Quét với auto_connect = false */
bt_audio_start_discovery(false);

/* Bước 2: Đợi DISCOVERY_DONE trong event callback, hoặc chờ ~12s */
vTaskDelay(pdMS_TO_TICKS(13000));

/* Bước 3: Lấy danh sách */
uint16_t count = BT_AUDIO_MAX_DISCOVERED;
bt_audio_discovered_dev_t devs[BT_AUDIO_MAX_DISCOVERED];
bt_audio_discovery_get_results(&count, devs);

/* Bước 4: Hiển thị cho user (serial, LCD, ...) */
for (int i = 0; i < count; i++) {
    printf("[%d] %s  RSSI:%d\n", i, devs[i].name, devs[i].rssi);
}
/* Hoặc dùng bt_audio_discovery_print() cho serial log */

/* Bước 5: User chọn → kết nối */
int user_choice = 0;    /* Ví dụ user chọn thiết bị #0 */
bt_audio_connect_by_index(user_choice);

/* Bước 6: Đợi event CONNECTED → play */
```

### Playlist — Phát nhiều bài liên tục

Dùng event `TRACK_FINISHED` để chuyển bài tự động.

```c
static const char *playlist[] = {
    "/sdcard/track01.wav",
    "/sdcard/track02.wav",
    "/sdcard/track03.raw",
};
static int s_track_index = 0;
static const int s_track_count = 3;

void on_bt_event(const bt_audio_event_t *evt)
{
    switch (evt->type) {
    case BT_AUDIO_EVT_STATE_CHANGED: {
        const bt_audio_evt_state_changed_t *s = evt->data;
        if (s->new_state == BT_AUDIO_STATE_CONNECTED) {
            s_track_index = 0;
            bt_audio_play(playlist[s_track_index]);
        }
        break;
    }

    case BT_AUDIO_EVT_TRACK_FINISHED:
        s_track_index++;
        if (s_track_index < s_track_count) {
            bt_audio_play(playlist[s_track_index]);
        }
        /* Hết playlist → không play nữa, state = IDLE */
        break;

    default: break;
    }
}
```

Lưu ý: `bt_audio_play()` gọi trong `TRACK_FINISHED` callback là an toàn — module không block trong hàm này. A2DP stream sẽ được restart tự động.

### Playback control

```c
bt_audio_play("/sdcard/music/song.wav");

bt_audio_pause();
bt_audio_resume();
bt_audio_seek_ms(105000);   // Seek đến 1:45
bt_audio_set_volume(50);

bt_audio_stop();
bt_audio_play("/sdcard/music/next.wav");
```

### Custom decoder (MP3)

```c
const bt_audio_decoder_t mp3_decoder = {
    .name = "MP3",
    .open = mp3_open,
    .read = mp3_read,
    .seek = mp3_seek,
    .close = mp3_close,
};

// Đăng ký sau khi init
const char *ext[] = {"mp3"};
bt_audio_register_decoder(&mp3_decoder, ext, 1);

// Play bình thường — auto detect
bt_audio_play("/sdcard/song.mp3");
```

## Cấu hình nội bộ

| Constant | Giá trị | Mô tả |
|----------|---------|--------|
| `STREAM_BUF_SIZE` | 8 KB | StreamBuffer size |
| `PREFILL_SIZE` | 4 KB | Ngưỡng bắt đầu phát |
| `READ_CHUNK` | 512 B | Chunk size cho reader task |
| `READER_STACK` | 4 KB | Stack size reader task |

Có thể điều chỉnh tùy theo heap khả dụng của board. BT Classic + Bluedroid chiếm đáng kể RAM, cần kiểm tra `esp_get_free_heap_size()` trước khi tăng buffer.