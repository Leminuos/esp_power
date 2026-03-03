#ifndef UI_BT_SELECT_H
#define UI_BT_SELECT_H

#ifdef __cplusplus
extern "C" {
#endif

// SCREEN: ui_bt_select
extern void ui_bt_select_screen_init(void);
extern void ui_bt_select_screen_destroy(void);
extern lv_obj_t * ui_bt_select;
extern lv_obj_t * lbl_title;
extern lv_obj_t * lbl_status;
extern lv_obj_t * panel_device_list;
extern lv_obj_t * btn_rescan;
extern lv_obj_t * spinner;
// CUSTOM VARIABLES
extern lv_obj_t * uic_bt_select;

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif