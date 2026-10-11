add_library(application_p2p STATIC InProcessTransport.cpp util/Core.cpp)
add_library(application::p2p ALIAS application_p2p)
target_include_directories(application_p2p PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(application_p2p PUBLIC pneumo::messaging pneumo::coroutines)
target_compile_options(application_p2p PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(tests)
endif()
# The application selects the C++ threading ABI. This carries headers/ABI flags,
# not a HAL implementation or ownership of an RTOS kernel.
if(TARGET runtime::cxx_abi)
    target_link_libraries(application_p2p PUBLIC runtime::cxx_abi)
endif()
# Keep the communication templates debuggable while fitting the Nucleo flash.
target_compile_options(application_p2p PRIVATE $<$<AND:$<CONFIG:Debug>,$<STREQUAL:${CMAKE_SYSTEM_NAME},Generic>>:-Og>)
