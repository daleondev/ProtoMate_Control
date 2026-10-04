if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(application_control_tests
        stepper_motor_test.cpp
        axis_conversion_test.cpp
        scara_kinematics_test.cpp
        robot_test.cpp
        motion_units_test.cpp
        motion_profile_test.cpp)
    target_link_libraries(application_control_tests PRIVATE
        application::control pneumo::pneumo platform::hal GTest::gtest_main)
    target_compile_definitions(application_control_tests PRIVATE
        RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U)
    runtime_attach(application_control_tests)
    add_test(NAME application.control COMMAND application_control_tests --gtest_color=no)
    set_tests_properties(application.control PROPERTIES TIMEOUT 45)
endif()
