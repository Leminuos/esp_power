/*
 * ESP32 A2DP Source — Phát audio qua Bluetooth A2DP
 * ESP-IDF 5.4+
 */

#include "bt_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"

static const char *TAG = "bt_audio";

/* ─── Buffer config ──────────────────────────────────────────────────────── */

#define BUF_SIZE            (16 * 1024)             /* 32 KB ≈ 186ms audio */
#define PREFILL_SIZE        (12 * 1024)             /* Ngưỡng bắt đầu phát */
#define READ_CHUNK_SIZE     512

/* ─── Reader task config ─────────────────────────────────────────────────── */

#define READER_TASK_STACK   (4 * 1024)
#define READER_TASK_PRIO    5
#define READER_STOP_WAIT_MS 500

/* ─── Atomic flags ───────────────────────────────────────────────────────── */

#define FLAG_PREFILLED      (1U << 1)
#define FLAG_PAUSED         (1U << 2)
#define FLAG_STOP_READER    (1U << 3)
#define FLAG_END_OF_STREAM  (1U << 4)

static atomic_uint s_flags = 0;
static inline bool bt_flag_get(uint32_t mask) { return (atomic_load(&s_flags) & mask) != 0; }
static inline void bt_flag_set(uint32_t mask) { atomic_fetch_or(&s_flags, mask); }
static inline void bt_flag_clear(uint32_t mask) { atomic_fetch_and(&s_flags, ~mask); }

/* ─── State ──────────────────────────────────────────────────────────────── */

static bool                         s_auto_connect  = false;
static atomic_uint_fast32_t         s_bytes_played  = 0;
static volatile uint8_t             s_volume        = 0;
static volatile int                 s_state         = BT_AUDIO_STATE_IDLE;
static bt_audio_device_info_t       s_device        = {0};
static bt_audio_event_cb_t          s_event_cb      = NULL;
static StreamBufferHandle_t         s_stream_buf    = NULL;
static bt_audio_file_info_t         s_file_info     = {0};

/* Reader task */
static TaskHandle_t                 s_reader_task     = NULL;
static const bt_audio_decoder_t    *s_decoder         = NULL;

/* Discovery */
static bt_audio_discovered_dev_t    s_disc_list[BT_AUDIO_MAX_DISCOVERED];
static int                          s_disc_count = 0;
static portMUX_TYPE                 s_disc_mux   = portMUX_INITIALIZER_UNLOCKED;

/* ─── Helpers ────────────────────────────────────────────────────────────── */

