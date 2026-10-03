add_executable(hal_tests)

target_sources(hal_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/clock_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/console_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ethernet_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/gpio_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/motor_peripheral_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pwm_pulse_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rng_test.cpp
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
