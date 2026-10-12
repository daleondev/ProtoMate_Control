add_library(proto_io_app INTERFACE)
add_library(proto::io::app ALIAS proto_io_app)

target_sources(proto_io_app
    INTERFACE
        Application.hpp
        Application.cpp 
)

target_link_libraries(proto_io_app 
    INTERFACE
        runtime::threads
)
