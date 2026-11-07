idf_component_register(SRCS device_details.cpp main.cpp board.cpp graphics.cpp application.cpp web.cpp transport.cpp wire.cpp telemetry.cpp runtime.cpp network.cpp
    INCLUDE_DIRS . "${CMAKE_CURRENT_BINARY_DIR}"
    LDFRAGMENTS ../../common/memory_usage.lf
    PRIV_REQUIRES NaWServer spi_flash app_update esp_hw_support esp_app_format esp_lcd driver esp_wifi esp_event esp_netif nvs_flash esp_timer
        espressif__cbor espressif__led_strip espressif__esp_lcd_touch espressif__esp_lcd_touch_gt911 lvgl__lvgl)
