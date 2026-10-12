add_library(proto_common_util INTERFACE)
add_library(proto::common::util ALIAS proto_common_util)

target_sources(proto_common_util
    INTERFACE
        Completion.hpp
)
