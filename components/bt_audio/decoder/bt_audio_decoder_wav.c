#include "bt_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE     *s_wav_file        = NULL;
static uint32_t  s_wav_data_offset = 0;
static uint32_t  s_wav_data_size   = 0;
static uint32_t  s_wav_bytes_read  = 0;

static inline uint16_t swap16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t swap32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

/**
 * Parse LIST/INFO tìm INAM (title).
 */
static void wav_parse_list_info(FILE *f, long list_end, char *title, size_t title_sz)
{
    uint8_t hdr[8];

    while (ftell(f) + 8 <= list_end && fread(hdr, 1, 8, f) == 8) {
        uint32_t sub_size = swap32(hdr + 4);

        if (memcmp(hdr, "INAM", 4) == 0 && sub_size > 0) {
            size_t to_read = (sub_size < title_sz - 1) ? sub_size : title_sz - 1;
            size_t got = fread(title, 1, to_read, f);
            title[got] = '\0';

            while (got > 0 && (title[got - 1] == '\0' || title[got - 1] == ' '))
                title[--got] = '\0';
            
            return;
        }

        fseek(f, (long)((sub_size + 1) & ~1), SEEK_CUR);
    }
}

static esp_err_t wav_open(const char *path, bt_audio_file_info_t *info)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    /* RIFF header */
    uint8_t riff[12] = {0};
    if (fread(riff, 1, 12, f) != 12) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t hdr[8] = {0};
    uint32_t data_offset = 0, data_size = 0;
    uint16_t audio_fmt = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;
    
    while (fread(hdr, 1, 8, f) == 8) {
        uint32_t sz = swap32(&hdr[4]);
        long data   = (uint32_t)ftell(f);
        
        if (memcmp(hdr, "fmt ", 4) == 0) {
            uint8_t fmt[16] = {0};

            if (sz < 16) { fseek(f, (long)sz, SEEK_CUR); continue; }
            if (fread(fmt, 1, 16, f) != 16) break;

            audio_fmt   = swap16(fmt);
            channels    = swap16(fmt + 2);
            bits        = swap16(fmt + 14);
            sample_rate = swap32(fmt + 4);

            if (sz > 16) fseek(f, data + (long)sz, SEEK_SET);
        }
        else if (memcmp(hdr, "data", 4) == 0) {
            data_offset = (uint32_t) data;
            data_size   = sz;
            fseek(f, data + (long)sz, SEEK_SET);
        }
        else if (memcmp(hdr, "LIST", 4) == 0 && sz >= 4) {
            char list_type[4];

            if (fread(list_type, 1, 4, f) != 4) break;

            if (memcmp(list_type, "INFO", 4) == 0) {
                long list_end = data + (long)sz;
                wav_parse_list_info(f, list_end, info->title, sizeof(info->title));
                fseek(f, list_end, SEEK_SET);

                if (data_offset) break;
            } else {
                fseek(f, data + (long)sz, SEEK_SET);
            }
        }
        else {
            fseek(f, data + (long)((sz + 1) & ~1), SEEK_SET);
        }
    }
    
    if (!data_offset) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    if (audio_fmt != 1) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    if (channels != BT_AUDIO_CHANNELS || bits != BT_AUDIO_BITS || sample_rate != BT_AUDIO_SAMPLE_RATE) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Parse metadata */
    info->total_pcm_bytes = data_size;
    info->title[0] = '\0';
    
    fseek(f, (long)data_offset, SEEK_SET);

    s_wav_file        = f;
    s_wav_data_offset = data_offset;
    s_wav_data_size   = data_size;
    s_wav_bytes_read  = 0;

    return ESP_OK;
}

static int wav_read(uint8_t *pcm_buf, size_t buf_size)
{
    if (!s_wav_file) return 0;

    uint32_t remaining = s_wav_data_size - s_wav_bytes_read;
    if (remaining == 0) return 0;

    size_t to_read = (buf_size < remaining) ? buf_size : remaining;
    size_t got = fread(pcm_buf, 1, to_read, s_wav_file);
    s_wav_bytes_read += (uint32_t)got;
    return (int)got;
}

static esp_err_t wav_seek(uint32_t pcm_offset)
{
    if (!s_wav_file) return ESP_ERR_INVALID_STATE;
    if (pcm_offset > s_wav_data_size) pcm_offset = s_wav_data_size;

    fseek(s_wav_file, (long)(s_wav_data_offset + pcm_offset), SEEK_SET);
    s_wav_bytes_read = pcm_offset;
    return ESP_OK;
}

static void wav_close(void)
{
    if (s_wav_file) { fclose(s_wav_file); s_wav_file = NULL; }
    s_wav_data_offset = 0;
    s_wav_data_size   = 0;
    s_wav_bytes_read  = 0;
}

const bt_audio_decoder_t bt_audio_wav_decoder = {
    .name  = "WAV",
    .open  = wav_open,
    .read  = wav_read,
    .seek  = wav_seek,
    .close = wav_close,
};
