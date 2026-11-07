idf_component_register(SRCS main.cpp wire.cpp runtime.cpp telemetry.cpp lcd.cpp device_details.cpp INCLUDE_DIRS .
    LDFRAGMENTS ../../common/memory_usage.lf
    PRIV_REQUIRES NaWServer esp_wifi esp_event esp_netif nvs_flash esp_timer esp_system esp_app_format
        esp_driver_gpio esp_driver_spi esp_driver_ledc spi_flash app_update esp_hw_support espressif__cbor espressif__led_strip)
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
