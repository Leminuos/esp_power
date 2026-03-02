#ifndef __BT_AUDIO_
#define __BT_AUDIO_

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BT_AUDIO_MODE_SINK      0
#define BT_AUDIO_MODE_SOURCE    1

#ifndef BT_AUDIO_MODE
#define BT_AUDIO_MODE           BT_AUDIO_MODE_SOURCE
#endif /* BT_AUDIO_MODE */

#define BT_AUDIO_SAMPLE_RATE    44100
#define BT_AUDIO_CHANNELS       2
#define BT_AUDIO_BITS           16
#define BT_AUDIO_BITRATE        (BT_AUDIO_SAMPLE_RATE * BT_AUDIO_CHANNELS * (BT_AUDIO_BITS / 8))
#define BT_AUDIO_FRAME_SIZE     (BT_AUDIO_CHANNELS * (BT_AUDIO_BITS / 8))

/**
 * @brief Thông tin file audio — decoder trả về sau khi open.
 */
typedef struct {
    uint32_t    total_pcm_bytes;    /**< Tổng PCM data (bytes), 0 nếu không biết */
    uint32_t    sample_rate;        /**< Sample rate (Hz) */
    uint16_t    channels;           /**< Số kênh */
    uint16_t    bits_per_sample;    /**< Bits per sample */
} bt_audio_file_info_t;

/**
 * @brief Decoder interface — abstract layer cho các format audio khác nhau.
 */
typedef struct {
    const char *name;
    /**
     * @brief Mở file và đọc metadata.
     *
     * @param      path  Đường dẫn file
     * @param[out] info  Thông tin file
     * @return ESP_OK nếu thành công
     */
    esp_err_t (*open)(const char *path, bt_audio_file_info_t *info);

    /**
     * @brief Đọc PCM data. Output: signed 16-bit LE, stereo, 44100 Hz.
     *
     * @param pcm_buf   Buffer output
     * @param buf_size  Kích thước buffer (bytes)
     * @return Bytes thực tế ghi vào pcm_buf, 0 = EOF
     */
    int (*read)(uint8_t *pcm_buf, size_t buf_size);

    /**
     * @brief Seek theo PCM byte offset. NULL nếu không hỗ trợ.
     *
     * @param pcm_offset  Byte offset trong PCM output (frame-aligned)
     * @return ESP_OK nếu thành công
     */
    esp_err_t (*seek)(uint32_t pcm_offset);

    /**
     * @brief Đóng file và free context.
     */
    void (*close)();
} bt_audio_decoder_t;

extern const bt_audio_decoder_t bt_audio_wav_decoder;
extern const bt_audio_decoder_t bt_audio_raw_decoder;

esp_err_t bt_audio_register_decoder(
    const bt_audio_decoder_t *decoder,
    const char *const extensions[],
    size_t ext_count
);

/* ─── Discovery ───────────────────────────────────────────────────────────── */

/** Số thiết bị tối đa lưu trong danh sách discovery. */
#define BT_AUDIO_MAX_DISCOVERED     8

/**
 * @brief Thông tin thiết bị phát hiện được qua BT discovery.
 */
typedef struct {
    char        name[64];       /**< Tên thiết bị (có thể rỗng nếu không quảng bá) */
    uint8_t     bda[6];         /**< BD_ADDR raw 6 bytes */
    char        bda_str[18];    /**< BD_ADDR dạng "xx:xx:xx:xx:xx:xx" */
    int8_t      rssi;           /**< Cường độ tín hiệu (dBm) */
    uint32_t    cod;            /**< Class of Device — dùng để filter audio device */
} bt_audio_discovered_dev_t;

/* ─── State machine ───────────────────────────────────────────────────────── */

