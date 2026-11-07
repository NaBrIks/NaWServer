idf_component_register(SRCS main.cpp "../../checks.cpp"
    INCLUDE_DIRS "../.." "../../../../include" "../../../../examples/minimal/main"
    PRIV_REQUIRES NaWServer esp_wifi esp_event esp_netif nvs_flash esp_timer lwip freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -Os -Wall -Wextra -Werror -fstack-usage)
