add_library(application_icc_transport INTERFACE)
add_library(application::icc::transport ALIAS application_icc_transport)

target_sources(application_icc_transport
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/ITransport.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/InProcessTransport.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/InProcessTransport.cpp
)

# Hardware-facing adapter is separate; the portable bridge retains no HAL or
# runtime implementation dependency. Host tests can inject a channel model.
add_library(application_icc_intercore STATIC
    InterCoreTransport.hpp InterCoreTransport.cpp)
add_library(application::icc::intercore ALIAS application_icc_intercore)
target_link_libraries(application_icc_intercore PUBLIC
    application::icc hal::core runtime::runtime)
