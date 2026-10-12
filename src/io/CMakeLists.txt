add_library(application_io STATIC Application.cpp Application.hpp system_threads.cpp)
add_library(application::io ALIAS application_io)
target_link_libraries(application_io PUBLIC application::common application::p2p runtime::threads)
