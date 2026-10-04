add_library(runtime_synchronization STATIC Notification.cpp)
add_library(runtime::synchronization ALIAS runtime_synchronization)
target_link_libraries(runtime_synchronization
    PRIVATE platform::hal runtime::threadx
    PUBLIC runtime::cxx_abi project_compiler_settings
)
