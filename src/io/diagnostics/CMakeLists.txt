add_library(proto_io_diagnostics INTERFACE)
add_library(proto::io::diagnostics ALIAS proto_io_diagnostics)

target_sources(proto_io_diagnostics INTERFACE Server.hpp Server.cpp)
target_link_libraries(proto_io_diagnostics INTERFACE runtime::synchronization pneumo::messaging)
