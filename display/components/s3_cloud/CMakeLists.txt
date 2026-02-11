idf_component_register(SRCS "s3_https_cloud.c" "s3_sync_account_contents.c" "s3_tracking.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_peripherals esp_http_client esp-tls nvs_flash json storage manual_ota alarm_mgr
        )
