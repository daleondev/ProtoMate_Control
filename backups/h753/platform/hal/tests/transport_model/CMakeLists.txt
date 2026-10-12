cmake_minimum_required(VERSION 3.22)
project(HalTransportTests LANGUAGES CXX)
enable_testing()
find_package(GTest REQUIRED)
get_filename_component(project_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

include(${project_root}/cmake/hal_serial_instances.cmake)
hal_generate_serial_instances(${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_CURRENT_SOURCE_DIR}/fakes)

# Exercise production polling drivers with native GoogleTest, independently of
# the main project's ThreadX C++ runtime and Linux HAL implementations.
add_executable(hal_transport_tests
    transport_stm32_test.cpp
    ../../drivers/impl/stm32/Spi.cpp
    ../../drivers/impl/stm32/Uart.cpp
    ../../drivers/factory/stm32/spi.cpp
    ../../drivers/factory/stm32/uart.cpp
)
target_include_directories(hal_transport_tests BEFORE PRIVATE fakes ${project_root}/platform ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(hal_transport_tests PRIVATE GTest::gtest_main)
target_compile_options(hal_transport_tests PRIVATE -std=c++26 -Wall -Wextra -Wpedantic)
add_test(NAME hal.transport COMMAND hal_transport_tests)
set_tests_properties(hal.transport PROPERTIES TIMEOUT 10)
