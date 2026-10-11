add_subdirectory(bridge)
add_subdirectory(transport)

add_library(application_p2p STATIC)
add_library(application::p2p ALIAS application_p2p)
target_include_directories(application_p2p PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(application_p2p
    PUBLIC
        pneumo::coroutines
    PRIVATE
        application::p2p::bridge
        application::p2p::transport
)
target_compile_options(application_p2p PRIVATE -Wall -Wextra -Wpedantic)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    # Coroutine and messaging templates otherwise exceed H753 flash in Debug.
    # Keep debug information and useful stepping, as for the control endpoints.
    target_compile_options(application_p2p PRIVATE $<$<CONFIG:Debug>:-Og>)
endif()

# The application selects the threading ABI; the transport has no HAL dependency.
if(TARGET runtime::cxx_abi)
    target_link_libraries(application_p2p PUBLIC runtime::cxx_abi)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(examples)
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()