typedef enum {
    BT_AUDIO_STATE_IDLE,
    BT_AUDIO_STATE_DISCOVERING,     /**< Đang quét thiết bị */
    BT_AUDIO_STATE_DISCOVERY_DONE,  /**< Đã quét xong */
    BT_AUDIO_STATE_CONNECTING,      /**< Đang kết nối đến thiết bị */
    BT_AUDIO_STATE_CONNECTED,       /**< Đã kết nối, sẵn sàng phát */
    BT_AUDIO_STATE_PLAYING,         /**< Đang phát audio */
    BT_AUDIO_STATE_PAUSED,          /**< Đã tạm dừng */
    BT_AUDIO_STATE_DISCONNECTED     /**< Mất kết nối */
} bt_audio_state_t;

/* ─── Event system ────────────────────────────────────────────────────────── */

typedef enum {
    BT_AUDIO_EVT_STATE_CHANGED,     /**< Trạng thái audio thay đổi */
    BT_AUDIO_EVT_DISCOVERY_RESULT,  /**< Tìm thấy thiết bị mới */
    BT_AUDIO_EVT_DATA_UPDATE,
    BT_AUDIO_EVT_TRACK_FINISHED,    /**< Track phát xong */
} bt_audio_evt_type_t;

typedef struct {
    bt_audio_evt_type_t type;       /**< Loại event */
    const void         *data;       /**< Pointer đến event data, NULL nếu không có */
    size_t              data_size;  /**< Kích thước data (bytes) */
} bt_audio_event_t;

typedef struct {
    bt_audio_state_t    new_state;
    bt_audio_state_t    old_state;
} bt_audio_evt_state_changed_t;

/**
 * @brief Callback thông báo events.
 *
 * Được gọi từ BT task. Callback cần thực hiện nhanh chóng, không block.
 */
typedef void (*bt_audio_event_cb_t)(const bt_audio_event_t *event);

/* ─── Device info ─────────────────────────────────────────────────────────── */

/**
 * @brief Thông tin thiết bị bluetooth đang kết nối.
 */
typedef struct {
    char        name[64];       /**< Tên thiết bị */
    uint8_t     bda[6];         /**< BD_ADDR raw 6 bytes */
    char        bda_str[18];    /**< BD_ADDR dạng "xx:xx:xx:xx:xx:xx" */
    bool        connected;      /**< true nếu đang kết nối */
} bt_audio_device_info_t;

/* ─── Playback position ──────────────────────────────────────────────────── */

/**
 * @brief Thông tin vị trí phát — dùng cho UI progress bar / timestamp.
 */
typedef struct {
    uint32_t position_ms;       /**< Vị trí phát hiện tại (ms) */
    uint32_t duration_ms;       /**< Tổng thời lượng (ms) */
    uint8_t  progress_pct;      /**< 0–100%, 0 nếu chưa có duration */
} bt_audio_playback_pos_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*                              PUBLIC API                                     */
/* ═══════════════════════════════════════════════════════════════════════════ */

void bt_audio_snk(void);

esp_err_t bt_audio_init(const char *device_name);
void      bt_audio_register_callback(bt_audio_event_cb_t callback);

void      bt_audio_start_discovery(bool auto_connect);
void      bt_audio_stop_discovery(void);
void      bt_audio_discovery_print(void);
esp_err_t bt_audio_discovery_get_count(uint16_t *count);
esp_err_t bt_audio_discovery_get_results(uint16_t *count, bt_audio_discovered_dev_t *devs);
esp_err_t bt_audio_connect_by_index(int index);
esp_err_t bt_audio_connect(const uint8_t bda[6]);
esp_err_t bt_audio_disconnect(void);

esp_err_t bt_audio_play(const char *path);
void      bt_audio_pause(void);
void      bt_audio_resume(void);
void      bt_audio_stop(void);
void      bt_audio_seek(uint32_t position_ms);

void      bt_audio_set_volume(uint8_t volume_pct);
uint8_t   bt_audio_get_volume(void);

esp_err_t bt_audio_get_position(bt_audio_playback_pos_t *pos);
esp_err_t bt_audio_get_device_info(bt_audio_device_info_t *info);

#endif /* __BT_AUDIO_ */
