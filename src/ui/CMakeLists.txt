add_library(application_ui STATIC Application.cpp Application.hpp system_threads.cpp)
add_library(application::ui ALIAS application_ui)
target_link_libraries(application_ui PUBLIC
    application::common application::icc runtime::threads)
