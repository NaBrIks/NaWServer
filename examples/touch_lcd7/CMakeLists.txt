cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.1.0-touch-lcd7")
project(NaWServer_touch_lcd7)

# Windows Xtensa GCC can crash in RGB register allocation at -Og (IDFGH-10991).
# Keep the pinned SDK and scope the size-optimization workaround to esp_lcd.
# https://github.com/espressif/esp-idf/issues/12180
if(CMAKE_HOST_WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_C_COMPILER_VERSION VERSION_EQUAL "14.2.0")
    target_compile_options(__idf_esp_lcd PRIVATE -Os)
endif()
