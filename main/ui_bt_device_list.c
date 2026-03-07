#include "ui.h"
#include "bt_audio.h"


extern lv_style_t style_item;
extern lv_style_t style_item_pressed;

static uint16_t s_count;
static uint16_t s_dev_count;
static lv_timer_t *s_discovery_timer ;
static bt_audio_discovered_dev_t * s_devs;

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
    lv_label_set_text_fmt(lbl_status, "Connecting to %s", name);
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
static void cb_device_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (idx < 0 || idx >= s_count) return;

    if (s_discovery_timer) {
        lv_timer_delete(s_discovery_timer);
        s_discovery_timer = NULL;
    }

    bt_audio_stop_discovery();

    // Hiển thị trạng thái connecting
    ui_bt_select_show_connecting(s_devs[idx].name);

    // Thực hiện kết nối BT
    bt_audio_connect_by_index(idx);

    free(s_devs); s_devs = NULL;
}

static void ui_refresh_bt_device_list(void) {
    bt_audio_discovery_get_count(&s_count);
    if (!s_count) return;

    if (s_devs) { free(s_devs); s_devs = NULL; }
    s_devs = (bt_audio_discovered_dev_t*) malloc(s_count * sizeof(bt_audio_discovered_dev_t));
    if (!s_devs) return;

    bt_audio_discovery_get_results(&s_count, s_devs);

    for (int i = s_dev_count; i < s_count; i++) {
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

        lv_obj_add_event_cb(item, cb_device_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    s_dev_count = s_count;
}

// ============================================================
// Bắt đầu scan
// ============================================================

static void discovery_poll_cb(lv_timer_t *timer)
{
    ui_refresh_bt_device_list();
}

void ui_bt_start_scan(void) {
    s_dev_count = 0;
    if (s_devs) { free(s_devs); s_devs = NULL; }
    lv_obj_clean(panel_device_list);
    
    ui_bt_select_show_scanning();
    s_discovery_timer = lv_timer_create(discovery_poll_cb, 300, NULL);

    // Bắt đầu BT scan
    bt_audio_start_discovery(false);
}

void ui_bt_stop_scan(void)
{
    if (!s_discovery_timer) return;

    lv_timer_delete(s_discovery_timer);
    s_discovery_timer = NULL;
    
    ui_refresh_bt_device_list();

    if (!s_count) {
        ui_bt_select_show_error("No audio devices found");
        return;
    }

    char status_buf[32];
    snprintf(status_buf, sizeof(status_buf), "Found %d device(s)", s_count);
    ui_bt_select_show_error(status_buf);
}
