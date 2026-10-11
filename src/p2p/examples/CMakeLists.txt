add_executable(application_p2p_example topics.cpp)
target_link_libraries(application_p2p_example PRIVATE application::p2p)
target_compile_options(application_p2p_example PRIVATE -Wall -Wextra -Wpedantic)

if(COMMAND runtime_attach)
    target_compile_definitions(application_p2p_example PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=65536U)
    runtime_attach(application_p2p_example)
endif()

if(BUILD_TESTING)
    add_test(NAME application.p2p.example COMMAND application_p2p_example)
    set_tests_properties(application.p2p.example PROPERTIES TIMEOUT 10)
endif()
