idf_component_register(SRCS "alarm_mgr.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES esp_event esp_peripherals json WiFi app_timeout esp_timer nfc-service)
