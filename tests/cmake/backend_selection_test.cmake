cmake_minimum_required(VERSION 3.16)
get_filename_component(REPOSITORY_ROOT "${REPOSITORY_ROOT}" ABSOLUTE)
set(count 0)
foreach(with_websocket OFF ON)
    foreach(with_sse OFF ON)
        foreach(with_auth OFF ON)
            foreach(with_quotas OFF ON)
                foreach(with_managed OFF ON)
                    execute_process(COMMAND "${CMAKE_COMMAND}"
                        "-DREPOSITORY_ROOT=${REPOSITORY_ROOT}"
                        "-DWITH_WEBSOCKET=${with_websocket}" "-DWITH_SSE=${with_sse}"
                        "-DWITH_AUTH=${with_auth}" "-DWITH_QUOTAS=${with_quotas}"
                        "-DWITH_MANAGED=${with_managed}"
                        -P "${CMAKE_CURRENT_LIST_DIR}/backend_source_profile.cmake"
                        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
                    if(NOT result EQUAL 0 OR NOT output MATCHES "SOURCE_PROFILE_PASS")
                        message(FATAL_ERROR "Source profile failed: ${output}${error}")
                    endif()
                    math(EXPR count "${count}+1")
                endforeach()
            endforeach()
        endforeach()
    endforeach()
endforeach()
message(STATUS "COMMON_ENGINE_SOURCE_PROFILES_PASS count=${count}")