add_library(platform_opcua_memory STATIC Memory.c)
add_library(platform::opcua_memory ALIAS platform_opcua_memory)
set_target_properties(platform_opcua_memory PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(platform_opcua_memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform_opcua_memory PUBLIC open62541::open62541)
target_compile_options(platform_opcua_memory PRIVATE -Wall -Wextra -Wpedantic)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(platform_opcua_posix STATIC Posix.c)
    target_link_libraries(platform_opcua_posix PUBLIC open62541::open62541)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    add_library(platform_opcua_netx STATIC NetX.c Clock.cpp)
    set_target_properties(platform_opcua_netx PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
        CXX_STANDARD 26 CXX_STANDARD_REQUIRED YES)
    target_include_directories(platform_opcua_netx PRIVATE
        ${PROJECT_SOURCE_DIR}/external/open62541/arch/common
        ${PROJECT_SOURCE_DIR}/external/open62541/deps
        $<TARGET_PROPERTY:platform,INTERFACE_INCLUDE_DIRECTORIES>)
    target_link_libraries(platform_opcua_netx PUBLIC open62541::open62541 platform::network)
    target_link_libraries(platform_opcua_netx PRIVATE project_compiler_settings)
endif()
