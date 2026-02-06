idf_component_register(SRCS "app_timeout.c"
                      INCLUDE_DIRS "include"
                      PRIV_REQUIRES esp_timer power_management WiFi)
