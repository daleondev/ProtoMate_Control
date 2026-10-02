if(NOT DEFINED PROBE)
    message(FATAL_ERROR "PROBE is required")
endif()

execute_process(
    COMMAND "${PROBE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 5
)

if(NOT result EQUAL 42)
    message(FATAL_ERROR
        "Panic override probe returned ${result}, expected 42\nstdout:\n${output}\nstderr:\n${error}"
    )
endif()
