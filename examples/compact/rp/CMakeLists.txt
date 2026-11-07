cmake_minimum_required(VERSION 3.20)
set(PICO_BOARD pico2_w CACHE STRING "Pico 2 W")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "RP2350 secure Arm")
if(NOT PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif()
if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH to an installed Pico SDK (tested: 2.3.1)")
endif()
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
project(NaWServerCompactRP VERSION 1.0.0 LANGUAGES C CXX ASM)
set(NAWSERVER_DEMO_RP_RTOS_HEAP_BYTES 163840 CACHE STRING "FreeRTOS heap capacity")
set(NAWSERVER_DEMO_RP_LWIP_HEAP_BYTES 32768 CACHE STRING "lwIP heap capacity")
set(NAWSERVER_DEMO_RP_PBUF_POOL_SIZE 24 CACHE STRING "lwIP packet pool entries")
set(NAWSERVER_DEMO_RP_TCP_SEGMENTS 48 CACHE STRING "lwIP TCP segments")
set(NAWSERVER_DEMO_RP_TCP_PCB_COUNT 12 CACHE STRING "lwIP TCP control blocks")
set(NAWSERVER_DEMO_RP_NETCONN_COUNT 16 CACHE STRING "lwIP netconn objects")
set(NAWSERVER_DEMO_RP_PROBE_CYCLES 1 CACHE STRING "Opt-in diagnostic stop/restart probes")
set(NAWSERVER_DEMO_TX_MEMORY_MODE 0 CACHE STRING "TX payload: 0 lazy heap, 1 reserved heap, 2 static")
set(NAWSERVER_DEMO_TX_METADATA_MODE 0 CACHE STRING "TX metadata: 0 heap, 1 static arena")
set(NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES 2048 CACHE STRING "Static TX metadata arena bytes")
set(NAWSERVER_DEMO_TX_BLOCKS 4 CACHE STRING "Maximum shared transmission blocks")
set(NAWSERVER_DEMO_TX_BLOCK_BYTES 1024 CACHE STRING "Bytes per transmission block")
set(NAWSERVER_DEMO_HTTP_JOBS 2 CACHE STRING "Maximum concurrent HTTP jobs")
set(NAWSERVER_DEMO_CONNECTIONS 4 CACHE STRING "Maximum accepted connections")
set(NAWSERVER_PICO_HTTP_STACK_WORDS 3072 CACHE STRING "HTTP worker stack in FreeRTOS words")
set(NAWSERVER_PICO_TX_STACK_WORDS 1024 CACHE STRING "TX worker stack in FreeRTOS words")
set(NAWSERVER_PICO_STREAM_STACK_WORDS 1536 CACHE STRING "SSE/WebSocket worker stack in FreeRTOS words")
if(NOT "${NAWSERVER_DEMO_TX_MEMORY_MODE}" MATCHES "^[012]$")
    message(FATAL_ERROR "NAWSERVER_DEMO_TX_MEMORY_MODE must be 0, 1, or 2")
endif()
if(NOT "${NAWSERVER_DEMO_TX_METADATA_MODE}" MATCHES "^[01]$" OR
    NOT "${NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES}" MATCHES "^[1-9][0-9]*$" OR
    NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES LESS 256 OR NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES GREATER 65536)
    message(FATAL_ERROR "RP metadata mode must be 0 or 1, with a 256..65536 byte arena")
endif()
if(NOT "${NAWSERVER_DEMO_RP_PROBE_CYCLES}" MATCHES "^[1-5]$")
    message(FATAL_ERROR "NAWSERVER_DEMO_RP_PROBE_CYCLES must be 1..5")
endif()
if(NOT "${NAWSERVER_DEMO_HTTP_JOBS}" MATCHES "^[1-9][0-9]*$" OR
    NAWSERVER_DEMO_HTTP_JOBS LESS 2 OR NAWSERVER_DEMO_HTTP_JOBS GREATER 8 OR
    NOT "${NAWSERVER_DEMO_CONNECTIONS}" MATCHES "^[1-9][0-9]*$" OR
    NAWSERVER_DEMO_CONNECTIONS GREATER 12)
    message(FATAL_ERROR "HTTP jobs must be 2..8 and connections must be a positive integer <=12")
endif()
math(EXPR NAWSERVER_DEMO_MIN_CONNECTIONS "${NAWSERVER_DEMO_HTTP_JOBS} + 2")
if(NAWSERVER_DEMO_CONNECTIONS LESS NAWSERVER_DEMO_MIN_CONNECTIONS OR
    NAWSERVER_DEMO_CONNECTIONS GREATER NAWSERVER_DEMO_RP_NETCONN_COUNT)
    message(FATAL_ERROR "Connections must cover HTTP jobs plus two streams and fit in the lwIP netconn count")
endif()
foreach(setting IN ITEMS NAWSERVER_PICO_HTTP_STACK_WORDS NAWSERVER_PICO_TX_STACK_WORDS NAWSERVER_PICO_STREAM_STACK_WORDS)
    if(NOT "${${setting}}" MATCHES "^[1-9][0-9]*$" OR ${setting} LESS 256 OR ${setting} GREATER 16384)
        message(FATAL_ERROR "${setting} must be 256..16384 FreeRTOS stack words")
    endif()
endforeach()
if(NOT "${NAWSERVER_DEMO_TX_BLOCKS}" MATCHES "^[1-9][0-9]*$" OR
    NAWSERVER_DEMO_TX_BLOCKS LESS 2 OR NAWSERVER_DEMO_TX_BLOCKS GREATER 12)
    message(FATAL_ERROR "NAWSERVER_DEMO_TX_BLOCKS must be in the range 2..12")
endif()
if(NOT "${NAWSERVER_DEMO_TX_BLOCK_BYTES}" MATCHES "^[1-9][0-9]*$" OR
    NAWSERVER_DEMO_TX_BLOCK_BYTES LESS 768 OR NAWSERVER_DEMO_TX_BLOCK_BYTES GREATER 4096)
    message(FATAL_ERROR "NAWSERVER_DEMO_TX_BLOCK_BYTES must be 768..4096")
endif()
math(EXPR NAWSERVER_DEMO_TX_BLOCK_REMAINDER "${NAWSERVER_DEMO_TX_BLOCK_BYTES} % 16")
if(NOT NAWSERVER_DEMO_TX_BLOCK_REMAINDER EQUAL 0)
    message(FATAL_ERROR "NAWSERVER_DEMO_TX_BLOCK_BYTES must be a multiple of 16")
endif()
foreach(setting IN ITEMS NAWSERVER_DEMO_RP_RTOS_HEAP_BYTES NAWSERVER_DEMO_RP_LWIP_HEAP_BYTES
    NAWSERVER_DEMO_RP_PBUF_POOL_SIZE NAWSERVER_DEMO_RP_TCP_SEGMENTS
    NAWSERVER_DEMO_RP_TCP_PCB_COUNT NAWSERVER_DEMO_RP_NETCONN_COUNT
    NAWSERVER_PICO_HTTP_STACK_WORDS NAWSERVER_PICO_TX_STACK_WORDS NAWSERVER_PICO_STREAM_STACK_WORDS)
    if(NOT "${${setting}}" MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "${setting} must be a positive integer")
    endif()
    # These macros reach the SDK and FreeRTOS targets as well as the application.
    add_compile_definitions(${setting}=${${setting}})
endforeach()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(NAWSERVER_ROOT "../../.." ABSOLUTE)
set(FREERTOS_KERNEL_PATH "${NAWSERVER_ROOT}/examples/rp2350_pico2_w/.deps/FreeRTOS-Kernel" CACHE PATH "Existing native FreeRTOS kernel")
set(FREERTOS_CONFIG_FILE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(NAWSERVER_DEMO_LITTLEFS_PATH "${NAWSERVER_ROOT}/examples/rp2350_pico2_w/.deps/littlefs" CACHE PATH "Existing LittleFS source")
set(NAWSERVER_DEMO_WIFI_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.private" CACHE PATH "Private wifi_config.h folder")
set(NAWSERVER_DEMO_UPLOAD_TIMEOUT_MS 600000 CACHE STRING "Total upload deadline; inactivity remains separately bounded")
include("${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake")
pico_sdk_init()
include("${NAWSERVER_ROOT}/cmake/NaWServer.cmake")
include("${NAWSERVER_ROOT}/cmake/NaWServerJson.cmake")
option(NAWSERVER_DEMO_STATUS_WS "Use WebSocket status instead of SSE" OFF)
option(NAWSERVER_DEMO_RP_TASK_DIAGNOSTICS "USB task stack watermarks (uses additional static RAM)" OFF)
option(NAWSERVER_DEMO_RP_STARTUP_MEASURE "Measure startup allocations after Wi-Fi connects" OFF)
option(NAWSERVER_DEMO_RP_NO_WEB_BASELINE "Build-only baseline without server" OFF)
if(NAWSERVER_DEMO_STATUS_WS)
    set(NAWSERVER_DEMO_STREAM_EXCLUDE NO_SSE)
else()
    set(NAWSERVER_DEMO_STREAM_EXCLUDE NO_WEBSOCKET)
endif()
nawserver_add_library(nawserver_compact_core PLATFORM pico MANAGED NO_AUTH NO_CLIENT_QUOTAS ${NAWSERVER_DEMO_STREAM_EXCLUDE})
nawserver_attach_json(nawserver_compact_core bounded)
target_include_directories(nawserver_compact_core PUBLIC src)
target_compile_definitions(nawserver_compact_core PUBLIC NAWSERVER_API_KEYS_MAX=1
    NAWSERVER_DEMO_STATUS_WS=$<BOOL:${NAWSERVER_DEMO_STATUS_WS}>
    NAWSERVER_DEMO_HTTP_JOBS=${NAWSERVER_DEMO_HTTP_JOBS}
    NAWSERVER_DEMO_CONNECTIONS=${NAWSERVER_DEMO_CONNECTIONS}
    NAWSERVER_LWIP_SOCKET_COUNT=${NAWSERVER_DEMO_RP_NETCONN_COUNT}
    PICO_FLASH_ASSUME_CORE1_SAFE=1 PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0)
target_link_libraries(nawserver_compact_core PUBLIC pico_stdlib pico_rand pico_flash pico_sha256
    pico_cyw43_arch_lwip_sys_freertos FreeRTOS-Kernel-Heap4 hardware_watchdog boot_uf2_headers)
add_library(nawserver_compact_lfs STATIC "${NAWSERVER_DEMO_LITTLEFS_PATH}/lfs.c" "${NAWSERVER_DEMO_LITTLEFS_PATH}/lfs_util.c")
target_include_directories(nawserver_compact_lfs PUBLIC "${NAWSERVER_DEMO_LITTLEFS_PATH}")
target_compile_definitions(nawserver_compact_lfs PRIVATE LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)
find_program(NAWSERVER_DEMO_NODE node REQUIRED)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/compact_web.hpp"
    COMMAND "${NAWSERVER_DEMO_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/../web/pack.mjs" "${CMAKE_CURRENT_BINARY_DIR}/generated/compact_web.hpp"
    DEPENDS ../web/pack.mjs ../web/index.html ../web/sha256.mjs VERBATIM)
add_executable(nawserver_compact_rp src/main.cpp src/flash.cpp src/ota.cpp src/rtos_hooks.cpp
    ../platform/rp.cpp ../common/demo.cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/compact_web.hpp")
target_include_directories(nawserver_compact_rp PRIVATE src "${NAWSERVER_DEMO_WIFI_CONFIG_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(nawserver_compact_rp PRIVATE nawserver_compact_core nawserver_compact_lfs)
target_compile_definitions(nawserver_compact_rp PRIVATE PICO_CRT0_IMAGE_TYPE_TBYB=1
    NAWSERVER_DEMO_RP_PROBE_CYCLES=${NAWSERVER_DEMO_RP_PROBE_CYCLES}
    NAWSERVER_DEMO_TX_MEMORY_MODE=${NAWSERVER_DEMO_TX_MEMORY_MODE}
    NAWSERVER_DEMO_TX_METADATA_MODE=${NAWSERVER_DEMO_TX_METADATA_MODE}
    NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES=${NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES}
    NAWSERVER_DEMO_TX_BLOCKS=${NAWSERVER_DEMO_TX_BLOCKS}
    NAWSERVER_DEMO_TX_BLOCK_BYTES=${NAWSERVER_DEMO_TX_BLOCK_BYTES}
    NAWSERVER_DEMO_RP_STARTUP_MEASURE=$<BOOL:${NAWSERVER_DEMO_RP_STARTUP_MEASURE}>
    NAWSERVER_DEMO_RP_NO_WEB_BASELINE=$<BOOL:${NAWSERVER_DEMO_RP_NO_WEB_BASELINE}>
    NAWSERVER_DEMO_RP_TASK_DIAGNOSTICS=$<BOOL:${NAWSERVER_DEMO_RP_TASK_DIAGNOSTICS}>
    NAWSERVER_DEMO_UPLOAD_TIMEOUT_MS=${NAWSERVER_DEMO_UPLOAD_TIMEOUT_MS})
target_compile_options(nawserver_compact_rp PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-Wall;-Wextra>)
target_link_options(nawserver_compact_rp PRIVATE "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/src/flash_limits.ld")
pico_enable_stdio_usb(nawserver_compact_rp 1)
pico_enable_stdio_uart(nawserver_compact_rp 0)
pico_set_program_name(nawserver_compact_rp "NaWServer Compact Pico 2 W")
pico_set_program_version(nawserver_compact_rp "1.0.0")
pico_hash_binary(nawserver_compact_rp)
pico_add_extra_outputs(nawserver_compact_rp)
if(NOT NAWSERVER_DEMO_RP_NO_WEB_BASELINE)
add_custom_command(TARGET nawserver_compact_rp POST_BUILD
    COMMAND picotool uf2 convert "$<TARGET_FILE:nawserver_compact_rp>" "${CMAKE_CURRENT_BINARY_DIR}/nawserver_compact_rp.ota.uf2" --family rp2350-arm-s
    COMMAND "${NAWSERVER_DEMO_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/check-image.mjs" "${CMAKE_CURRENT_BINARY_DIR}/nawserver_compact_rp.ota.uf2"
    VERBATIM)
endif()
