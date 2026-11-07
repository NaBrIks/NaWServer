execute_process(COMMAND "${GO_EXECUTABLE}" run . -count 4096 -seed 1683304820
    WORKING_DIRECTORY "${ORACLE_SOURCE}" OUTPUT_FILE "${ORACLE_OUTPUT}"
    RESULT_VARIABLE oracle_result)
if(NOT oracle_result EQUAL 0)
    message(FATAL_ERROR "Independent Go handshake oracle failed: ${oracle_result}")
endif()
execute_process(COMMAND ${HANDSHAKE_EMULATOR} "${HANDSHAKE_EXECUTABLE}" --oracle INPUT_FILE "${ORACLE_OUTPUT}"
    RESULT_VARIABLE handshake_result)
if(NOT handshake_result EQUAL 0)
    message(FATAL_ERROR "Handshake comparison failed: ${handshake_result}")
endif()
