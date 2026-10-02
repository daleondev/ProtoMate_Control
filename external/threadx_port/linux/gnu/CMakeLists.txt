
find_program(THREADX_PATCH_EXECUTABLE NAMES patch REQUIRED)

set(THREADX_LINUX_GNU_DIR "${CMAKE_SOURCE_DIR}/external/threadx/ports/linux/gnu")
set(THREADX_LINUX_GNU_PORT_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}/patches")
set(THREADX_LINUX_GNU_PORT_SRC_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_src")

function(threadx_linux_generate_patched_source output_var source_name)
    set(upstream_source "${THREADX_LINUX_GNU_DIR}/src/${source_name}")
    set(patch_file "${THREADX_LINUX_GNU_PORT_PATCH_DIR}/${source_name}.patch")
    set(output_file "${THREADX_LINUX_GNU_PORT_SRC_DIR}/${source_name}")

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${upstream_source}"
        "${patch_file}"
    )

    file(MAKE_DIRECTORY "${THREADX_LINUX_GNU_PORT_SRC_DIR}")
    file(REMOVE "${output_file}")

    execute_process(
        COMMAND
            "${THREADX_PATCH_EXECUTABLE}"
            --silent
            --unified
            --output
            "${output_file}"
            "${upstream_source}"
            "${patch_file}"
        RESULT_VARIABLE patch_result
        OUTPUT_VARIABLE patch_stdout
        ERROR_VARIABLE patch_stderr
    )

    if(NOT patch_result EQUAL 0)
        file(REMOVE "${output_file}")
        message(FATAL_ERROR
            "Failed to apply patch '${patch_file}' to '${upstream_source}'.\n"
            "stdout:\n${patch_stdout}\n"
            "stderr:\n${patch_stderr}"
        )
    endif()

    set(${output_var} "${output_file}" PARENT_SCOPE)
endfunction()

threadx_linux_generate_patched_source(TX_LINUX_INITIALIZE_LOW_LEVEL_SOURCE "tx_initialize_low_level.c")
threadx_linux_generate_patched_source(TX_LINUX_THREAD_STACK_BUILD_SOURCE "tx_thread_stack_build.c")
threadx_linux_generate_patched_source(TX_LINUX_THREAD_SYSTEM_RETURN_SOURCE "tx_thread_system_return.c")
threadx_linux_generate_patched_source(TX_LINUX_THREAD_SCHEDULE_SOURCE "tx_thread_schedule.c")

target_sources(${PROJECT_NAME}
    PRIVATE
    # {{BEGIN_TARGET_SOURCES}}
    ${THREADX_LINUX_GNU_DIR}/src/tx_thread_context_restore.c
    ${THREADX_LINUX_GNU_DIR}/src/tx_thread_context_save.c
    ${THREADX_LINUX_GNU_DIR}/src/tx_thread_interrupt_control.c
    ${TX_LINUX_THREAD_SCHEDULE_SOURCE}
    ${THREADX_LINUX_GNU_DIR}/src/tx_timer_interrupt.c
    ${TX_LINUX_INITIALIZE_LOW_LEVEL_SOURCE}
    ${TX_LINUX_THREAD_STACK_BUILD_SOURCE}
    ${TX_LINUX_THREAD_SYSTEM_RETURN_SOURCE}
    ${CMAKE_CURRENT_LIST_DIR}/src/tx_thread_stack_info.cpp

    # {{END_TARGET_SOURCES}}
)

target_include_directories(${PROJECT_NAME}
    PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${THREADX_LINUX_GNU_DIR}/inc
)

target_compile_definitions(${PROJECT_NAME}
    PUBLIC
    _GNU_SOURCE
    TX_LINUX_DEBUG_ENABLE
    TX_TIMER_THREAD_STACK_SIZE=1024
)
