if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(application_cli_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/filesystem_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/parser_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/utilities_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/motion_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/axis_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/robot_test.cpp
    )

    target_link_libraries(application_cli_tests
        PRIVATE
            application::cli
            application::control
            GTest::gtest_main
    )

    target_compile_options(application_cli_tests
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_definitions(application_cli_tests
        PRIVATE
            RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U
    )

    runtime_attach(application_cli_tests)

    add_test(NAME application.cli COMMAND application_cli_tests --gtest_color=no)
    set_tests_properties(application.cli PROPERTIES TIMEOUT 30)
endif()
