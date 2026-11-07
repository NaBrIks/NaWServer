idf_component_register(SRCS "main.cpp" "../../decoder.cpp" "../../correctness.cpp"
    INCLUDE_DIRS "../.." REQUIRES esp_timer esp_system heap esp_app_format esp_hw_support)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -Os -fstack-usage)
