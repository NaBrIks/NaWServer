idf_component_register(SRCS device_details.cpp main.cpp wire.cpp runtime.cpp telemetry.cpp INCLUDE_DIRS .
    LDFRAGMENTS ../../common/memory_usage.lf
    PRIV_REQUIRES NaWServer lwip freertos spi_flash app_update esp_hw_support esp_wifi esp_event esp_netif nvs_flash esp_timer esp_system esp_app_format
        esp_driver_gpio esp_driver_usb_serial_jtag espressif__cbor espressif__led_strip)
if(IDF_TARGET STREQUAL "esp32c3")
    set(NAWSERVER_DEMO_SHARED_CLIENTS 1 CACHE STRING "Concurrent demo pages; measured C3 RAM profile")
else()
    set(NAWSERVER_DEMO_SHARED_CLIENTS 2 CACHE STRING "Concurrent demo pages")
endif()
target_compile_definitions(${COMPONENT_LIB} PUBLIC NAWSERVER_DEMO_SHARED_CLIENTS=${NAWSERVER_DEMO_SHARED_CLIENTS})
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17;-fstack-usage>)

if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    # Pinned led_strip 3.0.1 uses heap capabilities in its SPI source without a
    # direct include. IDF 6 SPI headers no longer provide that declaration.
    idf_component_get_property(led_strip_component espressif__led_strip COMPONENT_LIB)
    target_compile_options(${led_strip_component} PRIVATE -include esp_heap_caps.h)
endif()
