add_library(proto_common_motion INTERFACE)
add_library(proto::common::motion ALIAS proto_common_motion)

target_sources(proto_common_motion
    INTERFACE
        Profile.hpp
        Profile.cpp
)