static void bt_format_bda(char *out, size_t out_size, const uint8_t *bda)
{
    snprintf(out, out_size,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static void bt_notify_event(const bt_audio_event_t *evt)
{
    if (s_event_cb) s_event_cb(evt);
}

static void bt_notify_signal(bt_audio_evt_type_t sig) {
    bt_audio_event_t evt = {
        .type = sig,
        .data = NULL,
        .data_size = 0
    };
    
    bt_notify_event(&evt);
}

/**
 * @brief Cập nhật state machine và gọi event callback nếu có thay đổi.
 */
static void bt_set_state(bt_audio_state_t new_state)
{
    bt_audio_state_t old = (bt_audio_state_t)s_state;
    if (old == new_state) return;
    s_state = (int)new_state;

    bt_audio_evt_state_changed_t payload = {
        .new_state = new_state,
        .old_state = old,
    };

    bt_audio_event_t evt = {
        .type      = BT_AUDIO_EVT_STATE_CHANGED,
        .data      = &payload,
        .data_size = sizeof(payload),
    };

    bt_notify_event(&evt);
}

/* ─── Decoder registry ───────────────────────────────────────────────────── */

#define MAX_DECODERS        8
#define MAX_EXT_LEN         8

typedef struct {
    const bt_audio_decoder_t *decoder;
    char ext[MAX_EXT_LEN];
} decoder_entry_t;

static decoder_entry_t s_registry[MAX_DECODERS];
static int             s_registry_count = 0;

esp_err_t bt_audio_register_decoder(
    const bt_audio_decoder_t *decoder,
    const char *const extensions[],
    size_t ext_count
)
{
    if (!decoder || !extensions || ext_count == 0) return ESP_ERR_INVALID_ARG;

    for (size_t i = 0; i < ext_count; i++) {
        if (s_registry_count >= MAX_DECODERS) {
            ESP_LOGE(TAG, "Decoder registry full");
            return ESP_ERR_NO_MEM;
        }

        decoder_entry_t *e = &s_registry[s_registry_count];
        e->decoder = decoder;

        const char *ext = extensions[i];
        if (ext[0] == '.') ext++;
        strncpy(e->ext, ext, MAX_EXT_LEN - 1);
        for (char *c = e->ext; *c; c++) {
            if (*c >= 'A' && *c <= 'Z') *c += 32;
        }

        s_registry_count++;
    }

    ESP_LOGI(TAG, "Registered decoder '%s' (%d ext)", decoder->name, (int)ext_count);
    return ESP_OK;
}

static const bt_audio_decoder_t *bt_find_decoder(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot || !dot[1]) return NULL;

    char ext[MAX_EXT_LEN] = {0};
    const char *src = dot + 1;
    for (int i = 0; i < MAX_EXT_LEN - 1 && src[i]; i++) {
        ext[i] = (src[i] >= 'A' && src[i] <= 'Z') ? src[i] + 32 : src[i];
    }

    for (int i = 0; i < s_registry_count; i++) {
        if (strcmp(s_registry[i].ext, ext) == 0)
            return s_registry[i].decoder;
    }

    return NULL;
}

/* ─── Volume ─────────────────────────────────────────────────────────────── */

/*
 * Volume lookup table
 * Logarithmic curve -40dB: factor = 256 × 10^((vol - 100) × 40 / (100 × 20))
 * vol=0 = mute, vol=100 = 0dB (factor 256, no attenuation).
 */
static const uint16_t s_vol_table[101] = {
    /*   0 */    0,    3,    3,    3,    3,    3,    3,    4,    4,    4,
    /*  10 */    4,    4,    4,    5,    5,    5,    5,    6,    6,    6,
    /*  20 */    6,    7,    7,    7,    8,    8,    8,    9,    9,   10,
    /*  30 */   10,   11,   11,   12,   12,   13,   13,   14,   15,   15,
    /*  40 */   16,   17,   18,   19,   19,   20,   21,   22,   23,   24,
    /*  50 */   26,   27,   28,   29,   31,   32,   34,   35,   37,   39,
    /*  60 */   41,   42,   44,   47,   49,   51,   53,   56,   59,   61,
    /*  70 */   64,   67,   71,   74,   77,   81,   85,   89,   93,   97,
    /*  80 */  102,  107,  112,  117,  123,  128,  134,  141,  147,  154,
    /*  90 */  162,  169,  177,  185,  194,  203,  213,  223,  233,  244,
    /* 100 */  256,
};

/**
 * @brief Apply software volume lên PCM buffer (16-bit signed stereo).
 */
static void bt_apply_volume(uint8_t *buf, size_t len, uint32_t vol)
{
    if (vol >= 100) return;

    uint16_t factor = s_vol_table[vol];
    if (factor == 0) {
        memset(buf, 0, len);
        return;
    }

    int16_t *samples = (int16_t *)buf;
    size_t count = len / sizeof(int16_t);

    for (size_t i = 0; i < count; i++) {
        samples[i] = (int16_t)((int32_t)samples[i] * factor >> 8);
    }
}

/* ─── Reader task ────────────────────────────────────────────────────────── */

static void bt_reader_task(void *arg)
{
    ESP_LOGI(TAG, "Free heap: %lu", (unsigned long)esp_get_free_heap_size());
    uint8_t *buf = malloc(READ_CHUNK_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Reader: malloc failed");
        goto exit;
    }

    while (!bt_flag_get(FLAG_STOP_READER)) {
        /* Pause: chờ resume, không đọc file */
        if (bt_flag_get(FLAG_PAUSED)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Gọi decoder đọc PCM data */
        int rd = s_decoder->read(buf, READ_CHUNK_SIZE);
        if (rd <= 0) break;

        /* Ghi vào stream buffer, retry nếu buffer đầy */
        size_t offset = 0;
        while (offset < (size_t)rd && !bt_flag_get(FLAG_STOP_READER)) {
            size_t written = xStreamBufferSend( s_stream_buf,
                                                buf + offset,
                                                (size_t)rd - offset,
                                                100 );
            
            offset += written;

            if (!bt_flag_get(FLAG_PREFILLED) && s_stream_buf) {
                if (xStreamBufferBytesAvailable(s_stream_buf) >= PREFILL_SIZE) {
                    bt_flag_set(FLAG_PREFILLED);
                }
            }
        }
    }

    free(buf);

    /*
     * Nếu thoát loop do end of stream (không phải force stop):
     *   - Set FLAG_EOS → a2dp_data_cb biết không còn data mới
     *   - Set FLAG_PREFILLED nếu chưa → cho phép phát nốt data còn trong buffer
     *     (dùng cho trường hợp file ngắn hơn PREFILL_SIZE)
     */
    if (!bt_flag_get(FLAG_STOP_READER)) {
        bt_flag_set(FLAG_END_OF_STREAM);
        if (!bt_flag_get(FLAG_PREFILLED) && s_stream_buf) {
            bt_flag_set(FLAG_PREFILLED);
        }
    }
    
exit:
    s_reader_task = NULL;
    vTaskDelete(NULL);
}

/* ─── Playback resource ───────────────────────────────────────────────────── */

static void bt_stop_reader(void)
{
    if (!s_reader_task) return;

    bt_flag_set(FLAG_STOP_READER);
    for (int i = 0; i < 10 && s_reader_task; i++)
        vTaskDelay(pdMS_TO_TICKS(10));

    /* force delete nếu task không thoát đúng hạn */
    if (s_reader_task) {
        vTaskDelete(s_reader_task);
        s_reader_task = NULL;
    }
}

/**
 * @brief Giải phóng tài nguyên playback, đảm bảo data callback không truy cập sau khi xóa.
 */
static void bt_cleanup_resource_playback(void)
{
    bt_stop_reader();

    if (s_decoder) {
        s_decoder->close();
        s_decoder = NULL;
    }

    atomic_store(&s_flags, 0);
    atomic_store(&s_bytes_played, 0);
    s_file_info.total_pcm_bytes = 0;
    s_file_info.title[0] = '\0';

    if (s_stream_buf) {
        vStreamBufferDelete(s_stream_buf);
        s_stream_buf = NULL;
    }
}

/* ─── A2DP data callback ─────────────────────────────────────────────────── *
 *
 * Gọi bởi BT stack mỗi khi cần data để encode SBC.
 * Tần suất: ~7ms (phụ thuộc SBC frame size và bitpool).
 * Phải return nhanh — không block, không malloc, không log (trừ warning).
 *
 * Params:
 *   out: buffer để ghi PCM data vào (BT stack sẽ encode SBC từ đây)
 *   len: số bytes cần ghi (thường ~512-2048 bytes)
 *
 * Return: luôn return len (BT stack cần đúng số byte yêu cầu).
 * Nếu không đủ data thì cần padding zero (silence).
 */
static int32_t bt_audio_a2dp_data_cb(uint8_t *out, int32_t len)
{
    if (!out || len <= 0) return 0;

    uint32_t flags = atomic_load(&s_flags);

    /* Output silence nếu:
     *   - Chưa có stream buffer
     *   - Chưa prefill đủ (tránh underrun)
     *   - Đang pause
     */
    if (!s_stream_buf || !(flags & FLAG_PREFILLED) || (flags & FLAG_PAUSED)) {
        memset(out, 0, len);
        return len;
    }

     /* Lấy PCM data từ stream buffer (non-blocking, timeout = 0) */
    size_t got = xStreamBufferReceive(s_stream_buf, out, (size_t)len, 0);
    if (got > 0) atomic_fetch_add(&s_bytes_played, (uint_fast32_t)got);
    else ESP_LOGW(TAG, "CB: underrun (%ld requested)", (long)len);

    /* Padding silence nếu buffer không đủ data (underrun) */
    if ((int32_t)got < len) memset(out + got, 0, len - got);

    /* Apply software volume */
    bt_apply_volume(out, (size_t)len, s_volume);

    /* Bắn sự kiện DATA_UPDATE event */
    bt_notify_signal(BT_AUDIO_EVT_DATA_UPDATE);

    /* Check end of stream */
    if ((flags & FLAG_END_OF_STREAM) && xStreamBufferBytesAvailable(s_stream_buf) == 0) {
        bt_flag_clear(FLAG_END_OF_STREAM);
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
        bt_notify_signal(BT_AUDIO_EVT_TRACK_FINISHED);
    }

    return len;
}

/* ─── A2DP event callback ────────────────────────────────────────────────── */

static void bt_audio_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        const uint8_t *bda = param->conn_stat.remote_bda;

        switch (param->conn_stat.state) {
        case ESP_A2D_CONNECTION_STATE_CONNECTING:
            bt_set_state(BT_AUDIO_STATE_CONNECTING);
            break;

        case ESP_A2D_CONNECTION_STATE_CONNECTED:
            s_device.connected = true;
            memcpy(s_device.bda, bda, 6);
            bt_format_bda(s_device.bda_str, sizeof(s_device.bda_str), bda);
            bt_set_state(BT_AUDIO_STATE_CONNECTED);
            break;

        case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
            break;

        case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
            bt_cleanup_resource_playback();
            memset(&s_device, 0, sizeof(s_device));
            bt_set_state(BT_AUDIO_STATE_DISCONNECTED);
            break;
        }
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT: {
        switch (param->audio_stat.state) {
        case ESP_A2D_AUDIO_STATE_STARTED:
            /* BT sink accept stream → bắt đầu phát */
            ESP_LOGI(TAG, "Audio stream started");
            bt_set_state(BT_AUDIO_STATE_PLAYING);
            break;

        case ESP_A2D_AUDIO_STATE_STOPPED:
            /* Stream dừng (do bt_audio_stop() hoặc auto stop sau track finished) */
            ESP_LOGI(TAG, "Audio stream stopped");
            bt_cleanup_resource_playback();
            bt_set_state(BT_AUDIO_STATE_IDLE);
            break;

        default:
            break;
        }
        break;
    }

    case ESP_A2D_AUDIO_CFG_EVT: {
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            int sample_rate = 16000;
            uint8_t oct0 = param->audio_cfg.mcc.cie.sbc[0];
            if      (oct0 & 0x40) sample_rate = 32000;
            else if (oct0 & 0x20) sample_rate = 44100;
            else if (oct0 & 0x10) sample_rate = 48000;
            ESP_LOGI(TAG, "SBC codec configured: %d Hz", sample_rate);
        }
        break;
    }

    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        ESP_LOGI(TAG, "Media ctrl ACK cmd=%d st=%d",
                 param->media_ctrl_stat.cmd,
                 param->media_ctrl_stat.status);
        break;

    default:
        ESP_LOGD(TAG, "A2DP event %d", event);
        break;
    }
}

