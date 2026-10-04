include(FetchContent)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_disable_pthreads ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    # GoogleTest v1.17.0, pinned for reproducible builds.
    URL https://github.com/google/googletest/archive/52eb8108c5bdec04579160ae17225d66034bd723.tar.gz
    URL_HASH SHA256=745c55415660044610f7fcd3af7a6420d5de16a7dbb9ebfe2e131275676232be
)
FetchContent_MakeAvailable(googletest)

# GoogleTest is part of the test executable and must see the same libstdc++
# replacement headers and gthread ABI as the code under test.
target_link_libraries(gtest PRIVATE runtime::runtime)
target_link_libraries(gtest_main PRIVATE runtime::runtime)

add_executable(runtime_tests)

target_sources(runtime_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libc_filex_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/libc_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/libstdcxx_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/notification_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/storage_media_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/filex_patch_test.cpp
)

target_link_libraries(runtime_tests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(runtime_tests
    PRIVATE
        RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U
)

target_compile_options(runtime_tests
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

runtime_attach(runtime_tests)

add_test(NAME runtime COMMAND runtime_tests --gtest_color=no)
set_tests_properties(runtime PROPERTIES TIMEOUT 30)

add_executable(runtime_storage_failure_probe storage_failure_probe.cpp)
target_compile_definitions(runtime_storage_failure_probe PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U)
runtime_attach(runtime_storage_failure_probe)
add_test(NAME runtime.storage.corrupt_images COMMAND runtime_storage_failure_probe)
set_tests_properties(runtime.storage.corrupt_images PROPERTIES TIMEOUT 30)

add_executable(runtime_terminate_probe
    ${CMAKE_CURRENT_SOURCE_DIR}/terminate_probe.cpp
)
target_compile_definitions(runtime_terminate_probe
    PRIVATE
        RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U
)
runtime_attach(runtime_terminate_probe)

add_executable(runtime_static_terminate_probe
    ${CMAKE_CURRENT_SOURCE_DIR}/static_terminate_probe.cpp
)
target_compile_definitions(runtime_static_terminate_probe
    PRIVATE
        RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U
)
runtime_attach(runtime_static_terminate_probe)

add_executable(runtime_panic_override_probe
    ${CMAKE_CURRENT_SOURCE_DIR}/panic_override_probe.cpp
)
target_compile_definitions(runtime_panic_override_probe
    PRIVATE
        RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U
)
runtime_attach(runtime_panic_override_probe)

add_test(
    NAME runtime.terminate.application
    COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:runtime_terminate_probe>
            -DPROBE_MODE=application
            "-DEXPECTED_PANIC=Unhandled C++ exception: uncaught application exception"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_terminate.cmake
)
add_test(
    NAME runtime.terminate.thread
    COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:runtime_terminate_probe>
            -DPROBE_MODE=thread
            "-DEXPECTED_PANIC=Unhandled C++ exception: uncaught thread exception"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_terminate.cmake
)
add_test(
    NAME runtime.terminate.static_initialization
    COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:runtime_static_terminate_probe>
            "-DEXPECTED_PANIC=Unhandled C++ exception: uncaught static initialization exception"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_terminate.cmake
)
add_test(
    NAME runtime.terminate.non_std_exception
    COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:runtime_terminate_probe>
            -DPROBE_MODE=non_std
            "-DEXPECTED_PANIC=Unhandled non-std C++ exception"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_terminate.cmake
)
add_test(
    NAME runtime.terminate.explicit
    COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:runtime_terminate_probe>
            -DPROBE_MODE=explicit
            "-DEXPECTED_PANIC=Runtime terminated without an active exception"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_terminate.cmake
)
add_test(
    NAME runtime.terminate.bad_alloc
    COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:runtime_terminate_probe>
            -DPROBE_MODE=bad_alloc
            "-DEXPECTED_PANIC=Unhandled C++ exception: std::bad_alloc"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_terminate.cmake
)
add_test(
    NAME runtime.panic.error_handler
    COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:runtime_terminate_probe>
            -DPROBE_MODE=error_handler
            "-DEXPECTED_PANIC=HAL Error_Handler invoked"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_terminate.cmake
)
add_test(
    NAME runtime.panic.fault_handler
    COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:runtime_terminate_probe>
            -DPROBE_MODE=fault_handler
            "-DEXPECTED_PANIC=HardFault"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_terminate.cmake
)
add_test(
    NAME runtime.panic.override
    COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:runtime_panic_override_probe>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_panic_override.cmake
)
set_tests_properties(
    runtime.terminate.application
    runtime.terminate.thread
    runtime.terminate.static_initialization
    runtime.terminate.non_std_exception
    runtime.terminate.explicit
    runtime.terminate.bad_alloc
    runtime.panic.error_handler
    runtime.panic.fault_handler
    runtime.panic.override
    PROPERTIES TIMEOUT 10
)

# Each startup scenario runs in a separate ThreadX process.
add_executable(runtime_storage_startup_probe storage_startup_probe.cpp)
target_compile_definitions(runtime_storage_startup_probe PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U)
runtime_attach(runtime_storage_startup_probe)
foreach(scenario both flash sd none corrupt flash-corrupt-sd sd-corrupt-flash warning-failure)
    set(expected_exit 0)
    if(RUNTIME_STORAGE_REQUIRED AND scenario MATCHES "^(none|corrupt|warning-failure)$")
        set(expected_exit 42)
    endif()
    add_test(NAME runtime.storage.startup.${scenario}
        COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:runtime_storage_startup_probe>
            -DSCENARIO=${scenario} -DEXPECTED_EXIT=${expected_exit}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_storage_startup.cmake)
    set_tests_properties(runtime.storage.startup.${scenario} PROPERTIES TIMEOUT 35)
endforeach()
