idf_component_register(SRCS "manual_ota.c"
                    INCLUDE_DIRS "include"
                    REQUIRES ota_service
                             esp_peripherals
                             )
