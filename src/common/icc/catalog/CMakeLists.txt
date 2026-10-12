add_library(proto_common_icc_catalog INTERFACE)
add_library(proto::common::icc::catalog ALIAS proto_common_icc_catalog)

target_sources(proto_common_icc_catalog
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/topics.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/services.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/actions.hpp
)
