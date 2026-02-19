set(srcs
        "app_state_machine.c"
	    "audio_player.c"
        "fac_bt.c"
        "fac_wifi.c"
        "lv_decoders.c"
        "lv_screen_mgr.c"
        "main.c"
        # "manual_ota.c"
        "s3_definitions.c"

        "s3_album_mgr.c"
        "s3_logger.c"
        "s3_nfc_handler.c"
        "voltage_kalman.c"
        # "ulp_adc.c"
        # "../overrides/lvgl__lvgl/src/extra/libs/gif/gifdec.c"
        # "../overrides/lvgl__lvgl/src/extra/libs/gif/lv_gif.c"
        # Cherry Bomb custom fonts
        "fonts/cherry_bomb_16.c"
        "fonts/cherry_bomb_24.c"
        "fonts/cherry_bomb_48.c"
        "fonts/cherry_bomb_72.c"
        "fonts/cherry_bomb_90.c"
)

idf_component_register(
    SRCS
        ${srcs}
    INCLUDE_DIRS
        "."
        include
        # ../overrides
    REQUIRES
        app_event_handler
        app_screen
        audio_pipeline
        audio_sal
        audio_stream
        battery_service
        bluetooth_service  # ESP-ADF A2DP stream support only
        bt
        ulp
        esp_adc
        esp_event
        esp_hw_support
        esp_mm
        esp_netif
        esp_peripherals
        esp_wifi
        fatfs
        input_key_service
        lcd_touch
        lvgl
        lvgl_gui
        nfc-service
        manual_ota
        playlist
        WiFi
        xor_decrypt_filter
        s3_cloud
        alarm_mgr
        cjson_psram_hooks
        power_management
        clock
        s3_bluetooth
        file_transfer
)

#spiffs_create_partition_image(
#    storage
#    ${PROJECT_DIR}/spiffs
#    FLASH_IN_PROJECT
#)

# Needs investigation, for me without -w code breaks
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
target_compile_definitions(${COMPONENT_LIB} PRIVATE NO_LOTTIE)

