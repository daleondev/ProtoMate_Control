add_library(proto_common_icc_bridge INTERFACE)
add_library(proto::common::icc::bridge ALIAS proto_common_icc_bridge)

target_sources(proto_common_icc_bridge
    INTERFACE
        Catalog.hpp
        Bridge.hpp
        Bridge.cpp
        ConnectionHealth.hpp
        ConnectionHealth.cpp
        TopicBridge.hpp
        TopicBridge.cpp
        ServiceBridge.hpp
        ServiceBridge.cpp
        ActionBridge.hpp
        ActionBridge.cpp
        Frame.hpp
        Frame.cpp
        OutgoingWakeup.hpp
)
