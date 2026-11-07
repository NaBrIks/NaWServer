idf_component_register(SRCS "main.cpp" "../../checks.cpp"
    INCLUDE_DIRS "../.." "../../../../include"
    PRIV_REQUIRES NaWServer esp_netif esp_event esp_timer esp_system lwip freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -Os -Wall -Wextra -Werror -fstack-usage)