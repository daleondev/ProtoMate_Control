add_library(application_io STATIC Application.cpp Application.hpp system_threads.cpp)
add_library(application::io ALIAS application_io)
target_link_libraries(application_io PUBLIC application::common runtime::threads)

if(APPLICATION_ENABLE_P2P)
    target_link_libraries(application_io PUBLIC application::p2p)
    target_compile_definitions(application_io PUBLIC APPLICATION_ENABLE_P2P)
endif()