/* ─── GAP callback ───────────────────────────────────────────────────────── */

static void bt_audio_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        uint8_t *bda = param->disc_res.bda;

        char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = "<unknown>";
        int8_t rssi = -127;
        uint32_t cod = 0;

        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];

            switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_BDNAME: {
                int l = p->len;
                if (l > ESP_BT_GAP_MAX_BDNAME_LEN) l = ESP_BT_GAP_MAX_BDNAME_LEN;
                memcpy(name, p->val, l);
                name[l] = '\0';
                break;
            }
            case ESP_BT_GAP_DEV_PROP_RSSI:
                if (p->len >= 1) rssi = *(int8_t *)p->val;
                break;
            case ESP_BT_GAP_DEV_PROP_COD:
                if (p->len >= 4) cod = *(uint32_t *)p->val;
                break;
            default:
                break;
            }
        }

        /* Filter: Audio/Video major class = 0x04 */
        bool is_audio = ((cod >> 8) & 0x1F) == 0x04;

        /* Bỏ qua thiết bị non-audio */
        if (!is_audio) break;

        /* Auto-connect: kết nối thiết bị audio đầu tiên tìm thấy */
        if (s_auto_connect) {
            strncpy(s_device.name, name, sizeof(s_device.name) - 1);
            esp_bt_gap_cancel_discovery();
            esp_a2d_source_connect(bda);
            break;
        }

        /* Manual mode: thêm vào danh sách discovery */
        taskENTER_CRITICAL(&s_disc_mux);
        {
            /* Kiểm tra duplicate — cập nhật name/rssi nếu đã có */
            bool dup = false;
            for (int i = 0; i < s_disc_count; i++) {
                if (memcmp(s_disc_list[i].bda, bda, 6) == 0) {
                    if (name[0] && !s_disc_list[i].name[0]) {
                        strncpy(s_disc_list[i].name, name,
                                sizeof(s_disc_list[i].name) - 1);
                    }
                    if (rssi > s_disc_list[i].rssi) {
                        s_disc_list[i].rssi = rssi;
                    }
                    dup = true;
                    break;
                }
            }

            if (!dup && s_disc_count < BT_AUDIO_MAX_DISCOVERED) {
                bt_audio_discovered_dev_t *d = &s_disc_list[s_disc_count];
                strncpy(d->name, name, sizeof(d->name) - 1);
                memcpy(d->bda, bda, 6);
                bt_format_bda(d->bda_str, sizeof(d->bda_str), bda);
                d->rssi = rssi;
                d->cod  = cod;
                s_disc_count++;
            }
        }
        taskEXIT_CRITICAL(&s_disc_mux);
        bt_notify_signal(BT_AUDIO_EVT_DISCOVERY_RESULT);

        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(TAG, "Discovery started...");
            bt_set_state(BT_AUDIO_STATE_DISCOVERING);
        }
        else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "Discovery stopped");
            bt_set_state(BT_AUDIO_STATE_DISCOVERY_DONE);
        }
        break;

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Paired with: %s", param->auth_cmpl.device_name);

            /* Cập nhật tên từ pairing — chính xác hơn discovery */
            if (param->auth_cmpl.device_name[0] != '\0') {
                strncpy(s_device.name, (const char *)param->auth_cmpl.device_name,
                        sizeof(s_device.name) - 1);
            }
        } else {
            ESP_LOGE(TAG, "Pairing failed: %d", param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP confirm passkey: %lu", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey: %lu", param->key_notif.passkey);
        break;

    default:
        ESP_LOGD(TAG, "GAP event %d", event);
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*                              PUBLIC API                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

esp_err_t bt_audio_init(const char *device_name)
{
    esp_err_t ret;

    /* 1. NVS */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. BT controller */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    /* 3. Bluedroid stack */
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* 4. Callbacks */
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_audio_gap_cb));
    ESP_ERROR_CHECK(esp_a2d_register_callback(bt_audio_a2dp_cb));

    /* 5. A2DP Source */
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(bt_audio_a2dp_data_cb));
    ESP_ERROR_CHECK(esp_a2d_source_init());

    /* 6. Device name & scan mode */
    esp_bt_gap_set_device_name(device_name);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    /* 7. SSP — No Input No Output (auto-accept) */
    esp_bt_sp_param_t sp_param = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(sp_param, &iocap, sizeof(uint8_t));

    /* 8. Reset state */
    s_state      = BT_AUDIO_STATE_IDLE;
    s_volume     = 100;
    s_decoder    = NULL;
    s_disc_count = 0;
    atomic_store(&s_bytes_played, 0);
    atomic_store(&s_flags, 0);
    memset(&s_device, 0, sizeof(s_device));

    s_file_info.total_pcm_bytes  = 0;
    memset(&s_file_info.title, 0, sizeof(s_file_info.title));

    bt_set_state(BT_AUDIO_STATE_IDLE);

    /* Đăng ký decoder interface */
    s_registry_count = 0;
    const char *wav_ext[] = {"wav"};
    bt_audio_register_decoder(&bt_audio_wav_decoder, wav_ext, 1);
    const char *raw_ext[] = {"pcm", "raw"};
    bt_audio_register_decoder(&bt_audio_raw_decoder, raw_ext, 2);

    ESP_LOGI(TAG, "BT Audio Source initialized: '%s'", device_name);
    return ESP_OK;
}

