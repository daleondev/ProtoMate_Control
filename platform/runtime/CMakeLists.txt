# HAL factories and application code exchange C++ objects. On Arm, the
# toolchain otherwise defaults shared_ptr to the single-threaded policy in
# HAL and the atomic policy in the ThreadX application. Share headers and
# ABI definitions without making the lower HAL depend on runtime binaries.
add_library(runtime_cxx_abi INTERFACE)
add_library(runtime::cxx_abi ALIAS runtime_cxx_abi)
target_include_directories(runtime_cxx_abi BEFORE INTERFACE
    ${PROJECT_SOURCE_DIR}/platform
    ${PROJECT_SOURCE_DIR}/platform/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/libstdcxx/include
)
target_compile_definitions(runtime_cxx_abi INTERFACE
    THREADX_STD_ENABLED=1
    _GLIBCXX_HAS_GTHREADS=1
    _GLIBCXX_HAVE_POSIX_SEMAPHORE=1
    _GLIBCXX_USE_SCHED_YIELD=1
    $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Generic>:RUNTIME_LIBSTDCXX_HAVE_TLS=1>
)
target_link_libraries(runtime_cxx_abi INTERFACE threadx)

add_library(runtime INTERFACE)
add_library(runtime::runtime ALIAS runtime)

add_subdirectory(threadx)
add_subdirectory(synchronization)
add_subdirectory(libc)
add_subdirectory(libstdcxx)

target_link_libraries(runtime
    INTERFACE
        runtime::synchronization
        runtime::libc
        runtime::libstdcxx
        project_compiler_settings
)

function(runtime_attach target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "Unknown runtime executable target: ${target_name}")
    endif()

    target_sources(${target_name}
        PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/startup.cpp
    )

    target_link_libraries(${target_name}
        PRIVATE
            runtime::runtime
            platform::hal
    )

    target_link_options(${target_name}
        PRIVATE
            LINKER:--wrap=main
    )
endfunction()

runtime_attach(${PROJECT_NAME})

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(tests)
endif()
