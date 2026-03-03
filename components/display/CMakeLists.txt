idf_component_register(
    SRCS
        "display.c"
        "ui/ui.c"
        "ui/ui_helpers.c"
        "ui/ui_events.c"
        "ui/screens/ui_explorer.c"
        "ui/screens/ui_audioplayer.c"
        "ui/screens/ui_bt_select.c"

    INCLUDE_DIRS "include" "ui" "ui/screens"
    
    REQUIRES
        "driver"
        "esp_lcd"
        "esp_timer"
        "bt_audio"
        "lvgl__lvgl"
        "espressif__esp_lcd_ili9341"
)