void bt_audio_register_callback(bt_audio_event_cb_t callback)
{
    s_event_cb = callback;
}

/* ─── Discovery & connection ─────────────────────────────────────────────── */

void bt_audio_start_discovery(bool auto_connect)
{
    ESP_LOGI(TAG, "Starting discovery (auto_connect=%d)...", auto_connect);

    taskENTER_CRITICAL(&s_disc_mux);
    s_disc_count = 0;
    s_auto_connect = auto_connect;
    memset(s_disc_list, 0, sizeof(s_disc_list));
    taskEXIT_CRITICAL(&s_disc_mux);

    /* 10 inquiry cycles ≈ 12 giây */
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
}

void bt_audio_stop_discovery(void)
{
    esp_bt_gap_cancel_discovery();
}

esp_err_t bt_audio_discovery_get_count(uint16_t *count)
{
    if (!count) return ESP_ERR_INVALID_ARG;

    taskENTER_CRITICAL(&s_disc_mux);
    *count = (uint16_t)s_disc_count;
    taskEXIT_CRITICAL(&s_disc_mux);

    return ESP_OK;
}

esp_err_t bt_audio_discovery_get_results(uint16_t *count, bt_audio_discovered_dev_t *devs)
{
    if (!count || !devs) return ESP_ERR_INVALID_ARG;

    taskENTER_CRITICAL(&s_disc_mux);
    {
        uint16_t copy_count = (*count < (uint16_t)s_disc_count)
                            ? *count : (uint16_t)s_disc_count;
        memcpy(devs, s_disc_list, copy_count * sizeof(bt_audio_discovered_dev_t));
        *count = copy_count;
    }
    taskEXIT_CRITICAL(&s_disc_mux);

    return ESP_OK;
}

/** In danh sách discovery ra serial log dạng bảng. */
void bt_audio_discovery_print(void)
{
    taskENTER_CRITICAL(&s_disc_mux);
    int count = s_disc_count;
    bt_audio_discovered_dev_t devs[BT_AUDIO_MAX_DISCOVERED];
    if (count > 0) {
        memcpy(devs, s_disc_list, count * sizeof(bt_audio_discovered_dev_t));
    }
    taskEXIT_CRITICAL(&s_disc_mux);

    if (count == 0) {
        ESP_LOGI(TAG, "No discovered devices");
        return;
    }

    ESP_LOGI(TAG, "┌─────────────────────────────────────────────────────────────┐");
    ESP_LOGI(TAG, "│  # │ Name                 │ Address           │ RSSI │ CoD  │");
    ESP_LOGI(TAG, "├─────────────────────────────────────────────────────────────┤");
    for (int i = 0; i < count; i++) {
        const bt_audio_discovered_dev_t *d = &devs[i];
        ESP_LOGI(TAG, "│ %2d │ %-20.20s │ %s │ %4d │ %04lx │",
                 i,
                 d->name[0] ? d->name : "<unknown>",
                 d->bda_str,
                 d->rssi,
                 (unsigned long)(d->cod & 0xFFFF));
    }
    ESP_LOGI(TAG, "└─────────────────────────────────────────────────────────────┘");
    ESP_LOGI(TAG, "Total: %d device(s)", count);
}

