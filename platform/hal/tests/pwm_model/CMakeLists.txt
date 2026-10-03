# Standalone host tests: no ThreadX or firmware/application dependencies.
cmake_minimum_required(VERSION 3.22)
project(PwmModelTests LANGUAGES CXX)
enable_testing()
find_package(GTest REQUIRED)
get_filename_component(project_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

add_executable(pwm_linux_tests
    ../pwm_pulse_test.cpp
    ${project_root}/platform/hal/drivers/impl/linux/PwmOutput.cpp
)
add_executable(pwm_stm32_tests
    pwm_stm32_test.cpp
    ${project_root}/platform/hal/drivers/impl/stm32/PwmOutput.cpp
)
target_include_directories(pwm_stm32_tests BEFORE PRIVATE fakes)
target_compile_definitions(pwm_stm32_tests PRIVATE HAL_PLATFORM_STM32)
foreach(target pwm_linux_tests pwm_stm32_tests)
    target_include_directories(${target} PRIVATE ${project_root}/platform)
    target_compile_options(${target} PRIVATE -std=c++26 -Wall -Wextra -Wpedantic)
    target_link_libraries(${target} PRIVATE GTest::gtest_main)
    add_test(NAME ${target} COMMAND ${target})
endforeach()
