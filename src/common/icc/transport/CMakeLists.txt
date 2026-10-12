add_library(proto_common_icc_transport INTERFACE)
add_library(proto::common::icc::transport ALIAS proto_common_icc_transport)

target_sources(proto_common_icc_transport
    INTERFACE
        ITransport.hpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(proto_common_icc_transport
        INTERFACE
            InterCoreTransport.hpp
            InterCoreTransport.cpp
    )
else()
    target_sources(proto_common_icc_transport   
        INTERFACE
            InProcessTransport.hpp
            InProcessTransport.cpp
    )
endif()

target_link_libraries(proto_common_icc_transport 
    INTERFACE 
        runtime::runtime
)
