cmake_minimum_required(VERSION 3.20)
set(PICO_BOARD pico2_w CACHE STRING "Pico 2 W")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "RP2350 ARM Cortex-M33")
include(pico_sdk_import.cmake)
project(NaWServer_pico2_w VERSION 0.1.0 LANGUAGES C CXX ASM)
option(NAWSERVER_DEMO_RP_BOUNDED_WIFI_WAIT "Experimental bare-metal response-wait workaround" OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(FREERTOS_KERNEL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/.deps/FreeRTOS-Kernel" CACHE PATH "Pinned kernel")
set(FREERTOS_CONFIG_FILE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE PATH "Demo RTOS configuration")
include("${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake")
pico_sdk_init()
get_filename_component(NAWSERVER_ROOT "../.." ABSOLUTE)

include("${NAWSERVER_ROOT}/cmake/NaWServer.cmake")
nawserver_add_library(nawserver_rp_demo_core PLATFORM pico MANAGED)
target_include_directories(nawserver_rp_demo_core PUBLIC src)
target_compile_definitions(nawserver_rp_demo_core PUBLIC
    NAWSERVER_LWIP_SOCKET_COUNT=16
    PICO_FLASH_SAFE_EXECUTE_SUPPORT_FREERTOS_SMP=1 PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000)
target_compile_options(nawserver_rp_demo_core PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-Wall;-Wextra>)
target_link_libraries(nawserver_rp_demo_core PUBLIC
    pico_stdlib pico_rand pico_unique_id pico_cyw43_arch_lwip_sys_freertos
    FreeRTOS-Kernel-Heap4 pico_flash hardware_flash hardware_watchdog)

add_library(rp_demo_littlefs STATIC .deps/littlefs/lfs.c .deps/littlefs/lfs_util.c)
target_include_directories(rp_demo_littlefs PUBLIC .deps/littlefs)
target_compile_definitions(rp_demo_littlefs PRIVATE LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)
add_executable(nawserver_pico2_w
    src/main.cpp src/storage.cpp src/web_demo.cpp src/rtos_hooks.cpp src/showcase_api.cpp src/flash_guard.cpp)
target_include_directories(nawserver_pico2_w PRIVATE src .private build/generated)
target_link_libraries(nawserver_pico2_w PRIVATE nawserver_rp_demo_core rp_demo_littlefs)
target_compile_options(nawserver_pico2_w PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-Wall;-Wextra>)
# Keep the entire linked firmware below the first storage partition, including
# the Wi-Fi firmware and seeded web assets. The Pico SDK linker script is reused.
target_link_options(nawserver_pico2_w PRIVATE
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/src/flash_limits.ld")
if(NAWSERVER_DEMO_RP_BOUNDED_WIFI_WAIT)
    target_sources(nawserver_pico2_w PRIVATE src/cyw43_wait.c)
    target_link_options(nawserver_pico2_w PRIVATE "-Wl,--wrap=cyw43_await_background_or_timeout_us")
endif()
pico_enable_stdio_usb(nawserver_pico2_w 1)
pico_enable_stdio_uart(nawserver_pico2_w 0)
pico_set_program_name(nawserver_pico2_w "NaWServer RP2350 FreeRTOS demo")
pico_set_program_version(nawserver_pico2_w "0.1.0")
pico_add_extra_outputs(nawserver_pico2_w)
find_program(NODE_EXECUTABLE node REQUIRED)
add_custom_command(TARGET nawserver_pico2_w POST_BUILD
    COMMAND ${NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check-layout.mjs"
        "$<TARGET_FILE:nawserver_pico2_w>.bin" VERBATIM)
