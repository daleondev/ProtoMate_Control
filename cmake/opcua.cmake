include_guard(GLOBAL)
# Isolated vendor configuration; no reflection flags or application dependencies.
set(UA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(UA_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(UA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
if(PROTOMATE_OPCUA_SAMPLE)
    set(UA_NAMESPACE_ZERO MINIMAL CACHE STRING "" FORCE)
else()
    # Structured records and engineering-unit/enum metadata require the standard
    # DataType hierarchy omitted by MINIMAL.
    set(UA_NAMESPACE_ZERO REDUCED CACHE STRING "" FORCE)
endif()
set(UA_MULTITHREADING 0 CACHE STRING "" FORCE)
set(UA_ENABLE_ENCRYPTION OFF CACHE STRING "" FORCE)
set(UA_ENABLE_PUBSUB OFF CACHE BOOL "" FORCE)
set(UA_ENABLE_SUBSCRIPTIONS_EVENTS OFF CACHE BOOL "" FORCE)
set(UA_ENABLE_SUBSCRIPTIONS ON CACHE BOOL "" FORCE)
set(UA_ENABLE_METHODCALLS ON CACHE BOOL "" FORCE)
set(UA_ENABLE_DISCOVERY OFF CACHE BOOL "" FORCE)
# We publish base variables with engineering-unit properties, not AnalogItem,
# DiscreteItem or axis-scale types. Runtime installs the required standard
# EUInformation type/encoding from the SDK descriptor.
set(UA_ENABLE_DA OFF CACHE BOOL "" FORCE)
set(UA_ENABLE_JSON_ENCODING OFF CACHE BOOL "" FORCE)
set(UA_ENABLE_XML_ENCODING OFF CACHE BOOL "" FORCE)
set(UA_ENABLE_NODESET_COMPILER_DESCRIPTIONS OFF CACHE BOOL "" FORCE)
set(UA_ENABLE_MALLOC_SINGLETON ON CACHE BOOL "" FORCE)
set(UA_ENABLE_HISTORIZING OFF CACHE BOOL "" FORCE)
set(UA_ENABLE_DIAGNOSTICS OFF CACHE BOOL "" FORCE)
set(UA_ENABLE_PARSING OFF CACHE BOOL "" FORCE)
set(UA_ENABLE_STATUSCODE_DESCRIPTIONS ON CACHE BOOL "" FORCE)
set(UA_LOGLEVEL 400 CACHE STRING "" FORCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    set(UA_ARCHITECTURE none CACHE STRING "" FORCE)
else()
    set(UA_ARCHITECTURE posix CACHE STRING "" FORCE)
endif()
set(opcua_parent_c_flags "${CMAKE_C_FLAGS}")
block(SCOPE_FOR VARIABLES)
# The application owns its optimization policy; the upstream automatic IPO
# probe otherwise starts a separate bare-metal project without core selection.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
add_subdirectory(${PROJECT_SOURCE_DIR}/external/open62541 ${PROJECT_BINARY_DIR}/open62541 EXCLUDE_FROM_ALL)
endblock()
# Inherited application warnings/reflection do not apply to vendor translation
# units. The upstream directory retains its own C compiler settings.
set_property(TARGET open62541-object open62541-plugins PROPERTY COMPILE_OPTIONS "")
# Upstream caches its warning and C99 flags globally. Keep them vendor-local.
set(CMAKE_C_FLAGS "${opcua_parent_c_flags}" CACHE STRING "C compiler flags" FORCE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ua_node_source ${PROJECT_SOURCE_DIR}/external/open62541/src/server/ua_services_nodemanagement.c)
set(ua_node_fixed ${PROJECT_BINARY_DIR}/open62541/ua_services_nodemanagement.c)
execute_process(COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/src/ui/opcua/util/prepare_nodemanagement.py
    ${ua_node_source} ${ua_node_fixed} COMMAND_ERROR_IS_FATAL ANY)
get_target_property(ua_core_sources open62541-object SOURCES)
list(REMOVE_ITEM ua_core_sources ${ua_node_source})
list(APPEND ua_core_sources ${ua_node_fixed})
set_property(TARGET open62541-object PROPERTY SOURCES ${ua_core_sources})
target_include_directories(open62541-object PRIVATE
    ${PROJECT_SOURCE_DIR}/external/open62541/src
    ${PROJECT_SOURCE_DIR}/external/open62541/src/server
    ${PROJECT_SOURCE_DIR}/external/open62541/src/pubsub)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${ua_node_source} ${PROJECT_SOURCE_DIR}/src/ui/opcua/util/prepare_nodemanagement.py)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(netx_config ${PROJECT_BINARY_DIR}/open62541/ua_config_netx.c)
    execute_process(COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/src/ui/opcua/util/prepare_netx.py
        ${PROJECT_SOURCE_DIR}/external/open62541
        ${PROJECT_BINARY_DIR}/open62541/src_generated ${netx_config}
        COMMAND_ERROR_IS_FATAL ANY)
    get_target_property(ua_plugin_sources open62541-plugins SOURCES)
    list(REMOVE_ITEM ua_plugin_sources ${PROJECT_SOURCE_DIR}/external/open62541/plugins/ua_config_default.c)
    list(APPEND ua_plugin_sources ${netx_config}
        ${PROJECT_SOURCE_DIR}/external/open62541/arch/common/timer.c)
    set_property(TARGET open62541-plugins PROPERTY SOURCES ${ua_plugin_sources})
    # Used exclusively by the POSIX ethernet connection manager; no socket
    # compatibility headers or fake address-family definitions in the NetX port.
    get_target_property(ua_core_sources open62541-object SOURCES)
    list(REMOVE_ITEM ua_core_sources ${PROJECT_SOURCE_DIR}/external/open62541/deps/musl_inet_pton.c)
    set_property(TARGET open62541-object PROPERTY SOURCES ${ua_core_sources})
    foreach(ua_target open62541 open62541-object open62541-plugins)
        target_compile_definitions(${ua_target} PUBLIC UA_ARCHITECTURE_NETX)
        target_include_directories(${ua_target} BEFORE PUBLIC
            $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/open62541/netx-include>)
    endforeach()
    foreach(ua_target open62541-object open62541-plugins)
        target_compile_options(${ua_target} PRIVATE -mno-unaligned-access
            -include ${PROJECT_SOURCE_DIR}/platform/network/ByteAccess.h)
        target_compile_definitions(${ua_target} PRIVATE NETWORK_BYTE_ACCESS_OVERRIDE)
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/src/ui/opcua/util/prepare_netx.py)
endif()
add_subdirectory(${PROJECT_SOURCE_DIR}/src/ui/opcua ${PROJECT_BINARY_DIR}/opcua)
