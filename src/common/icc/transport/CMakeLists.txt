add_library(proto_common_icc_transport INTERFACE)
add_library(proto::common::icc::transport ALIAS proto_common_icc_transport)

target_sources(proto_common_icc_transport
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/ITransport.hpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(proto_common_icc_transport
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/InterCoreTransport.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/InterCoreTransport.cpp
    )
else()
    target_sources(proto_common_icc_transport   
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/InProcessTransport.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/InProcessTransport.cpp
    )
endif()

target_link_libraries(proto_common_icc_transport 
    INTERFACE 
        runtime::runtime
)
