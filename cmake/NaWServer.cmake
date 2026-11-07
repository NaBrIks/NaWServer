include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/NaWServerConfiguration.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/NaWServerSources.cmake")

# Ordinary CMake entry point, independent of ESP-IDF. SDK/network startup and
# application configuration remain owned by the executable that links this target.
function(nawserver_add_library target)
    get_property(NAWSERVER_SOURCE_ROOT GLOBAL PROPERTY NAWSERVER_SOURCE_ROOT)
    get_filename_component(compiler_name "${CMAKE_CXX_COMPILER}" NAME)
    if(compiler_name MATCHES "^zig(\\.exe)?$")
        set(CMAKE_CXX_ARCHIVE_CREATE "\"${CMAKE_CXX_COMPILER}\" ar qc <TARGET> <OBJECTS>" PARENT_SCOPE)
        set(CMAKE_CXX_ARCHIVE_FINISH "\"${CMAKE_CXX_COMPILER}\" ranlib <TARGET>" PARENT_SCOPE)
    endif()
    cmake_parse_arguments(NAWSERVER_BUILD "NO_WEBSOCKET;NO_SSE;NO_AUTH;NO_CLIENT_QUOTAS;NO_API_KEYS;MANAGED" "PLATFORM" "" ${ARGN})
    if(NAWSERVER_BUILD_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown NaWServer factory arguments: ${NAWSERVER_BUILD_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT NAWSERVER_BUILD_PLATFORM)
        if(WIN32)
            set(NAWSERVER_BUILD_PLATFORM windows)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(NAWSERVER_BUILD_PLATFORM linux)
        else()
            message(FATAL_ERROR "Select NaWServer PLATFORM windows, linux or pico explicitly")
        endif()
    endif()
    set(groups CORE HTTP_WIRE TCP_SESSION TCP_REGISTRY TCP_RUNTIME)
    if(NOT NAWSERVER_BUILD_NO_WEBSOCKET)
        list(APPEND groups WS WS_WIRE TCP_WS)
    endif()
    if(NOT NAWSERVER_BUILD_NO_SSE)
        list(APPEND groups SSE SSE_QUEUED TCP_SSE)
    endif()
    if(NAWSERVER_BUILD_MANAGED)
        list(APPEND groups TCP_MANAGED)
    endif()
    if(NOT NAWSERVER_BUILD_NO_AUTH)
        list(APPEND groups AUTH)
    endif()
    if(NOT NAWSERVER_BUILD_NO_API_KEYS)
        list(APPEND groups API_KEYS)
    endif()
    nawserver_collect_sources(sources ${groups})
    add_library(${target} STATIC ${sources})
    target_include_directories(${target} PUBLIC "${NAWSERVER_SOURCE_ROOT}/include"
        PRIVATE "${NAWSERVER_SOURCE_ROOT}/src")
    target_compile_features(${target} PUBLIC cxx_std_17)
    if(MSVC)
        # Consumers instantiate the same C++17 templates as the host demos.
        target_compile_options(${target} PUBLIC /permissive- /Zc:__cplusplus /utf-8)
    endif()
    target_compile_definitions(${target} PUBLIC
        NAWSERVER_API_KEYS=$<NOT:$<BOOL:${NAWSERVER_BUILD_NO_API_KEYS}>>
        NAWSERVER_AUTH=$<NOT:$<BOOL:${NAWSERVER_BUILD_NO_AUTH}>>
        NAWSERVER_CLIENT_QUOTAS=$<NOT:$<BOOL:${NAWSERVER_BUILD_NO_CLIENT_QUOTAS}>>
        NAWSERVER_TCP_WEBSOCKET=$<NOT:$<BOOL:${NAWSERVER_BUILD_NO_WEBSOCKET}>>
        NAWSERVER_TCP_SSE=$<NOT:$<BOOL:${NAWSERVER_BUILD_NO_SSE}>>
        NAWSERVER_TCP_MANAGED=$<BOOL:${NAWSERVER_BUILD_MANAGED}>)
    if(NAWSERVER_BUILD_PLATFORM STREQUAL "pico")
        target_sources(${target} PRIVATE
            "${NAWSERVER_SOURCE_ROOT}/src/backends/tcp/platform/lwip_tcp.cpp"
            "${NAWSERVER_SOURCE_ROOT}/src/backends/tcp/platform/pico_freertos_worker.cpp"
            "${NAWSERVER_SOURCE_ROOT}/src/backends/tcp/platform/freertos_mutex.cpp")
        target_compile_definitions(${target} PUBLIC NAWSERVER_FREERTOS_MUTEX=1)
        # The supplied bare Arm libstdc++ otherwise selects _S_single shared_ptr
        # counts. This explicit tested-toolchain bridge is separate from Mutex.
        target_include_directories(${target} PUBLIC
            "${NAWSERVER_SOURCE_ROOT}/include/NaWServer/detail/libstdcxx_freertos")
        target_compile_definitions(${target} PUBLIC _GLIBCXX_HAS_GTHREADS=1)
        target_link_libraries(${target} PUBLIC pico_stdlib pico_cyw43_arch_lwip_sys_freertos
            FreeRTOS-Kernel-Heap4)
    elseif(NAWSERVER_BUILD_PLATFORM STREQUAL "windows" OR NAWSERVER_BUILD_PLATFORM STREQUAL "linux")
        if(NOT NAWSERVER_BUILD_NO_API_KEYS)
            target_sources(${target} PRIVATE "${NAWSERVER_SOURCE_ROOT}/src/api_keys/file.cpp")
            if(NAWSERVER_BUILD_PLATFORM STREQUAL "windows")
                target_link_libraries(${target} PUBLIC bcrypt advapi32)
            endif()
        endif()
        find_package(Threads REQUIRED)
        target_sources(${target} PRIVATE
            "${NAWSERVER_SOURCE_ROOT}/src/backends/tcp/platform/${NAWSERVER_BUILD_PLATFORM}_tcp.cpp"
            "${NAWSERVER_SOURCE_ROOT}/src/backends/tcp/platform/${NAWSERVER_BUILD_PLATFORM}_worker.cpp")
        target_link_libraries(${target} PUBLIC Threads::Threads)
        if(NAWSERVER_BUILD_PLATFORM STREQUAL "windows")
            target_compile_definitions(${target} PUBLIC NOMINMAX)
            target_link_libraries(${target} PUBLIC ws2_32)
        endif()
    else()
        message(FATAL_ERROR "Unsupported NaWServer platform: ${NAWSERVER_BUILD_PLATFORM}")
    endif()
endfunction()
