add_library(proto_io_logging INTERFACE)
add_library(proto::io::logging ALIAS proto_io_logging)

target_sources(proto_io_logging 
    INTERFACE 
        Sink.hpp 
        Sink.cpp
) 
