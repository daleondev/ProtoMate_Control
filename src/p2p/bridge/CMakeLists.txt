add_library(application_p2p_bridge INTERFACE)
add_library(application::p2p::bridge ALIAS application_p2p_bridge)

target_sources(application_p2p_bridge
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Bridge.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Bridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/TopicBridge.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/TopicBridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ServiceBridge.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ServiceBridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ActionBridge.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ActionBridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Frame.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/OutgoingWakeup.hpp
)
