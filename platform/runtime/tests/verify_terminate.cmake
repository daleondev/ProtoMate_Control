if(NOT DEFINED PROBE)
    message(FATAL_ERROR "PROBE is required")
endif()

set(probe_command "${PROBE}")
if(DEFINED PROBE_MODE)
    list(APPEND probe_command "${PROBE_MODE}")
endif()

execute_process(
    COMMAND ${probe_command}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 5
)

if(result EQUAL 0)
    message(FATAL_ERROR "Terminate probe returned normally")
endif()

if(result MATCHES "[Tt]imeout")
    message(FATAL_ERROR "Terminate probe timed out")
endif()

if(NOT DEFINED EXPECTED_PANIC)
    set(EXPECTED_PANIC "Unhandled C++ exception")
endif()

string(FIND "${error}" "[hal][panic] ${EXPECTED_PANIC}" panic_position)
if(panic_position EQUAL -1)
    message(FATAL_ERROR
        "Terminate probe did not reach the panic handler\nstdout:\n${output}\nstderr:\n${error}"
    )
endif()
