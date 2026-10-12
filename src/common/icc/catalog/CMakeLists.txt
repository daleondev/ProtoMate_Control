add_library(proto_common_icc_catalog INTERFACE)
add_library(proto::common::icc::catalog ALIAS proto_common_icc_catalog)

target_sources(proto_common_icc_catalog
    INTERFACE
        topics.hpp
        services.hpp
        actions.hpp
)
