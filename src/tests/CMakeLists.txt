add_executable(proto_lifecycle_tests application_test.cpp)
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
