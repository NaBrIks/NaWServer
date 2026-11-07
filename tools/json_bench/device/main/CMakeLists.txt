get_filename_component(nawserver_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
idf_component_register(SRCS main.cpp ../../bench.cpp INCLUDE_DIRS "${nawserver_root}/include"
    PRIV_REQUIRES esp_timer NaWServer)
if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()
if(NAWSERVER_BENCH_EXISTING_YYJSON AND NAWSERVER_BENCH_EXISTING_RAPIDJSON)
    message(FATAL_ERROR "Select at most one existing application JSON consumer")
endif()
if(NAWSERVER_BENCH_EXISTING_YYJSON)
    # Reuse the library target already selected by the yyjson adapter. Bounded
    # builds add exactly the same upstream implementation for the app consumer.
    if(NOT TARGET nawserver_yyjson)
        add_library(nawserver_yyjson STATIC "${nawserver_root}/third_party/yyjson/src/yyjson.c")
        target_include_directories(nawserver_yyjson PUBLIC "${nawserver_root}/third_party/yyjson/src")
        target_compile_definitions(nawserver_yyjson PRIVATE YYJSON_DISABLE_FILE=1 YYJSON_DISABLE_UTILS=1
            YYJSON_DISABLE_NON_STANDARD=1 YYJSON_DISABLE_INCR_READER=1)
    endif()
    target_link_libraries(${COMPONENT_LIB} PRIVATE nawserver_yyjson)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NAWSERVER_BENCH_EXISTING_YYJSON=1)
elseif(NAWSERVER_BENCH_EXISTING_RAPIDJSON)
    target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE "${nawserver_root}/third_party/rapidjson/include")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NAWSERVER_BENCH_EXISTING_RAPIDJSON=1)
endif()
