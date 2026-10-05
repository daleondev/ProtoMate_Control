cmake_minimum_required(VERSION 3.22)
project(Application LANGUAGES C CXX ASM)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16 OR
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 17)
    message(FATAL_ERROR "This project requires GCC 16")
endif()

add_library(project_compiler_settings INTERFACE)
target_compile_options(project_compiler_settings
    INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-std=c++26>
)

option(ENABLE_CLANG_TIDY "Run clang-tidy while compiling project-owned C++ targets" OFF)
option(RUNTIME_HARDWARE_SELF_TEST "Build the board-resident runtime conformance image" OFF)
option(RUNTIME_HARDWARE_ETHERNET_TEST "Build the Ethernet cable test image" OFF)
option(RUNTIME_HARDWARE_IO_TEST "Build the serial and reset-persistence test image" OFF)
option(RUNTIME_HARDWARE_STORAGE_STARTUP_TEST "Build the storage-startup test image" OFF)
option(HAL_HARDWARE_STEP_TEST "Build the interactive hardware step-generator test image" OFF)
option(HAL_HARDWARE_TMC_TEST "Build the single-driver TMC2209 UART/motor bench test image" OFF)
option(HAL_HARDWARE_TMC_STANDALONE_TEST "Build the TMC2209 STEP/DIR bench image without driver UART" OFF)
option(RUNTIME_STORAGE_REQUIRED "Require at least one mounted FileX volume at startup" OFF)
option(RUNTIME_STORAGE_ERASE_FLASH_ON_BOOT
       "Destructively erase the external NOR flash before mounting (recovery only)"
       OFF)

if((RUNTIME_HARDWARE_SELF_TEST OR RUNTIME_HARDWARE_ETHERNET_TEST OR RUNTIME_HARDWARE_IO_TEST OR RUNTIME_HARDWARE_STORAGE_STARTUP_TEST OR HAL_HARDWARE_STEP_TEST OR HAL_HARDWARE_TMC_TEST OR HAL_HARDWARE_TMC_STANDALONE_TEST) AND
   NOT CMAKE_SYSTEM_NAME STREQUAL "Generic")
    message(FATAL_ERROR "Hardware self-tests are only supported by the STM32 target")
endif()
set(hardware_image_count 0)
foreach(image_option RUNTIME_HARDWARE_SELF_TEST RUNTIME_HARDWARE_ETHERNET_TEST RUNTIME_HARDWARE_IO_TEST RUNTIME_HARDWARE_STORAGE_STARTUP_TEST HAL_HARDWARE_STEP_TEST HAL_HARDWARE_TMC_TEST HAL_HARDWARE_TMC_STANDALONE_TEST)
    if(${image_option})
        math(EXPR hardware_image_count "${hardware_image_count} + 1")
    endif()
endforeach()
if(hardware_image_count GREATER 0)
    # Preserve debugger-visible diagnostics in optimized conformance images.
    target_compile_options(project_compiler_settings INTERFACE $<$<CONFIG:Release>:-g>)
endif()
if(hardware_image_count GREATER 1)
    message(FATAL_ERROR "Select one hardware self-test image at a time")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CTest)
else()
    option(BUILD_TESTING "Build tests" OFF)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(cmake/platform_linux.cmake)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    include(cmake/platform_stm32.cmake)
else()
    message(FATAL_ERROR "Unsupported target system: ${CMAKE_SYSTEM_NAME}")
endif()

add_executable(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        platform
        project_compiler_settings
)

target_compile_options(${PROJECT_NAME}
    PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

if(hardware_image_count GREATER 0)
    target_compile_definitions(${PROJECT_NAME}
        PRIVATE
            RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U
    )
else()
    target_compile_definitions(${PROJECT_NAME}
        PRIVATE
            RUNTIME_APPLICATION_THREAD_STACK_SIZE=8192U
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY
        LINK_DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/external/CubeMX/STM32H753XX_FLASH.ld
    )

    target_link_options(${PROJECT_NAME}
        PRIVATE
            -T${CMAKE_CURRENT_SOURCE_DIR}/external/CubeMX/STM32H753XX_FLASH.ld
            --specs=nosys.specs
            -Wl,-Map=$<TARGET_FILE_DIR:${PROJECT_NAME}>/${PROJECT_NAME}.map
            -Wl,--gc-sections
            -Wl,--print-memory-usage
    )

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex
                $<TARGET_FILE:${PROJECT_NAME}>
                $<TARGET_FILE_DIR:${PROJECT_NAME}>/${PROJECT_NAME}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary
                $<TARGET_FILE:${PROJECT_NAME}>
                $<TARGET_FILE_DIR:${PROJECT_NAME}>/${PROJECT_NAME}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
        COMMENT "Generating hex and bin files, and printing size"
        VERBATIM
    )
endif()

add_subdirectory(external/pneumo)

add_subdirectory(platform/hal)
add_subdirectory(platform/runtime)
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(platform/hal/tests)
endif()

if(HAL_HARDWARE_TMC_STANDALONE_TEST)
    target_sources(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/hal/tests/hardware_tmc_standalone_test.cpp)
elseif(HAL_HARDWARE_TMC_TEST)
    target_sources(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/hal/tests/hardware_tmc_test.cpp)
elseif(HAL_HARDWARE_STEP_TEST)
    target_sources(${PROJECT_NAME}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/hal/tests/hardware_step_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/control/StepperMotor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/control/MotionProfile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/control/MotionSequence.cpp
    )
    set_source_files_properties(src/control/MotionProfile.cpp src/control/MotionSequence.cpp PROPERTIES
        COMPILE_OPTIONS "$<$<CONFIG:Release>:-O3>")
    target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(${PROJECT_NAME} PRIVATE pneumo::pneumo)
elseif(RUNTIME_HARDWARE_STORAGE_STARTUP_TEST)
    target_sources(${PROJECT_NAME}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/runtime/tests/hardware_storage_startup_test.cpp
    )
elseif(RUNTIME_HARDWARE_ETHERNET_TEST)
    target_sources(${PROJECT_NAME}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/hal/tests/hardware_ethernet_test.cpp
    )
elseif(RUNTIME_HARDWARE_IO_TEST)
    target_sources(${PROJECT_NAME}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/runtime/tests/hardware_io_test.cpp
    )
elseif(RUNTIME_HARDWARE_SELF_TEST)
    target_sources(${PROJECT_NAME}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/runtime/tests/hardware_self_test.cpp
    )
else()
    add_subdirectory(src)
endif()

if(ENABLE_CLANG_TIDY)
    # Older LLVM releases cannot parse the supported GCC 16 C++26 headers.
    find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        CXX_CLANG_TIDY
            "${CLANG_TIDY_EXECUTABLE};--line-filter=[{\"name\":\".*/src/.*\"}]"
    )
endif()
