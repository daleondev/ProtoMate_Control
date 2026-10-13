add_executable(hal_tests)

target_sources(hal_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/clock_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/display_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/bus_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/clock_reference_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/console_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ethernet_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/gpio_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/motor_peripheral_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/motor_bench_commands_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pwm_pulse_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rng_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tmc2209_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lan9253_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stepper_driver_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/index_feedback_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/motor_feedback_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/step_model/step_generator_test.cpp
)

target_link_libraries(hal_tests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(hal_tests
    PRIVATE
        RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U
)

target_compile_options(hal_tests
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

runtime_attach(hal_tests)

add_test(NAME hal COMMAND hal_tests --gtest_color=no)
set_tests_properties(hal PROPERTIES TIMEOUT 30)

# Compile the production STM32 STEP backend against a register model. This
# target uses the native host ABI and does not attach the ThreadX runtime.
# The main project's gtest targets use the ThreadX ABI, so build a native copy
# from the same already-fetched sources for these isolated register tests.
find_package(Threads REQUIRED)
FetchContent_GetProperties(googletest)
add_library(hal_register_gtest STATIC
    ${googletest_SOURCE_DIR}/googletest/src/gtest-all.cc
    ${googletest_SOURCE_DIR}/googletest/src/gtest_main.cc)
target_include_directories(hal_register_gtest PUBLIC ${googletest_SOURCE_DIR}/googletest/include
    PRIVATE ${googletest_SOURCE_DIR}/googletest)
target_link_libraries(hal_register_gtest PRIVATE Threads::Threads)
add_executable(step_stm32_tests
    step_model/step_stm32_test.cpp
    ../drivers/impl/StepGenerator.cpp
    ../drivers/impl/StepOutput.cpp
    ../drivers/impl/stm32/StepHardware.cpp
    ../stm32/FaultShutdown.cpp)
target_include_directories(step_stm32_tests BEFORE PRIVATE
    step_model/fakes ${PROJECT_SOURCE_DIR}/platform)
target_compile_definitions(step_stm32_tests PRIVATE HAL_PLATFORM_STM32)
target_link_libraries(step_stm32_tests PRIVATE hal_register_gtest project_compiler_settings)
add_test(NAME step_stm32 COMMAND step_stm32_tests)
set_tests_properties(step_stm32 PROPERTIES TIMEOUT 30)

add_executable(encoder_stm32_tests
    encoder_model/encoder_stm32_test.cpp ../drivers/impl/stm32/QuadratureEncoder.cpp)
target_include_directories(encoder_stm32_tests BEFORE PRIVATE
    encoder_model/fakes ${PROJECT_SOURCE_DIR}/platform)
target_compile_definitions(encoder_stm32_tests PRIVATE HAL_PLATFORM_STM32)
target_link_libraries(encoder_stm32_tests PRIVATE hal_register_gtest project_compiler_settings)
add_test(NAME encoder_stm32 COMMAND encoder_stm32_tests)
set_tests_properties(encoder_stm32 PROPERTIES TIMEOUT 10)

add_executable(usb_console_tests usb_model/usb_console_test.cpp ../stm32/console.cpp)
target_include_directories(usb_console_tests BEFORE PRIVATE
    usb_model/fakes ${PROJECT_SOURCE_DIR}/platform)
target_compile_definitions(usb_console_tests PRIVATE HAL_PLATFORM_STM32 CORE_CM7)
target_link_libraries(usb_console_tests PRIVATE hal_register_gtest project_compiler_settings)
add_test(NAME usb_console COMMAND usb_console_tests)
set_tests_properties(usb_console PROPERTIES TIMEOUT 10)
