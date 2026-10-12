add_library(proto_common_util INTERFACE)
add_library(proto::common::util ALIAS proto_common_util)

target_sources(proto_common_util
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Completion.hpp
)
