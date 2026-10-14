add_library(proto_common_app INTERFACE)
add_library(proto::common::app ALIAS proto_common_app)

target_sources(proto_common_app
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/State.hpp
)
