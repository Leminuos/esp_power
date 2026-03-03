#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "display.h"
#include "sdcard.h"
#include "esp_err.h"
#include "esp_log.h"
#include "bt_audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char* TAG = "main";

#define UI_EVENT_BT_DISCOVERY_DONE      BIT0
#define UI_EVENT_BT_DEVICE_CONNECTED    BIT1  
#define UI_EVENT_TRACK_CHANGED          BIT2
#define UI_EVENT_TRACK_FINISHED         BIT3
#define UI_EVENT_BT_DEVICE_DISCONNECTED BIT4

static EventGroupHandle_t s_audio_event_group;
static TaskHandle_t s_audio_task;

extern void ui_refresh_file_list(const char *dir_path);

static void ui_audio_task(void* param)
{
    (void) param;
    EventBits_t bits = 0;

    for ( ; ; ) {
        bits = xEventGroupWaitBits(s_audio_event_group,
                                    UI_EVENT_TRACK_CHANGED | UI_EVENT_TRACK_FINISHED,
                                    pdTRUE,
                                    pdFALSE,
                                    portMAX_DELAY);
        
        if (bits & UI_EVENT_BT_DISCOVERY_DONE) {
            if (display_port_lock(100)) {
                ui_refresh_bt_device_list();
                display_port_unlock();
            }
        }

        if (bits & UI_EVENT_BT_DEVICE_CONNECTED) {
            if (display_port_lock(100)) {
                lv_scr_load_anim(ui_explorer, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
                ui_refresh_file_list("/sdcard");
                display_port_unlock();
            }
        }

        if (bits & UI_EVENT_TRACK_CHANGED) {
            if (display_port_lock(100)) {
                bt_audio_playback_pos_t p = {0};
                bt_audio_get_position(&p);
                lv_label_set_text_fmt(ui_lblTimeElapsed, "%02ld:%02ld",
                                    ((p.position_ms / 1000) / 60),
                                    ((p.position_ms / 1000) % 60));
                lv_slider_set_value(ui_sliderProgress, p.progress_pct, LV_ANIM_ON);
                display_port_unlock();
            }
        }

        if (bits & UI_EVENT_TRACK_FINISHED) {
            if (display_port_lock(100)) {
                lv_label_set_text(ui_lblBtnPlayPause, LV_SYMBOL_PLAY);
                display_port_unlock();
            }
        }

        if (bits & UI_EVENT_BT_DEVICE_DISCONNECTED) {
            if (display_port_lock(100)) {
                lv_scr_load_anim(ui_bt_select, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
                display_port_unlock();
            }
        }
    }
}

static void on_bt_event(const bt_audio_event_t *evt)
{
    switch (evt->type) {
    case BT_AUDIO_EVT_STATE_CHANGED: {
        const bt_audio_evt_state_changed_t *s = evt->data;
        ESP_LOGI(TAG, "State: %d -> %d", s->old_state, s->new_state);

        switch (s->new_state) {
        case BT_AUDIO_STATE_DISCOVERY_DONE:
            xEventGroupSetBits(s_audio_event_group, UI_EVENT_BT_DISCOVERY_DONE);
            break;

        case BT_AUDIO_STATE_CONNECTED: {
            bt_audio_device_info_t info;
            if (bt_audio_get_device_info(&info) == ESP_OK) {
                ESP_LOGI(TAG, "Connected to: %s (%s)", info.name, info.bda_str);
            }

            xEventGroupSetBits(s_audio_event_group, UI_EVENT_BT_DEVICE_CONNECTED);
            
            break;
        }

        case BT_AUDIO_STATE_PLAYING:
            break;

        case BT_AUDIO_STATE_PAUSED:
            break;

        case BT_AUDIO_STATE_DISCONNECTED:
            xEventGroupSetBits(s_audio_event_group, UI_EVENT_BT_DEVICE_DISCONNECTED);
            break;

        default:
            break;
        }
        break;
    }

    case BT_AUDIO_EVT_DATA_UPDATE: {
        xEventGroupSetBits(s_audio_event_group, UI_EVENT_TRACK_CHANGED);
        break;
    }
    
    case BT_AUDIO_EVT_TRACK_FINISHED:
        xEventGroupSetBits(s_audio_event_group, UI_EVENT_TRACK_FINISHED);
        break;

    default:
        break;
    }
}

void app_main(void)
{
    s_audio_event_group = xEventGroupCreate();
    bt_audio_init("esp32_player");
    bt_audio_register_callback(on_bt_event);
    ESP_ERROR_CHECK(sdcard_init());
    ESP_ERROR_CHECK(display_init());

    xTaskCreatePinnedToCore(ui_audio_task, "audio_task", 2048, NULL, 6, &s_audio_task, 1);

    if (display_port_lock(0)) {
        ui_init();
        ui_bt_select_start_scan();
        display_port_unlock();
    } else {
        ESP_LOGE("app", "Failed to lock LVGL for screen creation");
    }
}
