add_executable(proto_diagnostics_tests diagnostics_test.cpp)
target_link_libraries(proto_diagnostics_tests PRIVATE proto::io proto::ui GTest::gtest_main)
target_compile_definitions(proto_diagnostics_tests PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U)
runtime_attach(proto_diagnostics_tests)
add_test(NAME application.diagnostics COMMAND proto_diagnostics_tests)
set_tests_properties(application.diagnostics PROPERTIES TIMEOUT 30)

add_executable(proto_lifecycle_tests application_test.cpp system_state_test.cpp)
target_link_libraries(proto_lifecycle_tests PRIVATE proto::io proto::ui GTest::gtest_main)
target_compile_definitions(proto_lifecycle_tests PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U)
runtime_attach(proto_lifecycle_tests)
add_test(NAME application.lifecycle COMMAND proto_lifecycle_tests)
set_tests_properties(application.lifecycle PROPERTIES TIMEOUT 30)

add_executable(proto_intercore_tests intercore_test.cpp)
# Exercise the hardware transport against ChannelModel on Linux without adding
# it to the host application's common library.
target_sources(proto_intercore_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/icc/transport/InterCoreTransport.cpp)
target_link_libraries(proto_intercore_tests PRIVATE proto::common GTest::gtest_main)
target_compile_definitions(proto_intercore_tests PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U)
runtime_attach(proto_intercore_tests)
add_test(NAME application.intercore COMMAND proto_intercore_tests)
set_tests_properties(application.intercore PROPERTIES TIMEOUT 30)

add_executable(proto_motion_controller_tests motion_controller_test.cpp)
target_link_libraries(proto_motion_controller_tests PRIVATE proto::io platform::hal GTest::gtest_main)
target_compile_definitions(proto_motion_controller_tests PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U)
runtime_attach(proto_motion_controller_tests)
add_test(NAME application.motion_controller COMMAND proto_motion_controller_tests)
set_tests_properties(application.motion_controller PROPERTIES TIMEOUT 30)

add_executable(proto_logging_tests logging_test.cpp)
target_link_libraries(proto_logging_tests PRIVATE proto::io proto::ui GTest::gtest_main)
target_compile_definitions(proto_logging_tests PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U)
runtime_attach(proto_logging_tests)
add_test(NAME application.logging COMMAND proto_logging_tests)
set_tests_properties(application.logging PROPERTIES TIMEOUT 30)

add_executable(proto_motor_cli_tests motor_cli_test.cpp)
target_link_libraries(proto_motor_cli_tests PRIVATE proto::io proto::ui GTest::gtest_main)
target_compile_definitions(proto_motor_cli_tests PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U)
runtime_attach(proto_motor_cli_tests)
add_test(NAME application.motor_cli COMMAND proto_motor_cli_tests)
set_tests_properties(application.motor_cli PROPERTIES TIMEOUT 45)

add_executable(proto_robot_math_tests axis_conversion_test.cpp scara_kinematics_test.cpp robot_planner_test.cpp)
target_link_libraries(proto_robot_math_tests PRIVATE proto::ui GTest::gtest_main)
target_compile_definitions(proto_robot_math_tests PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=131072U)
runtime_attach(proto_robot_math_tests)
add_test(NAME application.robot_math COMMAND proto_robot_math_tests)
set_tests_properties(application.robot_math PROPERTIES TIMEOUT 30)

add_executable(proto_robot_execution_tests robot_execution_test.cpp)
target_link_libraries(proto_robot_execution_tests PRIVATE proto::io GTest::gtest_main)
target_compile_definitions(proto_robot_execution_tests PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=131072U)
runtime_attach(proto_robot_execution_tests)
add_test(NAME application.robot_execution COMMAND proto_robot_execution_tests)
set_tests_properties(application.robot_execution PROPERTIES TIMEOUT 30)

add_executable(proto_robot_api_tests robot_api_test.cpp)
target_link_libraries(proto_robot_api_tests PRIVATE proto::ui GTest::gtest_main)
target_compile_definitions(proto_robot_api_tests PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=131072U)
runtime_attach(proto_robot_api_tests)
add_test(NAME application.robot_api COMMAND proto_robot_api_tests)
set_tests_properties(application.robot_api PROPERTIES TIMEOUT 45)

# Native, deterministic tests of the production executor and the DMA register
# boundary. No hosted scheduler or wall-clock sleeps in algorithm tests.
add_executable(proto_executor_tests executor_test.cpp ../io/motion/Executor.cpp
    ../io/motion/ExecutorDiagnostic.cpp
    ../io/motion/ProfileSchedule.cpp ../common/motion/Profile.cpp
    ${PROJECT_SOURCE_DIR}/platform/hal/drivers/impl/StepGenerator.cpp
    ${PROJECT_SOURCE_DIR}/platform/hal/drivers/impl/StepOutput.cpp
    ${PROJECT_SOURCE_DIR}/platform/hal/tests/fixtures/SimulatedStepHardware.cpp)
target_include_directories(proto_executor_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/platform)
target_link_libraries(proto_executor_tests PRIVATE hal_register_gtest project_compiler_settings)
add_test(NAME application.executor COMMAND proto_executor_tests)
set_tests_properties(application.executor PROPERTIES TIMEOUT 30)
