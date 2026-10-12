add_library(application_p2p_transport INTERFACE)
add_library(application::p2p::transport ALIAS application_p2p_transport)

target_sources(application_p2p_transport
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/ITransport.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/InProcessTransport.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/InProcessTransport.cpp
)
