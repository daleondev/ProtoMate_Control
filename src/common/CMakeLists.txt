add_library(application_common INTERFACE)
add_library(application::common ALIAS application_common)
target_sources(application_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Application.hpp)
target_include_directories(application_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(application_common INTERFACE project_compiler_settings)

if(APPLICATION_ENABLE_P2P)
    add_subdirectory(p2p)
endif()
