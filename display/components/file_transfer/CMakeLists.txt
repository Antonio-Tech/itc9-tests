idf_component_register(SRCS "file_transfer.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_server json vfs WiFi fatfs main
                    PRIV_REQUIRES esp_peripherals)
