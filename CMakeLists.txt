cmake_minimum_required(VERSION 3.22)

# This entry point orchestrates two independent compiler configurations.
option(H755_BUILD_BOTH "Build both H755 controller applications" OFF)
option(H755_STEP_BENCH "Build the isolated dual-core STEP hardware bench" OFF)
if(H755_BUILD_BOTH)
    project(Controllers LANGUAGES NONE)
    include(cmake/controllers_h755.cmake)
    return()
endif()

project(Application LANGUAGES C CXX ASM)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16 OR
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 17)
    message(FATAL_ERROR "This project requires GCC 16")
endif()

# Portable contracts shared by HAL and application code, without runtime dependencies.
add_subdirectory(platform/communication)

# Each hardware core has a separate configure tree, image and runtime instance.
if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    if(NOT H755_CORE MATCHES "^CM[47]$")
        message(FATAL_ERROR "Select an H755 preset with H755_CORE=CM4 or CM7")
    endif()
    option(H755_LED_SAMPLE "Build the initial bare-metal LED sample" OFF)
    if(H755_LED_SAMPLE)
        add_subdirectory(examples/h755_dual_core)
    else()
        include(cmake/application_h755.cmake)
    endif()
    return()
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Unsupported target system: ${CMAKE_SYSTEM_NAME}")
endif()

include(CTest)
option(RUNTIME_STORAGE_REQUIRED "Require a mounted FileX volume in host tests" OFF)
add_library(project_compiler_settings INTERFACE)
target_compile_features(project_compiler_settings INTERFACE cxx_std_26)
include(cmake/platform_linux.cmake)

add_executable(${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} PRIVATE platform project_compiler_settings)
target_compile_options(${PROJECT_NAME} PRIVATE
    "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")
target_compile_definitions(${PROJECT_NAME} PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=8192U)

add_subdirectory(external/pneumo)
add_subdirectory(platform/hal)
add_subdirectory(platform/runtime)
add_subdirectory(src)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(BUILD_TESTING)
    add_subdirectory(platform/hal/tests)
    add_test(NAME hal.architecture
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/cmake/tests/verify_hal_architecture.py
            --source ${PROJECT_SOURCE_DIR} --build ${PROJECT_BINARY_DIR} --platform linux)
    add_test(NAME tooling.clang_tidy
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/tests/test_clang_tidy.py)
    add_test(NAME tooling.memory_usage
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/cmake/tests/memory_usage_test.py)
endif()

add_custom_target(clang-tidy
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/clang_tidy.py
        --build-dir ${PROJECT_BINARY_DIR}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    USES_TERMINAL VERBATIM COMMENT "clang-tidy")
