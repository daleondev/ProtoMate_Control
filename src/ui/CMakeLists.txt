add_library(application_ui STATIC Application.cpp Application.hpp system_threads.cpp)
add_library(application::ui ALIAS application_ui)
target_link_libraries(application_ui PUBLIC application::common runtime::threads)

if(APPLICATION_ENABLE_P2P)
    target_link_libraries(application_ui PUBLIC application::p2p)
    target_compile_definitions(application_ui PUBLIC APPLICATION_ENABLE_P2P)
endif()
