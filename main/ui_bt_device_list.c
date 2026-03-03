#include "ui.h"
#include "bt_audio.h"

// ============================================================
// UI States
// ============================================================
void ui_bt_select_show_scanning(void) {
    lv_label_set_text(lbl_status, "Searching for devices...");
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);        // hiện spinner
    lv_obj_add_flag(btn_rescan, LV_OBJ_FLAG_HIDDEN);       // ẩn nút rescan
    lv_obj_clean(panel_device_list);                       // xóa list cũ
}

void ui_bt_select_show_results(void) {
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);          // ẩn spinner
    lv_obj_clear_flag(btn_rescan, LV_OBJ_FLAG_HIDDEN);     // hiện nút rescan
}

void ui_bt_select_show_connecting(const char *name) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Connecting to %s...", name);
    lv_label_set_text(lbl_status, buf);
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(btn_rescan, LV_OBJ_FLAG_HIDDEN);
}

void ui_bt_select_show_error(const char* msg) {
    lv_label_set_text(lbl_status, msg);
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(btn_rescan, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================
// Hiển thị danh sách devices
// ============================================================
static uint16_t s_count = 0;
static bt_audio_discovered_dev_t * s_devs;

static void cb_device_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (idx < 0 || idx >= s_count) return;

    // Hiển thị trạng thái connecting
    ui_bt_select_show_connecting(s_devs[idx]->name);

    // Thực hiện kết nối BT
    bt_audio_connect_by_index(idx);
}

void ui_refresh_bt_device_list(void) {
    bt_audio_discovery_get_count(&s_count);
    s_devs = (bt_audio_discovered_dev_t*) malloc(s_count * sizoef(bt_audio_discovered_dev_t));
    bt_audio_discovery_get_results(&s_count, s_devs);

    lv_obj_clean(panel_device_list);

    if (s_count == 0) {
        ui_bt_select_show_error("No audio devices found");
        return;
    }

    char status_buf[32];
    snprintf(status_buf, sizeof(status_buf), "Found %d device(s)", s_count);
    ui_bt_select_show_error(lbl_status, status_buf);

    for (int i = 0; i < s_count; i++) {
        // Bỏ qua device không có tên
        if (s_devs[i].name[0] == '\0') continue;

        // Container cho 1 device
        lv_obj_t *item = lv_obj_create(panel_device_list);
        lv_obj_set_size(item, lv_pct(100), 36);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(item, &style_item, LV_STATE_DEFAULT);
        lv_obj_add_style(item, &style_item_pressed, LV_STATE_PRESSED);

        // Icon Bluetooth
        lv_obj_t *icon = lv_label_create(item);
        lv_label_set_text(icon, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x4FC3F7), 0);

        // Tên device
        lv_obj_t *lbl_name = lv_label_create(item);
        lv_label_set_text(lbl_name, s_devs[i].name);
        lv_obj_set_flex_grow(lbl_name, 1);
        lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(lbl_name, lv_color_hex(0xFFFFFF), 0);

        // RSSI indicator
        lv_obj_t *lbl_rssi = lv_label_create(item);
        lv_label_set_text_fmt(lbl_rssi, "%d", s_devs[i].rssi);

        // Màu RSSI theo cường độ tín hiệu
        if (dev->rssi > -50) lv_obj_add_style(lbl_rssi, &style_rssi_good, 0);       // mạnh
        else if (dev->rssi > -70) lv_obj_add_style(lbl_rssi, &style_rssi_medium, 0);// trung bình
        else lv_obj_add_style(lbl_rssi, &style_rssi_weak, 0);                       // yếu

        lv_obj_add_event_cb(item, cb_device_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

// ============================================================
// Bắt đầu scan
// ============================================================
void ui_bt_select_start_scan(void) {
    ui_bt_select_show_scanning();

    // Bắt đầu BT scan
    bt_audio_start_discovery(false);
}
