idf_component_register(SRCS "sntp_syncer.c" "WiFi.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_wifi
                             esp_http_server
                             json
                             storage
                             lwip
                             esp_netif
                             esp_coex
                             lvgl
                             s3_cloud
                             s3_bluetooth
                             manual_ota
                             app_timeout
                             alarm_mgr
                             )
