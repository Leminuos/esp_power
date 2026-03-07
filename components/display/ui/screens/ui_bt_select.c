#include "../ui.h"

lv_obj_t *ui_bt_select      = NULL;
lv_obj_t *lbl_title         = NULL;
lv_obj_t *lbl_status        = NULL;
lv_obj_t *panel_device_list = NULL;
lv_obj_t *btn_rescan        = NULL;
lv_obj_t *spinner           = NULL;   // loading animation

// Styles
lv_style_t style_item;
lv_style_t style_item_pressed;

static lv_style_t style_screen;

// ============================================================
// Styles
// ============================================================
static void init_bt_select_styles(void) {
    // Screen background
    lv_style_init(&style_screen);
    lv_style_set_bg_color(&style_screen, lv_color_hex(0x1A1A2E));
    lv_style_set_bg_opa(&style_screen, LV_OPA_COVER);

    // Device item — bình thường
    lv_style_init(&style_item);
    lv_style_set_bg_opa(&style_item, LV_OPA_TRANSP);
    lv_style_set_border_width(&style_item, 0);
    lv_style_set_radius(&style_item, 6);
    lv_style_set_pad_all(&style_item, 10);
    lv_style_set_pad_column(&style_item, 10);

    // Device item — pressed
    lv_style_init(&style_item_pressed);
    lv_style_set_bg_opa(&style_item_pressed, LV_OPA_30);
    lv_style_set_bg_color(&style_item_pressed, lv_color_hex(0x4FC3F7));
}

void ui_bt_select_screen_init(void) {
    init_bt_select_styles();

    ui_bt_select = lv_obj_create(NULL);
    lv_obj_add_style(ui_bt_select, &style_screen, 0);
    lv_obj_clear_flag(ui_bt_select, LV_OBJ_FLAG_SCROLLABLE);

    // --- Title ---
    lbl_title = lv_label_create(ui_bt_select);
    lv_label_set_text(lbl_title, LV_SYMBOL_BLUETOOTH " Bluetooth Audio");
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xFFFFFF), 0);

    // --- Status label ---
    lbl_status = lv_label_create(ui_bt_select);
    lv_label_set_text(lbl_status, "");
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 35);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xAAAAAA), 0);

    // --- Spinner ---
    spinner = lv_spinner_create(ui_bt_select);
    lv_obj_set_size(spinner, 30, 30);
    lv_obj_align(spinner, LV_ALIGN_BOTTOM_MID, 0, -10);

    // --- Device list panel ---
    panel_device_list = lv_obj_create(ui_bt_select);
    lv_obj_set_size(panel_device_list, 280, 120);
    lv_obj_align(panel_device_list, LV_ALIGN_CENTER, 0, 5);
    lv_obj_set_flex_flow(panel_device_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel_device_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(panel_device_list, 4, 0);
    lv_obj_set_style_pad_all(panel_device_list, 4, 0);
    lv_obj_set_style_bg_opa(panel_device_list, LV_OPA_10, 0);
    lv_obj_set_style_bg_color(panel_device_list, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(panel_device_list, 0, 0);
    lv_obj_set_style_radius(panel_device_list, 8, 0);

    // --- Rescan button ---
    btn_rescan = lv_btn_create(ui_bt_select);
    lv_obj_set_size(btn_rescan, 120, 36);
    lv_obj_align(btn_rescan, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn_rescan, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_radius(btn_rescan, 18, 0);

    lv_obj_t *lbl_rescan = lv_label_create(btn_rescan);
    lv_label_set_text(lbl_rescan, LV_SYMBOL_REFRESH " Rescan");
    lv_obj_center(lbl_rescan);

    lv_obj_add_event_cb(btn_rescan, cb_rescan_clicked, LV_EVENT_CLICKED, NULL);
}

void ui_bt_select_screen_destroy(void) {
    if(ui_bt_select) lv_obj_del(ui_bt_select);

    lbl_title         = NULL;
    lbl_status        = NULL;
    panel_device_list = NULL;
    btn_rescan        = NULL;
    spinner           = NULL; 
}
