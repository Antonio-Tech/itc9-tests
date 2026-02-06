cmake_minimum_required(VERSION 3.5)

# Enable colored output in compiler messages
add_compile_options(-fdiagnostics-color=always)

# Add custom include path for main
add_compile_options(-I${CMAKE_SOURCE_DIR}/main/include)

# Append component directories:
#  - ADF (Audio Development Framework) components
#  - Your local project components
list(APPEND EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components
    $ENV{ADF_PATH}/components
)

# Define LVGL color swap macro
add_compile_definitions(LV_COLOR_16_SWAP=1)

# Include ESP-IDF project build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Define project name
project(cli_app)

# Optional debug info
message(STATUS "IDF_PATH: $ENV{IDF_PATH}")
message(STATUS "ADF_PATH: $ENV{ADF_PATH}")
message(STATUS "EXTRA_COMPONENT_DIRS: ${EXTRA_COMPONENT_DIRS}")
