add_library(platform_communication INTERFACE)
add_library(platform::communication ALIAS platform_communication)

target_sources(platform_communication INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Lanes.hpp)
target_include_directories(platform_communication INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
