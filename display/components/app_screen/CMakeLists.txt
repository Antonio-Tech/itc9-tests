idf_component_register(
    SRCS
        "app_screen.c"
        # "${CMAKE_SOURCE_DIR}/main/lv_screen_mgr.c"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/main/include/"
    REQUIRES
        lvgl
        # esp_peripherals
)