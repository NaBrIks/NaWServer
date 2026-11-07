include_guard(GLOBAL)

# Old cache entries must fail clearly, including OFF values which would otherwise
# hide a misspelled feature setting. A clean build uses only canonical names.
get_cmake_property(NAWSERVER_CONFIGURATION_CACHE_VARIABLES CACHE_VARIABLES)
foreach(NAWSERVER_CONFIGURATION_VARIABLE IN LISTS NAWSERVER_CONFIGURATION_CACHE_VARIABLES)
    get_property(NAWSERVER_CONFIGURATION_TYPE CACHE "${NAWSERVER_CONFIGURATION_VARIABLE}" PROPERTY TYPE)
    # CMake creates NaWServer_BINARY_DIR/SOURCE_DIR/IS_TOP_LEVEL for project().
    # These generated entries are not user-owned configuration switches.
    if(NAWSERVER_CONFIGURATION_TYPE STREQUAL "STATIC" OR NAWSERVER_CONFIGURATION_TYPE STREQUAL "INTERNAL")
        continue()
    endif()
    if(NAWSERVER_CONFIGURATION_VARIABLE MATCHES "^NaWServer_[A-Z][A-Z0-9_]*$" OR
       NAWSERVER_CONFIGURATION_VARIABLE STREQUAL "RP_DEMO_BOUNDED_WIFI_WAIT" OR
       NAWSERVER_CONFIGURATION_VARIABLE MATCHES "^NAWSERVER_(HOST_)?TCP_ADMISSION($|_)" OR
       NAWSERVER_CONFIGURATION_VARIABLE MATCHES "^(CONFIG_)?NAWSERVER_BACKEND_(HTTPD|TCP)$")
        message(FATAL_ERROR
            "Obsolete setting ${NAWSERVER_CONFIGURATION_VARIABLE}: use the uppercase NAWSERVER_ prefix. Reconfigure a clean cache with the renamed settings.")
    endif()
endforeach()
unset(NAWSERVER_CONFIGURATION_CACHE_VARIABLES)
unset(NAWSERVER_CONFIGURATION_VARIABLE)
unset(NAWSERVER_CONFIGURATION_TYPE)
