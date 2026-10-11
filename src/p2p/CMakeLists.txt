add_library(application_p2p STATIC Bridge.cpp InProcessTransport.cpp)
add_library(application::p2p ALIAS application_p2p)
target_include_directories(application_p2p PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(application_p2p PUBLIC pneumo::coroutines)
target_compile_options(application_p2p PRIVATE -Wall -Wextra -Wpedantic)

# The application selects the threading ABI; the transport has no HAL dependency.
if(TARGET runtime::cxx_abi)
    target_link_libraries(application_p2p PUBLIC runtime::cxx_abi)
endif()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(tests)
endif()
