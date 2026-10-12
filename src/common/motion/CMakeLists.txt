add_library(proto_common_motion INTERFACE)
add_library(proto::common::motion ALIAS proto_common_motion)

target_sources(proto_common_motion
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Profile.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Profile.cpp
)
