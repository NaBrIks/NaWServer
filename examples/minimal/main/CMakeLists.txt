idf_component_register(SRCS main.cpp self_test.cpp ../../../tests/url_parameters_test.cpp ../../../tests/response_test.cpp INCLUDE_DIRS . ../../../src
    PRIV_REQUIRES NaWServer esp_wifi esp_event esp_netif nvs_flash lwip freertos esp_timer esp_system esp_app_format)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17;-fstack-usage>)
set(HTTP_TEST_FRONTEND "${CMAKE_CURRENT_LIST_DIR}/../../../frontend/http-test")
set(HTTP_TEST_PROFILE "${CMAKE_CURRENT_LIST_DIR}/../frontend/http-test-config.json.in")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/HttpTestProfile.cmake")
nawserver_demo_write_http_profile("${IDF_TARGET}" "${HTTP_TEST_PROFILE}" "${CMAKE_CURRENT_BINARY_DIR}/http-test-config.json")
file(READ "${HTTP_TEST_FRONTEND}/index.html" HTTP_TEST_HTML)
file(READ "${HTTP_TEST_FRONTEND}/http-test.css" HTTP_TEST_CSS)
file(READ "${HTTP_TEST_FRONTEND}/http-test.js" HTTP_TEST_JAVASCRIPT)
file(READ "${HTTP_TEST_FRONTEND}/mcp-client.js" HTTP_TEST_MCP_CLIENT)
file(READ "${HTTP_TEST_FRONTEND}/mcp-panel.js" HTTP_TEST_MCP_PANEL)
file(READ "${CMAKE_CURRENT_BINARY_DIR}/http-test-config.json" HTTP_TEST_CONFIGURATION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${HTTP_TEST_FRONTEND}/index.html" "${HTTP_TEST_FRONTEND}/http-test.css"
    "${HTTP_TEST_FRONTEND}/http-test.js" "${HTTP_TEST_FRONTEND}/mcp-client.js"
    "${HTTP_TEST_FRONTEND}/mcp-panel.js")
configure_file(http_test_assets.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/http_test_assets.hpp" @ONLY)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
