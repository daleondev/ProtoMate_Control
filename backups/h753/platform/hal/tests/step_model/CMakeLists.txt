cmake_minimum_required(VERSION 3.22)
project(StepGeneratorTests LANGUAGES CXX)
enable_testing()
find_package(GTest REQUIRED)
get_filename_component(project_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
add_executable(step_generator_tests
    step_generator_test.cpp
    ../clock_reference_test.cpp
    ${project_root}/platform/hal/drivers/impl/StepGenerator.cpp
    ${project_root}/platform/hal/drivers/impl/StepOutput.cpp
    ${project_root}/platform/hal/drivers/impl/linux/SimulatedStepHardware.cpp
)
target_include_directories(step_generator_tests PRIVATE ${project_root}/platform)
target_compile_options(step_generator_tests PRIVATE -std=c++26 -Wall -Wextra -Wpedantic)
target_link_libraries(step_generator_tests PRIVATE GTest::gtest_main)
add_test(NAME step_generator COMMAND step_generator_tests)

add_executable(step_stm32_tests
    step_stm32_test.cpp
    ${project_root}/platform/hal/drivers/impl/StepGenerator.cpp
    ${project_root}/platform/hal/drivers/impl/StepOutput.cpp
    ${project_root}/platform/hal/drivers/impl/stm32/StepHardware.cpp
    ${project_root}/platform/hal/stm32/FaultShutdown.cpp
)
target_include_directories(step_stm32_tests BEFORE PRIVATE fakes ${project_root}/platform)
target_compile_definitions(step_stm32_tests PRIVATE HAL_PLATFORM_STM32)
target_compile_options(step_stm32_tests PRIVATE -std=c++26 -Wall -Wextra -Wpedantic)
target_link_libraries(step_stm32_tests PRIVATE GTest::gtest_main)
add_test(NAME step_stm32 COMMAND step_stm32_tests)
