add_library(runtime_threadx STATIC Support.cpp)
add_library(runtime::threadx ALIAS runtime_threadx)
target_link_libraries(runtime_threadx PUBLIC runtime::cxx_abi project_compiler_settings)
