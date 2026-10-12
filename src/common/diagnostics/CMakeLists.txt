add_library(proto_common_diagnostics INTERFACE)
add_library(proto::common::diagnostics ALIAS proto_common_diagnostics)

target_sources(proto_common_diagnostics 
    INTERFACE
        Memory.hpp
        Memory.cpp
)

target_link_libraries(proto_common_diagnostics 
    INTERFACE  
        runtime::cxx_abi
)
