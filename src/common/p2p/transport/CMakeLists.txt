add_library(application_p2p_transport INTERFACE)
add_library(application::p2p::transport ALIAS application_p2p_transport)

target_sources(application_p2p_transport
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/ITransport.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/InProcessTransport.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/InProcessTransport.cpp
)

# Hardware-facing adapter is separate; the portable bridge retains no HAL or
# runtime implementation dependency. Host tests can inject a channel model.
add_library(application_p2p_intercore STATIC
    InterCoreTransport.hpp InterCoreTransport.cpp)
add_library(application::p2p::intercore ALIAS application_p2p_intercore)
target_link_libraries(application_p2p_intercore PUBLIC
    application::p2p hal::core runtime::runtime)