esp_err_t bt_audio_connect_by_index(int index)
{
    uint8_t bda[6];
    char name[64] = "";
    bool valid = false;

    taskENTER_CRITICAL(&s_disc_mux);
    if (index >= 0 && index < s_disc_count) {
        memcpy(bda, s_disc_list[index].bda, 6);
        strncpy(name, s_disc_list[index].name, sizeof(name) - 1);
        valid = true;
    }
    taskEXIT_CRITICAL(&s_disc_mux);

    if (!valid) {
        ESP_LOGE(TAG, "Invalid device index: %d (have %d)", index, s_disc_count);
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_device.name, name, sizeof(s_device.name) - 1);
    return bt_audio_connect(bda);
}

esp_err_t bt_audio_connect(const uint8_t bda[6])
{
    if (!bda) return ESP_ERR_INVALID_ARG;

    esp_bt_gap_cancel_discovery();

    char bda_str[18];
    bt_format_bda(bda_str, sizeof(bda_str), bda);
    ESP_LOGI(TAG, "Connecting to %s ...", bda_str);

    return esp_a2d_source_connect((uint8_t *)bda);
}

esp_err_t bt_audio_disconnect(void)
{
    if (!s_device.connected) return ESP_ERR_INVALID_STATE;

    bt_audio_stop();
    return esp_a2d_source_disconnect(s_device.bda);
}

/* ─── Playback ───────────────────────────────────────────────────────────── */

