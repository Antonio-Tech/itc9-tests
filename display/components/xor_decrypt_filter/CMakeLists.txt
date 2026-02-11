idf_component_register(SRCS "xor_decrypt_filter.c"
                    INCLUDE_DIRS "include"
                    REQUIRES audio_pipeline audio_stream)
set(KCONFIG_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Kconfig")