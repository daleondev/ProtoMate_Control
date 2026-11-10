cmake_minimum_required(VERSION 3.22)

# This entry point orchestrates two independent compiler configurations.
option(PROTOMATE_PROFILING "Enable bounded ThreadX/application trace recording" OFF)
if(PROTOMATE_PROFILING)
    add_compile_definitions(PROTOMATE_PROFILING=1)
endif()
option(GIGA_BUILD_BOTH "Build both GIGA controller applications" OFF)
option(GIGA_MOTION_BENCH "Build the isolated dual-core motion architecture benchmark" OFF)
option(GIGA_STEP_BENCH "Build the isolated dual-core STEP hardware bench" OFF)
# Preserve existing build-directory settings while migrating the option name.
if(DEFINED GIGA_M4_RAM_DEBUG AND NOT DEFINED GIGA_M4_SDRAM_CODE)
    set(GIGA_M4_SDRAM_CODE "${GIGA_M4_RAM_DEBUG}" CACHE BOOL
        "Copy ordinary M4 application code from QSPI into its reserved SDRAM region")
endif()
option(GIGA_M4_SDRAM_CODE
    "Copy ordinary M4 application code from QSPI into its reserved SDRAM region" OFF)
if(GIGA_MOTION_BENCH AND (GIGA_STEP_BENCH OR GIGA_M4_SDRAM_CODE))
    message(FATAL_ERROR "Motion benchmark requires its own paired preset")
endif()
if(PROTOMATE_PROFILING AND (GIGA_MOTION_BENCH OR GIGA_STEP_BENCH OR GIGA_M4_SDRAM_CODE OR GIGA_MOTION_API_TEST))
    message(FATAL_ERROR "Profiling SDRAM reservations require the dedicated profiling presets")
endif()
if(GIGA_BUILD_BOTH)
    project(Controllers LANGUAGES NONE)
    include(cmake/controllers_giga.cmake)
    return()
endif()

project(Application LANGUAGES C CXX ASM)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16 OR
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 17)
    message(FATAL_ERROR "This project requires GCC 16")
endif()

# Each hardware core has a separate configure tree, image and runtime instance.
if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    if(NOT STM32_CORE MATCHES "^CM[47]$")
        message(FATAL_ERROR "Select an GIGA preset with STM32_CORE=CM4 or CM7")
    endif()
    include(cmake/application_giga.cmake)
    return()
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Unsupported target system: ${CMAKE_SYSTEM_NAME}")
endif()

include(CTest)
# Instrumentation changes host execution cost, not controller timing. Keep
# wall-time watchdogs and performance qualification separate in test harnesses.
set(PROTOMATE_TEST_INSTRUMENTED OFF)
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*(address|undefined)")
    set(PROTOMATE_TEST_INSTRUMENTED ON)
endif()
option(RUNTIME_STORAGE_REQUIRED "Require a mounted FileX volume in host tests" OFF)
add_library(project_compiler_settings INTERFACE)
target_compile_features(project_compiler_settings INTERFACE cxx_std_26)
include(cmake/platform_linux.cmake)

# The ThreadX controller is a private link image embedded into the native
# Application executable. Its C++ runtime must never be linked into the host.
set(PROTOMATE_CONTROLLER_TARGET protomate_controller)
set(PROTOMATE_CONTROLLER_IMAGE ${PROJECT_BINARY_DIR}/CMakeFiles/controller/controller.elf)
add_executable(${PROTOMATE_CONTROLLER_TARGET})
set_target_properties(${PROTOMATE_CONTROLLER_TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/CMakeFiles/controller
    OUTPUT_NAME controller SUFFIX .elf)
target_link_libraries(${PROTOMATE_CONTROLLER_TARGET} PRIVATE platform project_compiler_settings)
target_compile_options(${PROTOMATE_CONTROLLER_TARGET} PRIVATE
    "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")
target_compile_definitions(${PROTOMATE_CONTROLLER_TARGET} PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=8192U)

add_subdirectory(external/pneumo)
add_subdirectory(platform/profiling)
add_subdirectory(platform/hal)
add_subdirectory(platform/runtime)
add_subdirectory(src)
add_subdirectory(src/simulation)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(BUILD_TESTING)
    add_subdirectory(platform/hal/tests)
    add_subdirectory(examples/giga_motion_bench/tests)
    add_test(NAME hal.architecture
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/cmake/tests/verify_hal_architecture.py
            --source ${PROJECT_SOURCE_DIR} --build ${PROJECT_BINARY_DIR} --platform linux)
    add_test(NAME tooling.clang_tidy
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/tests/test_clang_tidy.py)
    add_test(NAME tooling.memory_usage
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/cmake/tests/memory_usage_test.py)
    add_test(NAME tooling.profiling
        COMMAND ${Python3_EXECUTABLE} -m pytest -q ${PROJECT_SOURCE_DIR}/scripts/tests/test_profiling.py)
    set_tests_properties(tooling.profiling PROPERTIES TIMEOUT 30)
    add_test(NAME boot.giga_images
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/tests/test_giga_images.py)
    add_test(NAME boot.giga_mpu
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/tests/test_giga_mpu.py)
endif()

add_custom_target(clang-tidy
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/clang_tidy.py
        --build-dir ${PROJECT_BINARY_DIR}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    USES_TERMINAL VERBATIM COMMENT "clang-tidy")

# Native initialization-policy regression tests use the same pinned toolchain.
add_custom_target(clang-tidy-style-test
    COMMAND bash ${PROJECT_SOURCE_DIR}/scripts/clang-tidy/test-style-check.sh
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    USES_TERMINAL VERBATIM COMMENT "Test native brace-initialization check")