esp_err_t bt_audio_play(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!s_device.connected) return ESP_ERR_INVALID_STATE;

    /* Stop bài đang phát (nếu có) trước khi play bài mới */
    if (s_reader_task || s_stream_buf) bt_audio_stop();

    /* Tìm decoder phù hợp với extension của file. Ví dụ: *.wav, *.raw,... */
    const bt_audio_decoder_t *dec = bt_find_decoder(path);
    if (!dec) {
        ESP_LOGE(TAG, "No decoder for '%s'", path);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Open file và lấy thông tin như kích thước pcm, sample rate,
     * số channel, một sample chứa bao nhiêu bit
     */
    esp_err_t ret = dec->open(path, &s_file_info);
    if (ret != ESP_OK) return ret;

    /* Tạo streambuffer để truyền nhận dữ liệu realtime giữa consumer và provider */
    StreamBufferHandle_t buf = xStreamBufferCreate(BUF_SIZE, BT_AUDIO_FRAME_SIZE);
    if (!buf) { dec->close(); return ESP_ERR_NO_MEM; }

    /* Setup state cho playback mới */
    s_stream_buf = buf;
    s_decoder    = dec;
    atomic_store(&s_bytes_played, 0);
    atomic_store(&s_flags, 0);

    /* Tạo reader task — bắt đầu đọc file */
    if (xTaskCreatePinnedToCore(bt_reader_task, "reader_task",
                                READER_TASK_STACK, NULL,
                                READER_TASK_PRIO, &s_reader_task, 1) != pdPASS) {
        dec->close();
        s_decoder = NULL;
        vStreamBufferDelete(buf);
        s_stream_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Gửi lệnh media ctrl start đến BT Stack để bắt đầu gửi data PCM */
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);

    ESP_LOGI(TAG, "Play [%s]: '%s' (%.1fs)", dec->name, path, s_file_info.total_pcm_bytes ? (float)s_file_info.total_pcm_bytes / BT_AUDIO_BITRATE : 0.0f);

    return ESP_OK;
}

void bt_audio_pause(void)
{
    bt_flag_set(FLAG_PAUSED);
    bt_set_state(BT_AUDIO_STATE_PAUSED);
}

void bt_audio_resume(void)
{
    bt_flag_clear(FLAG_PAUSED);
    bt_set_state(BT_AUDIO_STATE_PLAYING);
}

void bt_audio_stop(void)
{
    bt_stop_reader();

    /* Gửi lệnh media ctrl stop đến BT Stack để yêu cầu dừng gửi data PCM */
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
}

void bt_audio_seek(uint32_t position_ms)
{
    if (!s_stream_buf || !s_decoder || !s_decoder->seek) return;

    /* Tính toán offset trong file tương ứng với `position_ms` */
    uint32_t offset = (uint32_t)((uint64_t)position_ms * BT_AUDIO_BITRATE / 1000);
    offset = (offset / BT_AUDIO_FRAME_SIZE) * BT_AUDIO_FRAME_SIZE;
    if (offset > s_file_info.total_pcm_bytes) offset = s_file_info.total_pcm_bytes;

    /* Tạm thời pause để thực hiện seek tới offset */
    bool was_paused = bt_flag_get(FLAG_PAUSED);
    if (!was_paused) bt_flag_set(FLAG_PAUSED);

    if (s_decoder->seek(offset) == ESP_OK) {

        // Xóa dữ liệu cũ trong stream buffer để tránh phát lại
        bt_flag_clear(FLAG_PREFILLED | FLAG_END_OF_STREAM);
        xStreamBufferReset(s_stream_buf);
        atomic_store(&s_bytes_played, (uint_fast32_t)offset);
    }

    if (!was_paused) bt_flag_clear(FLAG_PAUSED);
}

esp_err_t bt_audio_get_position(bt_audio_playback_pos_t *pos)
{
    if (!pos) return ESP_ERR_INVALID_ARG;

    uint32_t played = (uint32_t)atomic_load(&s_bytes_played);

    pos->position_ms  = (uint32_t)((uint64_t)played * 1000 / BT_AUDIO_BITRATE);
    pos->duration_ms  = s_file_info.total_pcm_bytes ? (uint32_t)((uint64_t)s_file_info.total_pcm_bytes * 1000 / BT_AUDIO_BITRATE) : 0;
    pos->progress_pct = s_file_info.total_pcm_bytes ? (uint8_t)((uint64_t)played * 100 / s_file_info.total_pcm_bytes) : 0;

    return ESP_OK;
}

void bt_audio_set_volume(uint8_t volume_pct)
{
    if (volume_pct > 100) volume_pct = 100;
    s_volume = volume_pct;
}

uint8_t bt_audio_get_volume(void)
{
    return (uint8_t)atomic_load(&s_volume);
}

esp_err_t bt_audio_get_device_info(bt_audio_device_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;

    if (!s_device.connected) {
        memset(info, 0, sizeof(*info));
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(info, &s_device, sizeof(*info));
    return ESP_OK;
}

const char *bt_audio_get_title(void) { return s_file_info.title; }
