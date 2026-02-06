idf_component_register(
    SRCS "s3_bluetooth.c"
         "s3_ble_manager.c"
         "s3_bt_classic.c"
         "s3_bt_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES "bt" "storage" "json" "app_timeout" "WiFi" "lvgl" "esp_event" "freertos" "esp_timer" "nvs_flash"
)

# Disable some warnings for compatibility
target_compile_options(__idf_s3_bluetooth PRIVATE "-Wno-error")